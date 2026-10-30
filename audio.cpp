#include "audio.hpp"
#include <alsa/asoundlib.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

static void silent_alsa_handler(const char*, int, const char*, int, const char*, ...) {}

AudioEngine::AudioEngine() {
    snd_lib_error_set_handler(silent_alsa_handler);
    snd_pcm_t* p = nullptr;
    int err = snd_pcm_open(&p, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        pcm = nullptr;
    } else {
        snd_pcm_set_params(p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 150000);
        pcm = p;
    }
    worker = std::thread(&AudioEngine::run, this);
}

AudioEngine::~AudioEngine() {
    stop_requested = true;
    abort_decode = true;
    is_playing = false;
    if (pcm) snd_pcm_drop((snd_pcm_t*)pcm);
    if (worker.joinable()) worker.join();
    if (pcm) snd_pcm_close((snd_pcm_t*)pcm);
}

void AudioEngine::load(const std::string& path) {
    std::lock_guard<std::mutex> lk(cmd_mutex);
    queued_path = path;
    track_switch = true;
}

void AudioEngine::toggle_pause() { if (is_playing) is_paused = !is_paused; }
void AudioEngine::toggle_mute() { is_muted = !is_muted; }
void AudioEngine::toggle_reverse() { is_reverse = !is_reverse; }

void AudioEngine::stop() {
    is_playing = false;
    is_paused = false;
    abort_decode = true;
    ++cover_gen;

    if (pcm) {
        snd_pcm_t* p = (snd_pcm_t*)pcm;
        snd_pcm_drop(p);
    }

    {
        std::lock_guard<std::mutex> lk(pcm_mutex);
        std::vector<int16_t>().swap(pcm_data);
    }
    total_frames = 0;

    {
        std::lock_guard<std::mutex> lk(meta_mutex);
        active_meta = TrackMetadata{};
        last_played_path.clear();
        metadata_updated = true;
    }
    track_finished = false;
}

void AudioEngine::seek_relative(double seconds) {
    if (is_playing) {
        double target = cur_pts.load() + seconds;
        seek_req = std::clamp(target, 0.0, duration.load());
    }
}

void AudioEngine::seek_absolute_percent(double ratio) {
    if (is_playing && duration.load() > 0.0) {
        seek_req = std::clamp(ratio, 0.0, 1.0) * duration.load();
    }
}

void AudioEngine::reset_modifiers() {
    volume = 1.0;
    speed = 1.0;
    is_muted = false;
    is_reverse = false;
    
    // Reset A-B Loop
    ab_a = -1.0;
    ab_b = -1.0;
}

void AudioEngine::extract_album_art_async(const std::string& path, int target_w, int target_h, uint64_t my_gen) {
    AVFormatContext* fctx = nullptr;
    if (avformat_open_input(&fctx, path.c_str(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(fctx, nullptr) < 0) {
        avformat_close_input(&fctx);
        return;
    }

    std::vector<uint8_t> rgb; // Raw RGB data
    bool has_cover = false;
    int cover_out_w = 0, cover_out_h = 0;

    for (unsigned int i = 0; i < fctx->nb_streams; ++i) {
        if (!(fctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        AVPacket pkt = fctx->streams[i]->attached_pic;
        const AVCodec* c = avcodec_find_decoder(fctx->streams[i]->codecpar->codec_id);
        if (!c) break;

        AVCodecContext* cctx = avcodec_alloc_context3(c);
        if (!cctx) break;
        avcodec_parameters_to_context(cctx, fctx->streams[i]->codecpar);
        if (avcodec_open2(cctx, c, nullptr) == 0) {
            if (avcodec_send_packet(cctx, &pkt) == 0) {
                AVFrame* vframe = av_frame_alloc();
                if (vframe && avcodec_receive_frame(cctx, vframe) == 0) {
                    const int src_w = vframe->width;
                    const int src_h = vframe->height;
                    double scale = std::min(static_cast<double>(target_w) / std::max(1, src_w),
                                            static_cast<double>(target_h) / std::max(1, src_h));
                    int out_w = std::max(1, static_cast<int>(src_w * scale)) & ~1;
                    int out_h = std::max(1, static_cast<int>(src_h * scale)) & ~1;
                    if (out_w < 2) out_w = 2;
                    if (out_h < 2) out_h = 2;

                    SwsContext* sws = sws_getContext(src_w, src_h, static_cast<AVPixelFormat>(vframe->format),
                                                     out_w, out_h, AV_PIX_FMT_RGB24,
                                                     SWS_LANCZOS, nullptr, nullptr, nullptr);
                    if (sws) {
                        rgb.assign(out_w * out_h * 3, 0);
                        uint8_t* dst[1] = { rgb.data() };
                        int strides[1] = { out_w * 3 };
                        sws_scale(sws, vframe->data, vframe->linesize, 0, src_h, dst, strides);
                        sws_freeContext(sws);
                        has_cover = true;
                        cover_out_w = out_w;
                        cover_out_h = out_h;
                    }
                }
                if (vframe) av_frame_free(&vframe);
            }
        }
        avcodec_free_context(&cctx);
        break;
    }
    avformat_close_input(&fctx);

    if (my_gen != cover_gen.load()) return;

    std::lock_guard<std::mutex> lk(meta_mutex);
    if (last_played_path != path) return;
    
    active_meta.cover_rgb = std::move(rgb); // Store raw pixels
    active_meta.has_cover = has_cover;
    active_meta.cover_w = cover_out_w;
    active_meta.cover_h = cover_out_h;
    metadata_updated = true;
}

void AudioEngine::fill_pcm_buffer(const std::string& path) {
    AVFormatContext* fctx = nullptr;
    if (avformat_open_input(&fctx, path.c_str(), nullptr, nullptr) < 0) { decode_complete = true; return; }
    if (avformat_find_stream_info(fctx, nullptr) < 0) { avformat_close_input(&fctx); decode_complete = true; return; }

    int audio_idx = -1;
    for (unsigned int i = 0; i < fctx->nb_streams; ++i) {
        if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { audio_idx = i; break; }
    }
    if (audio_idx == -1) { avformat_close_input(&fctx); decode_complete = true; return; }

    AVCodecParameters* par = fctx->streams[audio_idx]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { avformat_close_input(&fctx); decode_complete = true; return; }

    AVCodecContext* cctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cctx, par);
    if (avcodec_open2(cctx, dec, nullptr) < 0) {
        avcodec_free_context(&cctx); avformat_close_input(&fctx); decode_complete = true; return;
    }

    const int out_rate = 44100;
    SwrContext* swr = swr_alloc();
    av_opt_set_chlayout(swr, "in_chlayout", &cctx->ch_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", cctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", cctx->sample_fmt, 0);
    AVChannelLayout out_ch; av_channel_layout_default(&out_ch, 2);
    av_opt_set_chlayout(swr, "out_chlayout", &out_ch, 0);
    av_opt_set_int(swr, "out_sample_rate", out_rate, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swr);
    av_channel_layout_uninit(&out_ch);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    std::vector<int16_t> temp_buf(out_rate * 2);
    size_t temp_count = 0;
    const size_t MAX_DECODE_FRAMES = 44100ULL * 7200ULL;

    while (!abort_decode && !stop_requested) {
        if (total_frames.load() >= MAX_DECODE_FRAMES) break;
        if (av_read_frame(fctx, pkt) < 0) break;

        if (pkt->stream_index == audio_idx) {
            if (avcodec_send_packet(cctx, pkt) == 0) {
                while (avcodec_receive_frame(cctx, frm) == 0) {
                    int samples = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) + frm->nb_samples,
                                                 out_rate, cctx->sample_rate, AV_ROUND_UP);
                    if (samples > 0) {
                        if (temp_count + samples * 2 > temp_buf.size()) {
                            temp_buf.resize(temp_count + samples * 2 + out_rate);
                        }
                        uint8_t* out_ptrs[1] = { reinterpret_cast<uint8_t*>(temp_buf.data() + temp_count) };
                        int converted = swr_convert(swr, out_ptrs, samples,
                                                    const_cast<const uint8_t**>(frm->extended_data), frm->nb_samples);
                        if (converted > 0) temp_count += converted * 2;
                    }
                    if (temp_count >= 16384) {
                        std::lock_guard<std::mutex> lk(pcm_mutex);
                        pcm_data.insert(pcm_data.end(), temp_buf.begin(), temp_buf.begin() + temp_count);
                        total_frames = pcm_data.size() / 2;
                        temp_count = 0;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (!abort_decode && !stop_requested) {
        avcodec_send_packet(cctx, nullptr);
        while (avcodec_receive_frame(cctx, frm) == 0) {
            int samples = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) + frm->nb_samples,
                                         out_rate, cctx->sample_rate, AV_ROUND_UP);
            if (samples > 0) {
                if (temp_count + samples * 2 > temp_buf.size()) {
                    temp_buf.resize(temp_count + samples * 2 + out_rate);
                }
                uint8_t* out_ptrs[1] = { reinterpret_cast<uint8_t*>(temp_buf.data() + temp_count) };
                int converted = swr_convert(swr, out_ptrs, samples,
                                            const_cast<const uint8_t**>(frm->extended_data), frm->nb_samples);
                if (converted > 0) temp_count += converted * 2;
            }
        }
    }

    if (temp_count > 0) {
        std::lock_guard<std::mutex> lk(pcm_mutex);
        pcm_data.insert(pcm_data.end(), temp_buf.begin(), temp_buf.begin() + temp_count);
        total_frames = pcm_data.size() / 2;
    }

    av_frame_free(&frm); av_packet_free(&pkt);
    swr_free(&swr); avcodec_free_context(&cctx); avformat_close_input(&fctx);
    decode_complete = true;
}

void AudioEngine::run() {
    while (!stop_requested) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(cmd_mutex);
            if (track_switch) { path = queued_path; track_switch = false; }
        }
        if (path.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        decode_loop(path);
    }
}

void AudioEngine::decode_loop(const std::string& path) {
    AVFormatContext* fctx = nullptr;
    if (avformat_open_input(&fctx, path.c_str(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(fctx, nullptr) < 0) { avformat_close_input(&fctx); return; }

    int audio_idx = -1;
    for (unsigned int i = 0; i < fctx->nb_streams; ++i) {
        if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { audio_idx = i; break; }
    }
    if (audio_idx == -1) { avformat_close_input(&fctx); return; }

    AVCodecParameters* par = fctx->streams[audio_idx]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    AVCodecContext* cctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cctx, par);
    if (avcodec_open2(cctx, dec, nullptr) < 0) {
        avcodec_free_context(&cctx); avformat_close_input(&fctx); return;
    }

    TrackMetadata local_meta;
    AVDictionaryEntry* tag = nullptr;
    if ((tag = av_dict_get(fctx->metadata, "title", nullptr, 0))) local_meta.title = tag->value;
    if ((tag = av_dict_get(fctx->metadata, "artist", nullptr, 0))) local_meta.artist = tag->value;
    if ((tag = av_dict_get(fctx->metadata, "album", nullptr, 0))) local_meta.album = tag->value;
    local_meta.codec_name = dec->name;
    local_meta.bit_rate = fctx->bit_rate;
    local_meta.sample_rate = cctx->sample_rate;
    local_meta.channels = cctx->ch_layout.nb_channels;

    {
        std::lock_guard<std::mutex> lk(meta_mutex);
        last_played_path = path;
        active_meta = local_meta;
        metadata_updated = true;
    }

    duration = (fctx->duration != AV_NOPTS_VALUE) ? static_cast<double>(fctx->duration) / AV_TIME_BASE : 0.0;

    uint64_t my_gen = ++cover_gen;
    int tw = std::clamp(cover_target_w.load(), 48, 400);
    int th = std::clamp(cover_target_h.load(), 48, 400);

    std::thread([this, path, my_gen, tw, th]() {
        extract_album_art_async(path, tw, th, my_gen);
    }).detach();

    avcodec_free_context(&cctx);
    avformat_close_input(&fctx);

    abort_decode = false;
    decode_complete = false;
    total_frames = 0;
    { std::lock_guard<std::mutex> lk(pcm_mutex); pcm_data.clear(); }

    std::thread dec_th(&AudioEngine::fill_pcm_buffer, this, path);

    while (!decode_complete && total_frames.load() < 4096 && !abort_decode && !stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Reset ALSA device if it was left in SETUP state
    if (pcm) snd_pcm_prepare((snd_pcm_t*)pcm);

    is_playing = true;
    is_paused = false;

    double play_pos = 0.0;
    if (is_reverse.load()) {
        play_pos = total_frames.load() > 0 ? static_cast<double>(total_frames.load() - 1) : 0.0;
    }

    const int CHUNK_FRAMES = 1024;
    std::vector<int16_t> alsa_out(CHUNK_FRAMES * 2);
    bool natural_eof = false;

    while (is_playing && !stop_requested) {
        {
            std::lock_guard<std::mutex> lk(cmd_mutex);
            if (track_switch) { natural_eof = false; break; }
        }

        if (is_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        double s = seek_req.exchange(-1.0);
        if (s >= 0.0) {
            play_pos = std::clamp(s * 44100.0, 0.0, static_cast<double>(total_frames.load()));
        }

        bool rev = is_reverse.load();
        double spd = std::clamp(speed.load(), 0.10, 3.0);
        double vol = is_muted.load() ? 0.0 : volume.load();

        size_t tf = total_frames.load();
        
        // A-B Loop Logic
        double a = ab_a.load();
        double b = ab_b.load();
        if (!rev && a >= 0.0 && b > 0.0) {
            if (play_pos >= b * 44100.0) {
                play_pos = a * 44100.0;
            }
        } else if (rev && a >= 0.0 && b > 0.0) {
            if (play_pos <= a * 44100.0) {
                play_pos = b * 44100.0;
            }
        }

        if (!rev) {
            if (play_pos >= tf) {
                if (decode_complete) { natural_eof = true; break; }
                else { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            }
        } else {
            if (play_pos <= 0.0) { natural_eof = true; break; }
        }

        int frames_to_render = CHUNK_FRAMES;
        int rendered = 0;

        {
            std::lock_guard<std::mutex> lk(pcm_mutex);
            size_t cur_tf = pcm_data.size() / 2;
            if (cur_tf == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            for (int i = 0; i < frames_to_render; ++i) {
                if (!rev && play_pos >= cur_tf) { if (decode_complete) natural_eof = true; break; }
                if (rev && play_pos < 0.0) { natural_eof = true; break; }

                double f = play_pos;
                int64_t i0 = static_cast<int64_t>(std::floor(f));
                if (i0 < 0) i0 = 0;
                if (static_cast<size_t>(i0) >= cur_tf) i0 = cur_tf - 1;
                int64_t i1 = (static_cast<size_t>(i0 + 1) < cur_tf) ? i0 + 1 : i0;
                double frac = f - std::floor(f);

                int32_t l0 = pcm_data[i0 * 2], r0 = pcm_data[i0 * 2 + 1];
                int32_t l1 = pcm_data[i1 * 2], r1 = pcm_data[i1 * 2 + 1];
                int32_t l = static_cast<int32_t>(l0 + (l1 - l0) * frac);
                int32_t r = static_cast<int32_t>(r0 + (r1 - r0) * frac);

                l = static_cast<int32_t>(l * vol);
                r = static_cast<int32_t>(r * vol);

                alsa_out[rendered * 2]     = static_cast<int16_t>(std::clamp(l, -32768, 32767));
                alsa_out[rendered * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
                rendered++;

                if (!rev) play_pos += spd;
                else play_pos -= spd;
            }
        }

        cur_pts = std::clamp(play_pos / 44100.0, 0.0, duration.load());
        
        if (pcm && rendered > 0) {
            snd_pcm_t* p = (snd_pcm_t*)pcm;
            int frames_left = rendered;
            int16_t* out_ptr = alsa_out.data();
            while (frames_left > 0 && is_playing && !stop_requested) {
                snd_pcm_sframes_t written = snd_pcm_writei(p, out_ptr, frames_left);
                if (written < 0) {
                    written = snd_pcm_recover(p, written, 1);
                    if (written < 0) {
                        // Last-ditch recovery: full close/reopen.
                        snd_pcm_close(p);
                        pcm = nullptr;
                        snd_pcm_t* new_p = nullptr;
                        if (snd_pcm_open(&new_p, "default", SND_PCM_STREAM_PLAYBACK, 0) >= 0) {
                            snd_pcm_set_params(new_p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 150000);
                            pcm = new_p;
                            written = snd_pcm_writei(new_p, out_ptr, frames_left);
                        }
                        if (written < 0) break;
                    }
                }
                out_ptr += written * 2;
                frames_left -= written;
            }
        }
        if (natural_eof) break;
    }

    abort_decode = true;
    if (dec_th.joinable()) dec_th.join();

    is_playing = false;
    if (natural_eof && !stop_requested) track_finished = true;
}