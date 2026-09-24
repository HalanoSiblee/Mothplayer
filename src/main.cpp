#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <clocale>
#include <cwchar>
#include <ncurses.h>
#include <alsa/asoundlib.h>
#include <sixel.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;

#ifndef BUTTON4_PRESSED
#define BUTTON4_PRESSED 00040000000L
#endif
#ifndef BUTTON5_PRESSED
#define BUTTON5_PRESSED 00200000000L
#endif

#ifndef APP_V
#define APP_V "0.1.000"
#endif

enum class PlayMode : uint8_t {
    NORMAL = 0,
    LOOP = 1,
    SEQUENTIAL = 2
};

struct FileItem {
    std::string name;
    std::string path;
    bool is_dir = false;
    bool is_audio = false;
};

struct TrackMetadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string codec_name;
    int64_t bit_rate = 0;
    int sample_rate = 0;
    int channels = 0;
    std::string sixel_art;
    bool has_cover = false;
    int cover_w = 0;
    int cover_h = 0;
};

static std::string truncate_utf8(const std::string& str, int max_cols, int* out_cols = nullptr) {
    if (max_cols <= 0 || str.empty()) {
        if (out_cols) *out_cols = 0;
        return "";
    }

    bool is_ascii = true;
    for (unsigned char c : str) {
        if (c >= 0x80) {
            is_ascii = false;
            break;
        }
    }

    if (is_ascii) {
        int len = std::min(static_cast<int>(str.size()), max_cols);
        if (out_cols) *out_cols = len;
        return str.substr(0, len);
    }

    std::wstring wstr(str.size(), L'\0');
    size_t converted = std::mbstowcs(&wstr[0], str.c_str(), str.size());
    if (converted == static_cast<size_t>(-1)) {
        std::string res;
        int cols = 0;
        for (char c : str) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                if (cols >= max_cols) break;
                cols++;
            }
            res += c;
        }
        if (out_cols) *out_cols = cols;
        return res;
    }
    wstr.resize(converted);

    int cur_cols = 0;
    size_t char_count = 0;
    for (wchar_t wc : wstr) {
        int w = wcwidth(wc);
        if (w < 0) w = 0;
        if (cur_cols + w > max_cols) break;
        cur_cols += w;
        char_count++;
    }

    if (out_cols) *out_cols = cur_cols;
    wstr.resize(char_count);
    std::string out(wstr.size() * 4 + 1, '\0');
    size_t back_len = std::wcstombs(&out[0], wstr.c_str(), out.size());
    if (back_len == static_cast<size_t>(-1)) return "";
    out.resize(back_len);
    return out;
}

static int utf8_display_width(const std::string& str) {
    if (str.empty()) return 0;

    bool is_ascii = true;
    for (unsigned char c : str) {
        if (c >= 0x80) {
            is_ascii = false;
            break;
        }
    }
    if (is_ascii) return static_cast<int>(str.size());

    std::wstring wstr(str.size(), L'\0');
    size_t converted = std::mbstowcs(&wstr[0], str.c_str(), str.size());
    if (converted == static_cast<size_t>(-1)) {
        return static_cast<int>(str.length());
    }
    int cols = 0;
    for (size_t i = 0; i < converted; ++i) {
        int w = wcwidth(wstr[i]);
        if (w > 0) cols += w;
    }
    return cols;
}

static std::string trim_str(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static int sixel_write_callback(char* data, int size, void* priv) {
    auto* buf = static_cast<std::string*>(priv);
    buf->append(data, size);
    return size;
}

static std::string encode_rgb_to_sixel_libsixel(uint8_t* rgb_data, int w, int h) {
    if (!rgb_data || w <= 0 || h <= 0) return "";
    std::string sixel_buffer;
    sixel_output_t* output = nullptr;
    sixel_dither_t* dither = nullptr;

    if (SIXEL_FAILED(sixel_output_new(&output, sixel_write_callback, &sixel_buffer, nullptr))) return "";
    if (SIXEL_FAILED(sixel_dither_new(&dither, 256, nullptr))) {
        sixel_output_unref(output);
        return "";
    }
    if (SIXEL_FAILED(sixel_dither_initialize(dither, rgb_data, w, h, SIXEL_PIXELFORMAT_RGB888, LARGE_AUTO, REP_AUTO, QUALITY_HIGH))) {
        sixel_dither_unref(dither);
        sixel_output_unref(output);
        return "";
    }

    sixel_encode(rgb_data, w, h, 3, dither, output);
    sixel_dither_unref(dither);
    sixel_output_unref(output);
    return sixel_buffer;
}

static std::string generate_blank_sixel(int w, int h) {
    std::vector<uint8_t> blank_rgb(w * h * 3, 0);
    return encode_rgb_to_sixel_libsixel(blank_rgb.data(), w, h);
}

static void silent_alsa_handler(const char*, int, const char*, int, const char*, ...) {}

class AudioEngine {
public:
    std::atomic<bool> is_playing{false};
    std::atomic<bool> is_paused{false};
    std::atomic<bool> is_muted{false};
    std::atomic<bool> is_reverse{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> metadata_updated{false};
    std::atomic<bool> track_finished{false};
    std::atomic<double> seek_req{-1.0};
    std::atomic<double> volume{1.0};
    std::atomic<double> speed{1.0};
    std::atomic<double> cur_pts{0.0};
    std::atomic<double> duration{0.0};

    std::string last_played_path;
    std::mutex meta_mutex;
    TrackMetadata active_meta;
    std::atomic<uint64_t> cover_gen{0};
    std::atomic<int> cover_target_w{160};
    std::atomic<int> cover_target_h{160};

    AudioEngine() {
        snd_lib_error_set_handler(silent_alsa_handler);

        int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            pcm = nullptr;
        } else {
            snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               2, 44100, 1, 150000);
        }
        worker = std::thread(&AudioEngine::run, this);
    }

    ~AudioEngine() {
        stop_requested = true;
        abort_decode = true;
        is_playing = false;
        if (pcm) snd_pcm_drop(pcm);
        if (worker.joinable()) worker.join();
        if (pcm) snd_pcm_close(pcm);
    }

    void load(const std::string& path) {
        std::lock_guard<std::mutex> lk(cmd_mutex);
        queued_path = path;
        track_switch = true;
    }

    void toggle_pause() { if (is_playing) is_paused = !is_paused; }
    void toggle_mute() { is_muted = !is_muted; }
    void toggle_reverse() { is_reverse = !is_reverse; }

    void stop() {
        is_playing = false;
        is_paused = false;
        abort_decode = true;
        ++cover_gen;
        if (pcm) snd_pcm_drop(pcm);
        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            active_meta = TrackMetadata{};
            last_played_path.clear();
            metadata_updated = true;
        }
    }

    void seek_relative(double seconds) {
        if (is_playing) {
            double target = cur_pts.load() + seconds;
            seek_req = std::clamp(target, 0.0, duration.load());
        }
    }

    void seek_absolute_percent(double ratio) {
        if (is_playing && duration.load() > 0.0) {
            seek_req = std::clamp(ratio, 0.0, 1.0) * duration.load();
        }
    }

    void reset_modifiers() {
        volume = 1.0;
        speed = 1.0;
        is_muted = false;
        is_reverse = false;
    }

private:
    snd_pcm_t* pcm = nullptr;
    std::thread worker;
    std::mutex cmd_mutex;
    std::string queued_path;
    bool track_switch = false;

    std::vector<int16_t> pcm_data;
    std::mutex pcm_mutex;
    std::atomic<size_t> total_frames{0};
    std::atomic<bool> decode_complete{false};
    std::atomic<bool> abort_decode{false};

    void extract_album_art_async(const std::string& path, int target_w, int target_h, uint64_t my_gen) {
        AVFormatContext* fctx = nullptr;
        if (avformat_open_input(&fctx, path.c_str(), nullptr, nullptr) < 0) return;
        if (avformat_find_stream_info(fctx, nullptr) < 0) {
            avformat_close_input(&fctx);
            return;
        }

        std::string sixel;
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
                        double scale = std::min(
                            static_cast<double>(target_w) / std::max(1, src_w),
                            static_cast<double>(target_h) / std::max(1, src_h));
                        int out_w = std::max(1, static_cast<int>(src_w * scale)) & ~1;
                        int out_h = std::max(1, static_cast<int>(src_h * scale)) & ~1;
                        if (out_w < 2) out_w = 2;
                        if (out_h < 2) out_h = 2;

                        SwsContext* sws = sws_getContext(
                            src_w, src_h, static_cast<AVPixelFormat>(vframe->format),
                            out_w, out_h, AV_PIX_FMT_RGB24,
                            SWS_LANCZOS, nullptr, nullptr, nullptr);
                        if (sws) {
                            std::vector<uint8_t> rgb(out_w * out_h * 3);
                            uint8_t* dst[1] = { rgb.data() };
                            int strides[1] = { out_w * 3 };
                            sws_scale(sws, vframe->data, vframe->linesize, 0, src_h, dst, strides);
                            sixel = encode_rgb_to_sixel_libsixel(rgb.data(), out_w, out_h);
                            has_cover = !sixel.empty();
                            sws_freeContext(sws);
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

        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            if (last_played_path != path) return;
            active_meta.sixel_art = std::move(sixel);
            active_meta.has_cover = has_cover;
            active_meta.cover_w = cover_out_w;
            active_meta.cover_h = cover_out_h;
            metadata_updated = true;
        }
    }

    void fill_pcm_buffer(const std::string& path) {
        AVFormatContext* fctx = nullptr;
        if (avformat_open_input(&fctx, path.c_str(), nullptr, nullptr) < 0) {
            decode_complete = true;
            return;
        }
        if (avformat_find_stream_info(fctx, nullptr) < 0) {
            avformat_close_input(&fctx);
            decode_complete = true;
            return;
        }

        int audio_idx = -1;
        for (unsigned int i = 0; i < fctx->nb_streams; ++i) {
            if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_idx = i;
                break;
            }
        }
        if (audio_idx == -1) {
            avformat_close_input(&fctx);
            decode_complete = true;
            return;
        }

        AVCodecParameters* par = fctx->streams[audio_idx]->codecpar;
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        if (!dec) {
            avformat_close_input(&fctx);
            decode_complete = true;
            return;
        }

        AVCodecContext* cctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cctx, par);
        if (avcodec_open2(cctx, dec, nullptr) < 0) {
            avcodec_free_context(&cctx);
            avformat_close_input(&fctx);
            decode_complete = true;
            return;
        }

        const int out_rate = 44100;
        SwrContext* swr = swr_alloc();
        av_opt_set_chlayout(swr, "in_chlayout", &cctx->ch_layout, 0);
        av_opt_set_int(swr, "in_sample_rate", cctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", cctx->sample_fmt, 0);

        AVChannelLayout out_ch;
        av_channel_layout_default(&out_ch, 2);
        av_opt_set_chlayout(swr, "out_chlayout", &out_ch, 0);
        av_opt_set_int(swr, "out_sample_rate", out_rate, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        swr_init(swr);
        av_channel_layout_uninit(&out_ch);

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frm = av_frame_alloc();

        std::vector<int16_t> temp_buf(out_rate * 2);
        size_t temp_count = 0;
        const size_t MAX_DECODE_FRAMES = 44100ULL * 7200ULL; // 2 hour max buffer limit

        while (!abort_decode && !stop_requested) {
            if (total_frames.load() >= MAX_DECODE_FRAMES) break;

            int ret = av_read_frame(fctx, pkt);
            if (ret < 0) break;

            if (pkt->stream_index == audio_idx) {
                if (avcodec_send_packet(cctx, pkt) == 0) {
                    while (avcodec_receive_frame(cctx, frm) == 0) {
                        int samples = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) +
                                                      frm->nb_samples, out_rate, cctx->sample_rate, AV_ROUND_UP);
                        if (samples > 0) {
                            if (temp_count + samples * 2 > temp_buf.size()) {
                                temp_buf.resize(temp_count + samples * 2 + out_rate);
                            }
                            uint8_t* out_ptrs[1] = { reinterpret_cast<uint8_t*>(temp_buf.data() + temp_count) };
                            int converted = swr_convert(swr, out_ptrs, samples,
                                                        const_cast<const uint8_t**>(frm->extended_data), frm->nb_samples);
                            if (converted > 0) {
                                temp_count += converted * 2;
                            }
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
                int samples = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) +
                                              frm->nb_samples, out_rate, cctx->sample_rate, AV_ROUND_UP);
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
            temp_count = 0;
        }

        av_frame_free(&frm);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&cctx);
        avformat_close_input(&fctx);

        decode_complete = true;
    }

    void run() {
        while (!stop_requested) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(cmd_mutex);
                if (track_switch) {
                    path = queued_path;
                    track_switch = false;
                }
            }
            if (path.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            decode_loop(path);
        }
    }

    void decode_loop(const std::string& path) {
        AVFormatContext* fctx = nullptr;
        if (avformat_open_input(&fctx, path.c_str(), nullptr, nullptr) < 0) return;
        if (avformat_find_stream_info(fctx, nullptr) < 0) {
            avformat_close_input(&fctx);
            return;
        }

        int audio_idx = -1;
        for (unsigned int i = 0; i < fctx->nb_streams; ++i) {
            if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_idx = i;
                break;
            }
        }
        if (audio_idx == -1) {
            avformat_close_input(&fctx);
            return;
        }

        AVCodecParameters* par = fctx->streams[audio_idx]->codecpar;
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        AVCodecContext* cctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cctx, par);
        if (avcodec_open2(cctx, dec, nullptr) < 0) {
            avcodec_free_context(&cctx);
            avformat_close_input(&fctx);
            return;
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
        {
            std::lock_guard<std::mutex> lk(pcm_mutex);
            pcm_data.clear();
        }

        std::thread dec_th(&AudioEngine::fill_pcm_buffer, this, path);

        while (!decode_complete && total_frames.load() < 4096 && !abort_decode && !stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

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
                if (track_switch) {
                    natural_eof = false;
                    break;
                }
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
            if (!rev) {
                if (play_pos >= tf) {
                    if (decode_complete) {
                        natural_eof = true;
                        break;
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                }
            } else {
                if (play_pos <= 0.0) {
                    natural_eof = true;
                    break;
                }
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
                    if (!rev && play_pos >= cur_tf) {
                        if (decode_complete) { natural_eof = true; }
                        break;
                    }
                    if (rev && play_pos < 0.0) {
                        natural_eof = true;
                        break;
                    }

                    double f = play_pos;
                    int64_t i0 = static_cast<int64_t>(std::floor(f));
                    if (i0 < 0) i0 = 0;
                    if (static_cast<size_t>(i0) >= cur_tf) i0 = cur_tf - 1;
                    int64_t i1 = (static_cast<size_t>(i0 + 1) < cur_tf) ? i0 + 1 : i0;
                    double frac = f - std::floor(f);

                    int32_t l0 = pcm_data[i0 * 2];
                    int32_t r0 = pcm_data[i0 * 2 + 1];
                    int32_t l1 = pcm_data[i1 * 2];
                    int32_t r1 = pcm_data[i1 * 2 + 1];

                    int32_t l = static_cast<int32_t>(l0 + (l1 - l0) * frac);
                    int32_t r = static_cast<int32_t>(r0 + (r1 - r0) * frac);

                    l = static_cast<int32_t>(l * vol);
                    r = static_cast<int32_t>(r * vol);

                    alsa_out[rendered * 2]     = static_cast<int16_t>(std::clamp(l, -32768, 32767));
                    alsa_out[rendered * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
                    rendered++;

                    if (!rev) {
                        play_pos += spd;
                    } else {
                        play_pos -= spd;
                    }
                }
            }

            cur_pts = std::clamp(play_pos / 44100.0, 0.0, duration.load());

            if (pcm && rendered > 0) {
                int frames_left = rendered;
                int16_t* p = alsa_out.data();
                while (frames_left > 0 && is_playing && !stop_requested) {
                    snd_pcm_sframes_t written = snd_pcm_writei(pcm, p, frames_left);
                    if (written < 0) {
                        written = snd_pcm_recover(pcm, written, 1);
                        if (written < 0) break;
                    } else {
                        p += written * 2;
                        frames_left -= written;
                    }
                }
            }

            if (natural_eof) break;
        }

        abort_decode = true;
        if (dec_th.joinable()) dec_th.join();

        is_playing = false;
        if (natural_eof && !stop_requested) {
            track_finished = true;
        }
    }
};

class MothApp {
public:
    bool need_redraw = true;
    bool need_status = true;
    bool is_searching = false;
    bool is_command_mode = false;
    bool show_help = false;
    bool show_about = false;
    bool show_del_confirm = false;
    PlayMode mode = PlayMode::NORMAL;

    int last_idx = -1;
    int last_scroll = -1;
    std::string last_cover_path;
    bool sixel_on_screen = false;
    int last_sixel_w = 0;
    int last_sixel_h = 0;
    int last_sixel_row = 0;
    int last_sixel_col = 0;
    static constexpr double RIGHT_FRAC = 0.40;

    std::string del_target_name;
    std::string del_target_path;
    bool del_target_is_dir = false;

    MothApp() {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        timeout(50);

        scrollok(stdscr, FALSE);
        idlok(stdscr, FALSE);
        leaveok(stdscr, TRUE);

        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
        mouseinterval(0);

        start_color();
        use_default_colors();
        init_pair(1, -1, -1);
        init_pair(2, COLOR_WHITE, COLOR_RED);

        blank_sixel_seq = generate_blank_sixel(160, 160);

        path = fs::current_path();
        scan();
    }

    ~MothApp() {
        if (sixel_on_screen) {
            int max_x = getmaxx(stdscr);
            int right_w = std::clamp(static_cast<int>(max_x * RIGHT_FRAC), 28, std::max(28, max_x - 22));
            int split_x = std::max(22, max_x - right_w);
            std::cout << "\033[s\033[2;" << (split_x + 4) << "H"
                      << blank_sixel_seq << "\033[u" << std::flush;
        }
        endwin();
    }

    void scan() {
        entries.clear();
        if (path.has_parent_path() && path != path.parent_path()) {
            entries.push_back({ "..", (path / "..").lexically_normal().string(), true, false });
        }

        std::vector<FileItem> dirs, regular;
        try {
            for (const auto& e : fs::directory_iterator(path)) {
                if (e.is_directory()) {
                    dirs.push_back({ e.path().filename().string(), e.path().string(), true, false });
                } else {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a") {
                        regular.push_back({ e.path().filename().string(), e.path().string(), false, true });
                    }
                }
            }
        } catch (...) {}

        std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b){ return a.name < b.name; });
        std::sort(regular.begin(), regular.end(), [](const auto& a, const auto& b){ return a.name < b.name; });

        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), regular.begin(), regular.end());
        if (idx >= static_cast<int>(entries.size())) idx = std::max(0, static_cast<int>(entries.size()) - 1);
        last_idx = -1;
        last_scroll = -1;
        need_redraw = true;
    }

    void play_next_track(AudioEngine& audio) {
        if (entries.empty()) return;
        int next_idx = idx + 1;
        while (next_idx < static_cast<int>(entries.size()) && !entries[next_idx].is_audio) {
            next_idx++;
        }
        if (next_idx < static_cast<int>(entries.size())) {
            idx = next_idx;
            audio.load(entries[idx].path);
        } else {
            audio.stop();
        }
        need_redraw = true;
    }

    static bool ci_find(const std::string& str, const std::string& sub) {
        if (sub.empty()) return true;
        auto it = std::search(
            str.begin(), str.end(),
            sub.begin(), sub.end(),
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }
        );
        return it != str.end();
    }

    void search_next(bool from_current = false) {
        if (last_query.empty() || entries.empty()) return;
        int start = from_current ? idx : (idx + 1) % entries.size();
        for (size_t offset = 0; offset < entries.size(); ++offset) {
            int cur = (start + offset) % entries.size();
            if (ci_find(entries[cur].name, last_query)) {
                idx = cur;
                need_redraw = true;
                return;
            }
        }
    }

    static std::string sec_to_str(double t) {
        int s = static_cast<int>(t);
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
        return b;
    }

    struct PanelGeom {
        int split_x;
        int side_w;
        int cover_px_w;
        int cover_px_h;
        int art_box_y;
        int art_rows;
        int meta_y;
    };

    PanelGeom compute_geom(int max_y, int max_x) const {
        PanelGeom g{};
        int right_w = std::clamp(static_cast<int>(max_x * RIGHT_FRAC), 28, std::max(28, max_x - 22));
        g.split_x = std::max(22, max_x - right_w);
        g.side_w = max_x - g.split_x - 1;

        g.art_box_y = 1;
        const int meta_lines_count = 7;
        g.meta_y = std::max(g.art_box_y + 4, (max_y - 2) - meta_lines_count);
        g.art_rows = std::max(3, g.meta_y - (g.art_box_y + 1));
        g.cover_px_w = std::clamp((g.side_w - 4) * 8, 48, 280);
        g.cover_px_h = std::clamp(g.art_rows * 16, 48, 280);
        return g;
    }

    void emit_sixel(const TrackMetadata& meta, int split_x, int art_box_y,
                    int art_rows, int side_w) {
        const int base_row = art_box_y + 1;
        const int base_col = split_x + 2;
        const int area_cols = std::max(4, side_w - 2);

        if (sixel_on_screen && last_sixel_w > 0 && last_sixel_h > 0) {
            std::string eraser = generate_blank_sixel(last_sixel_w, last_sixel_h);
            if (!eraser.empty()) {
                int er = last_sixel_row > 0 ? last_sixel_row : base_row;
                int ec = last_sixel_col > 0 ? last_sixel_col : base_col;
                std::cout << "\033[s\033[" << er << ";" << ec << "H"
                          << eraser << "\033[u" << std::flush;
            }
            sixel_on_screen = false;
        }

        for (int r = 0; r < art_rows; ++r) {
            move(base_row + r, base_col);
            for (int c = 0; c < area_cols; ++c) addch(' ');
        }

        if (meta.has_cover && !meta.sixel_art.empty()) {
            int img_cols = meta.cover_w > 0 ? std::max(1, (meta.cover_w + 9) / 10) : 10;
            if (img_cols > area_cols) img_cols = area_cols;
            int off_c = std::max(0, (area_cols - img_cols) / 2);

            const int row = base_row;
            const int col = base_col + off_c;

            std::cout << "\033[s\033[" << row << ";" << col << "H"
                      << meta.sixel_art << "\033[u" << std::flush;

            last_sixel_w = meta.cover_w > 0 ? meta.cover_w : 100;
            last_sixel_h = meta.cover_h > 0 ? meta.cover_h : 100;
            last_sixel_row = row;
            last_sixel_col = col;
            sixel_on_screen = true;
        } else {
            int msg_x = base_col + std::max(0, (area_cols - 13) / 2);
            int msg_y = base_row + std::max(0, art_rows / 2);
            mvprintw(msg_y, msg_x, "(No Artwork)");
            last_sixel_w = 0;
            last_sixel_h = 0;
            last_sixel_row = 0;
            last_sixel_col = 0;
            sixel_on_screen = false;
        }
    }

    void draw_entry_line(int y, int entry_idx, int split_x, bool is_selected) {
        if (split_x <= 0) return;
        move(y, 0);

        if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size())) {
            const auto& item = entries[entry_idx];
            if (is_selected) attron(COLOR_PAIR(1) | A_REVERSE);
            else attron(COLOR_PAIR(1));

            char prefix = item.is_dir ? '/' : ' ';
            int max_text_cols = std::max(0, split_x - 3);
            int disp_w = 0;
            std::string disp_name = truncate_utf8(item.name, max_text_cols, &disp_w);
            int pad = std::max(0, split_x - 2 - disp_w);

            mvprintw(y, 0, " %c%s%*s", prefix, disp_name.c_str(), pad, "");

            if (is_selected) attroff(COLOR_PAIR(1) | A_REVERSE);
            else attroff(COLOR_PAIR(1));
        } else {
            mvprintw(y, 0, "%*s", split_x, "");
        }
    }

    static void draw_meta_line(int& meta_y, int split_x, int side_w, int max_y, const char* label, const std::string& val) {
        if (meta_y >= max_y - 2) return;
        move(meta_y, split_x + 2);
        clrtoeol();
        mvaddch(meta_y, split_x, ACS_VLINE);
        int val_max_cols = std::max(0, side_w - 10);
        std::string val_str = truncate_utf8(val, val_max_cols);
        mvprintw(meta_y, split_x + 2, "%-8s %s", label, val_str.c_str());
        meta_y++;
    }

    void draw_status(AudioEngine& audio, int max_x, int y) {
        attron(COLOR_PAIR(1) | A_REVERSE);
        move(y, 0);
        clrtoeol();

        std::string status;
        if (audio.is_playing) {
            if (audio.is_paused) status = audio.is_reverse ? "PAUSED[REV]" : "PAUSED ";
            else status = audio.is_reverse ? "REV-PLAY" : "PLAYING ";
        } else {
            status = audio.is_reverse ? "STOPPED[REV]" : "STOPPED";
        }

        std::string mode_str = (mode == PlayMode::LOOP) ? "LOOP" : (mode == PlayMode::SEQUENTIAL ? "SEQ" : "NORMAL");
        int vol = static_cast<int>(audio.volume.load() * 100.0);
        int spd = static_cast<int>(std::round(audio.speed.load() * 100.0));

        char stat_buf[256];
        if (audio.is_muted.load()) {
            snprintf(stat_buf, sizeof(stat_buf), " [%s|%s] %s/%s | Vol: MUTE | Speed: %3d%% [F1: Help] ",
                     status.c_str(), mode_str.c_str(),
                     sec_to_str(audio.cur_pts.load()).c_str(),
                     sec_to_str(audio.duration.load()).c_str(), spd);
        } else {
            snprintf(stat_buf, sizeof(stat_buf), " [%s|%s] %s/%s | Vol: %3d%% | Speed: %3d%% [F1: Help] ",
                     status.c_str(), mode_str.c_str(),
                     sec_to_str(audio.cur_pts.load()).c_str(),
                     sec_to_str(audio.duration.load()).c_str(), vol, spd);
        }
        mvprintw(y, 1, "%.*s", std::max(0, max_x - 3), stat_buf);
        attroff(COLOR_PAIR(1) | A_REVERSE);
    }

    void draw_progress_bar(AudioEngine& audio, int max_x, int y) {
        move(y, 0);
        clrtoeol();

        if (is_command_mode) {
            std::string disp_cmd = truncate_utf8(command_query, std::max(0, max_x - 4));
            mvprintw(y, 1, ":%s", disp_cmd.c_str());
            int cur_x = 2 + utf8_display_width(disp_cmd);
            if (cur_x < max_x - 1) move(y, cur_x);
        } else if (is_searching) {
            std::string disp_query = truncate_utf8(search_query, std::max(0, max_x - 4));
            mvprintw(y, 1, "/%s", disp_query.c_str());
            int cur_x = 2 + utf8_display_width(disp_query);
            if (cur_x < max_x - 1) move(y, cur_x);
        } else {
            double dur = audio.duration.load();
            double ratio = (dur > 0.0) ? std::clamp(audio.cur_pts.load() / dur, 0.0, 1.0) : 0.0;
            int bar_w = std::max(4, max_x - 6);
            int filled = static_cast<int>(ratio * bar_w);

            mvaddch(y, 1, '[');
            if (filled > 0) {
                mvhline(y, 2, '=', filled);
            }
            if (filled < bar_w) {
                mvaddch(y, 2 + filled, audio.is_reverse ? '<' : '>');
                if (bar_w - filled - 1 > 0) {
                    mvhline(y, 2 + filled + 1, '-', bar_w - filled - 1);
                }
            }
            mvaddch(y, 2 + bar_w, ']');
        }
    }

    void render_full(AudioEngine& audio) {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);

        if (max_y < 8 || max_x < 30) {
            move(0, 0);
            clrtoeol();
            mvprintw(0, 0, "Terminal window too small.");
            refresh();
            return;
        }

        if (show_help || show_about || show_del_confirm) attron(A_DIM);

        auto g = compute_geom(max_y, max_x);
        const int split_x = g.split_x;
        const int view_h  = max_y - 3;
        audio.cover_target_w = g.cover_px_w;
        audio.cover_target_h = g.cover_px_h;

        if (idx < scroll) scroll = idx;
        if (idx >= scroll + view_h) scroll = idx - view_h + 1;

        attron(COLOR_PAIR(1) | A_REVERSE);
        move(0, 0);
        clrtoeol();
        int max_header_cols = std::max(0, max_x - 11);
        std::string header_path = truncate_utf8(path.string(), max_header_cols);
        mvprintw(0, 1, "Browser: %s", header_path.c_str());
        attroff(COLOR_PAIR(1) | A_REVERSE);

        for (int i = 0; i < view_h; ++i) {
            int entry_idx = scroll + i;
            draw_entry_line(i + 1, entry_idx, split_x, entry_idx == idx);
        }
        last_idx = idx;
        last_scroll = scroll;

        for (int y = 1; y < max_y - 2; ++y) {
            move(y, split_x + 1);
            clrtoeol();
            mvaddch(y, split_x, ACS_VLINE);
        }

        TrackMetadata meta;
        std::string cur_track_path;
        {
            std::lock_guard<std::mutex> lk(audio.meta_mutex);
            meta = audio.active_meta;
            cur_track_path = audio.last_played_path;
        }

        mvprintw(g.art_box_y, split_x + 2, "[ META ]");

        bool cover_changed = (cur_track_path != last_cover_path);
        if (cover_changed || audio.metadata_updated.load()) {
            if (!show_help && !show_about && !show_del_confirm) {
                emit_sixel(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
            }
            last_cover_path = cur_track_path;
            audio.metadata_updated = false;
        }

        int meta_y = g.meta_y;
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Title:",   meta.title.empty()  ? "(none)" : meta.title);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Artist:",  meta.artist.empty() ? "(none)" : meta.artist);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Album:",   meta.album.empty()  ? "(none)" : meta.album);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Codec:",   meta.codec_name);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Rate:",    std::to_string(meta.sample_rate) + " Hz");
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Bitrate:", std::to_string(meta.bit_rate / 1000) + " kb/s");
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Ch:",      std::to_string(meta.channels));

        draw_status(audio, max_x, max_y - 2);
        draw_progress_bar(audio, max_x, max_y - 1);

        if (show_help || show_about || show_del_confirm) attroff(A_DIM);

        if (show_help) {
            render_help_dialog(max_y, max_x);
        } else if (show_about) {
            render_about_dialog(max_y, max_x);
        } else if (show_del_confirm) {
            render_del_dialog(max_y, max_x);
        }

        refresh();
    }

    void render_light(AudioEngine& audio) {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        if (max_y < 8 || max_x < 30) return;

        auto g = compute_geom(max_y, max_x);
        const int split_x = g.split_x;
        const int view_h  = max_y - 3;
        audio.cover_target_w = g.cover_px_w;
        audio.cover_target_h = g.cover_px_h;

        bool scroll_changed = false;
        if (idx < scroll) { scroll = idx; scroll_changed = true; }
        if (idx >= scroll + view_h) { scroll = idx - view_h + 1; scroll_changed = true; }

        if (idx != last_idx || scroll != last_scroll) {
            if (scroll_changed || last_scroll != scroll) {
                for (int i = 0; i < view_h; ++i) {
                    int entry_idx = scroll + i;
                    draw_entry_line(i + 1, entry_idx, split_x, entry_idx == idx);
                }
            } else {
                int old_y = (last_idx - last_scroll) + 1;
                int new_y = (idx - scroll) + 1;
                if (old_y >= 1 && old_y <= view_h)
                    draw_entry_line(old_y, last_idx, split_x, false);
                if (new_y >= 1 && new_y <= view_h)
                    draw_entry_line(new_y, idx, split_x, true);
            }
            last_idx = idx;
            last_scroll = scroll;
        }

        draw_status(audio, max_x, max_y - 2);
        draw_progress_bar(audio, max_x, max_y - 1);

        if (audio.metadata_updated.load()) {
            TrackMetadata meta;
            std::string cur_track_path;
            {
                std::lock_guard<std::mutex> lk(audio.meta_mutex);
                meta = audio.active_meta;
                cur_track_path = audio.last_played_path;
            }
            if (!show_help && !show_about && !show_del_confirm) {
                emit_sixel(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
            }
            last_cover_path = cur_track_path;
            audio.metadata_updated = false;

            int meta_y = g.meta_y;
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Title:",   meta.title.empty()  ? "(none)" : meta.title);
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Artist:",  meta.artist.empty() ? "(none)" : meta.artist);
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Album:",   meta.album.empty()  ? "(none)" : meta.album);
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Codec:",   meta.codec_name);
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Rate:",    std::to_string(meta.sample_rate) + " Hz");
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Bitrate:", std::to_string(meta.bit_rate / 1000) + " kb/s");
            draw_meta_line(meta_y, split_x, g.side_w, max_y, "Ch:",      std::to_string(meta.channels));
        }

        if (show_help) {
            render_help_dialog(max_y, max_x);
        } else if (show_about) {
            render_about_dialog(max_y, max_x);
        } else if (show_del_confirm) {
            render_del_dialog(max_y, max_x);
        }

        refresh();
    }

    void render(AudioEngine& audio) {
        if (need_redraw) {
            render_full(audio);
            need_redraw = false;
            need_status = false;
        } else {
            render_light(audio);
            need_status = false;
        }
    }

    void render_del_dialog(int max_y, int max_x) {
        int dlg_w = std::min(60, max_x - 4);
        int dlg_h = 9;
        int top_y = (max_y - dlg_h) / 2;
        int left_x = (max_x - dlg_w) / 2;

        attron(COLOR_PAIR(2) | A_BOLD);
        for (int y = 0; y < dlg_h; ++y) {
            move(top_y + y, left_x);
            for (int x = 0; x < dlg_w; ++x) {
                if (y == 0 && x == 0) addch(ACS_ULCORNER);
                else if (y == 0 && x == dlg_w - 1) addch(ACS_URCORNER);
                else if (y == dlg_h - 1 && x == 0) addch(ACS_LLCORNER);
                else if (y == dlg_h - 1 && x == dlg_w - 1) addch(ACS_LRCORNER);
                else if (y == 0 || y == dlg_h - 1) addch(ACS_HLINE);
                else if (x == 0 || x == dlg_w - 1) addch(ACS_VLINE);
                else addch(' ');
            }
        }

        mvprintw(top_y, left_x + (dlg_w - 17) / 2, " CONFIRM DELETE ");
        attroff(COLOR_PAIR(2) | A_BOLD);

        std::string q = "Are you sure you wanna del this file?";
        mvprintw(top_y + 2, left_x + (dlg_w - static_cast<int>(q.length())) / 2, "%s", q.c_str());

        std::string disp_name = truncate_utf8(del_target_name, dlg_w - 8);
        attron(A_BOLD);
        mvprintw(top_y + 4, left_x + (dlg_w - static_cast<int>(disp_name.length()) - 4) / 2, "> %s <", disp_name.c_str());
        attroff(A_BOLD);

        std::string prompt = "[Y] Yes, Delete!     [N / Esc] Cancel";
        mvprintw(top_y + 6, left_x + (dlg_w - static_cast<int>(prompt.length())) / 2, "%s", prompt.c_str());
    }

    static void render_help_dialog(int max_y, int max_x) {
        int dlg_w = std::min(64, max_x - 4);
        int dlg_h = std::min(24, max_y - 2);
        int top_y = (max_y - dlg_h) / 2;
        int left_x = (max_x - dlg_w) / 2;

        attron(A_BOLD);
        for (int y = 0; y < dlg_h; ++y) {
            move(top_y + y, left_x);
            for (int x = 0; x < dlg_w; ++x) {
                if (y == 0 && x == 0) addch(ACS_ULCORNER);
                else if (y == 0 && x == dlg_w - 1) addch(ACS_URCORNER);
                else if (y == dlg_h - 1 && x == 0) addch(ACS_LLCORNER);
                else if (y == dlg_h - 1 && x == dlg_w - 1) addch(ACS_LRCORNER);
                else if (y == 0 || y == dlg_h - 1) addch(ACS_HLINE);
                else if (x == 0 || x == dlg_w - 1) addch(ACS_VLINE);
                else addch(' ');
            }
        }

        mvprintw(top_y, left_x + (dlg_w - 14) / 2, " KEY BINDINGS ");
        attroff(A_BOLD);

        int row = top_y + 2;
        auto draw_help_item = [&](const char* keys, const char* desc) {
            if (row < top_y + dlg_h - 1) {
                attron(A_BOLD);
                mvprintw(row, left_x + 3, "%-17s", keys);
                attroff(A_BOLD);
                printw(": %s", desc);
                row++;
            }
        };

        draw_help_item("Space", "Play / Enter Directory");
        draw_help_item("Backspace", "Go to Parent Directory");
        draw_help_item("p / c", "Pause / Resume");
        draw_help_item("r", "Toggle Reverse Playback");
        draw_help_item(":", "Vim Cmd (:speed, :vol, :seek, :del)");
        draw_help_item("v", "Stop playback");
        draw_help_item("m", "Toggle Mute");
        draw_help_item("Left / Right", "Seek -3s / +3s");
        draw_help_item("S-Left / S-Right", "Seek -1s / +1s");
        draw_help_item("0 - 9", "Instant Seek 0% - 90%");
        draw_help_item("[ / ]", "Varispeed -5% / +5%");
        draw_help_item("{ / }", "Varispeed -1% / +1%");
        draw_help_item("l / s / f", "Loop / Sequential / Normal");
        draw_help_item("PgUp / PgDn", "Scroll 5 items");
        draw_help_item("g / G", "Jump to Top / Bottom");
        draw_help_item("Mouse Wheel", "Smooth Up / Down scroll");
        draw_help_item("/ | ?", "Find track | Next match");
        draw_help_item("Home", "Reset Modifiers");
        draw_help_item("F1 / Esc", "Close Help");
        draw_help_item("F2", "About");
    }

    static void render_about_dialog(int max_y, int max_x) {
        int dlg_w = std::min(56, max_x - 4);
        int dlg_h = std::min(15, max_y - 2);
        int top_y = (max_y - dlg_h) / 2;
        int left_x = (max_x - dlg_w) / 2;

        attron(A_BOLD);
        for (int y = 0; y < dlg_h; ++y) {
            move(top_y + y, left_x);
            for (int x = 0; x < dlg_w; ++x) {
                if (y == 0 && x == 0) addch(ACS_ULCORNER);
                else if (y == 0 && x == dlg_w - 1) addch(ACS_URCORNER);
                else if (y == dlg_h - 1 && x == 0) addch(ACS_LLCORNER);
                else if (y == dlg_h - 1 && x == dlg_w - 1) addch(ACS_LRCORNER);
                else if (y == 0 || y == dlg_h - 1) addch(ACS_HLINE);
                else if (x == 0 || x == dlg_w - 1) addch(ACS_VLINE);
                else addch(' ');
            }
        }

        mvprintw(top_y, left_x + (dlg_w - 9) / 2, " ABOUT ");
        attroff(A_BOLD);

        int row = top_y + 2;
        auto line = [&](const char* s) {
            if (row < top_y + dlg_h - 1) {
                mvprintw(row++, left_x + 3, "%.*s", dlg_w - 6, s);
            }
        };

        line("Moth Player TUI audio player");
        line("");
        line("Audio   : FFmpeg decode + ALSA");
        line("Cover   : libsixel (async thread)");
        line("UI      : ncurses");
        line("");
        line("F1 Help   |  F2 About  |  q Quit");
        line("Made with <3 by @HalanoSiblee The Smart moth");
        line("");
        line(APP_V);
    }

    void execute_cmd(const std::string& line, AudioEngine& audio, bool& running) {
        std::string trimmed = trim_str(line);
        if (trimmed.empty()) return;

        std::string cmd, args;
        size_t sp = trimmed.find_first_of(" \t");
        if (sp != std::string::npos) {
            cmd = trimmed.substr(0, sp);
            args = trim_str(trimmed.substr(sp + 1));
        } else {
            cmd = trimmed;
        }
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "q" || cmd == "quit" || cmd == "exit") {
            running = false;
        } else if (cmd == "speed" || cmd == "spd") {
            if (!args.empty()) {
                double val = 1.0;
                try {
                    if (args.back() == '%') {
                        val = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                    } else {
                        val = std::stod(args);
                        if (val > 3.0 && val <= 300.0) val /= 100.0;
                    }
                    audio.speed = std::clamp(val, 0.10, 3.0);
                } catch (...) {}
            }
        } else if (cmd == "vol" || cmd == "volume" || cmd == "v") {
            if (!args.empty()) {
                double val = 1.0;
                try {
                    if (args.back() == '%') {
                        val = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                    } else {
                        val = std::stod(args);
                        if (val > 2.0 && val <= 200.0) val /= 100.0;
                    }
                    audio.volume = std::clamp(val, 0.0, 2.0);
                    audio.is_muted = false;
                } catch (...) {}
            }
        } else if (cmd == "seek" || cmd == "seeking" || cmd == "s") {
            if (!args.empty()) {
                try {
                    if (args.front() == '+' || args.front() == '-') {
                        double delta = std::stod(args);
                        audio.seek_relative(delta);
                    } else if (args.back() == '%') {
                        double pct = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                        audio.seek_absolute_percent(pct);
                    } else if (args.find(':') != std::string::npos) {
                        size_t colon = args.find(':');
                        int m = std::stoi(args.substr(0, colon));
                        double s = std::stod(args.substr(colon + 1));
                        double target = m * 60 + s;
                        if (audio.duration.load() > 0.0) {
                            audio.seek_req = std::clamp(target, 0.0, audio.duration.load());
                        }
                    } else {
                        double s = std::stod(args);
                        if (audio.duration.load() > 0.0) {
                            audio.seek_req = std::clamp(s, 0.0, audio.duration.load());
                        }
                    }
                } catch (...) {}
            }
        } else if (cmd == "reversing" || cmd == "reverse" || cmd == "rev") {
            if (args.empty()) {
                audio.toggle_reverse();
            } else {
                std::string a = args;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                if (a == "on" || a == "1" || a == "true") audio.is_reverse = true;
                else if (a == "off" || a == "0" || a == "false") audio.is_reverse = false;
                else audio.toggle_reverse();
            }
        } else if (cmd == "del" || cmd == "delete" || cmd == "rm") {
            if (!entries.empty() && idx >= 0 && idx < static_cast<int>(entries.size())) {
                if (entries[idx].name != "..") {
                    flash();
                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    attron(COLOR_PAIR(2));
                    for (int y = 0; y < max_y; ++y) {
                        mvhline(y, 0, ' ', max_x);
                    }
                    attroff(COLOR_PAIR(2));
                    refresh();
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));

                    show_del_confirm = true;
                    del_target_name = entries[idx].name;
                    del_target_path = entries[idx].path;
                    del_target_is_dir = entries[idx].is_dir;
                    last_cover_path.clear();
                    sixel_on_screen = false;
                }
            }
        } else if (cmd == "rename" || cmd == "mv") {
            if (!args.empty() && !entries.empty() && idx >= 0 && idx < static_cast<int>(entries.size())) {
                if (entries[idx].name != "..") {
                    std::string new_name = args;
                    if (new_name.front() == '"' && new_name.back() == '"' && new_name.size() >= 2) {
                        new_name = new_name.substr(1, new_name.size() - 2);
                    } else if (new_name.front() == '\'' && new_name.back() == '\'' && new_name.size() >= 2) {
                        new_name = new_name.substr(1, new_name.size() - 2);
                    }
                    fs::path clean_p(new_name);
                    std::string clean_name = clean_p.filename().string();
                    if (!clean_name.empty()) {
                        fs::path old_p = entries[idx].path;
                        fs::path new_p = old_p.parent_path() / clean_name;
                        std::error_code ec;
                        fs::rename(old_p, new_p, ec);
                        if (!ec) {
                            if (audio.last_played_path == old_p.string()) {
                                std::lock_guard<std::mutex> lk(audio.meta_mutex);
                                audio.last_played_path = new_p.string();
                            }
                            scan();
                            for (size_t i = 0; i < entries.size(); ++i) {
                                if (entries[i].name == clean_name) {
                                    idx = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else if (cmd == "help" || cmd == "h") {
            show_help = true;
        }
    }

    void handle_input(AudioEngine& audio, bool& running) {
        int ch = getch();
        if (ch == ERR) return;

        need_status = true;

        if (ch == KEY_RESIZE) {
            last_cover_path.clear();
            sixel_on_screen = false;
            need_redraw = true;
            return;
        }

        if (show_del_confirm) {
            if (ch == 'y' || ch == 'Y' || ch == 10 || ch == 13 || ch == KEY_ENTER) {
                if (audio.last_played_path == del_target_path) {
                    audio.stop();
                }
                std::error_code ec;
                if (del_target_is_dir) fs::remove_all(del_target_path, ec);
                else fs::remove(del_target_path, ec);

                show_del_confirm = false;
                last_cover_path.clear();
                sixel_on_screen = false;
                scan();
                need_redraw = true;
                return;
            }
            if (ch == 'n' || ch == 'N' || ch == 27 || ch == 'q' || ch == 'Q') {
                show_del_confirm = false;
                need_redraw = true;
                return;
            }
            return;
        }

        if (is_command_mode) {
            if (ch == 27) {
                is_command_mode = false;
                command_query.clear();
                curs_set(0);
                need_redraw = true;
                return;
            }
            if (ch == 10 || ch == 13 || ch == KEY_ENTER) {
                is_command_mode = false;
                curs_set(0);
                execute_cmd(command_query, audio, running);
                command_query.clear();
                need_redraw = true;
                return;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (!command_query.empty()) {
                    command_query.pop_back();
                } else {
                    is_command_mode = false;
                    curs_set(0);
                    need_redraw = true;
                }
                need_status = true;
                return;
            }
            if (ch >= 32 && ch <= 126) {
                command_query += static_cast<char>(ch);
                need_status = true;
                return;
            }
            return;
        }

        if (ch == KEY_F(1)) {
            show_about = false;
            show_help = !show_help;
            last_cover_path.clear();
            sixel_on_screen = false;
            need_redraw = true;
            return;
        }

        if (ch == KEY_F(2)) {
            show_help = false;
            show_about = !show_about;
            last_cover_path.clear();
            sixel_on_screen = false;
            need_redraw = true;
            return;
        }

        if (show_help || show_about) {
            if (ch == 27 || ch == 'q' || ch == ' ' || ch == 10 || ch == 13 || ch == KEY_ENTER) {
                show_help = false;
                show_about = false;
                last_cover_path.clear();
                sixel_on_screen = false;
                need_redraw = true;
                return;
            }
        }

        if (is_searching) {
            if (ch == 27) {
                is_searching = false;
                curs_set(0);
                need_redraw = true;
                return;
            }
            if (ch == 10 || ch == 13 || ch == KEY_ENTER) {
                is_searching = false;
                curs_set(0);
                if (!search_query.empty()) {
                    last_query = search_query;
                    search_next(true);
                }
                need_redraw = true;
                return;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (!search_query.empty()) search_query.pop_back();
                need_status = true;
                return;
            }
            if (ch >= 32 && ch <= 126) {
                search_query += static_cast<char>(ch);
                need_status = true;
                return;
            }
            return;
        }

        if (ch == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK) {
                if (ev.bstate & BUTTON4_PRESSED) {
                    idx = std::max(0, idx - 1);
                    need_status = true;
                    return;
                }
                if (ev.bstate & BUTTON5_PRESSED) {
                    idx = std::min(static_cast<int>(entries.size()) - 1, idx + 1);
                    need_status = true;
                    return;
                }

                if (ev.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
                    int max_y, max_x;
                    getmaxyx(stdscr, max_y, max_x);
                    auto g = compute_geom(max_y, max_x);
                    int view_h = max_y - 3;
                    int bot_y = max_y - 1;
                    int bar_w = std::max(4, max_x - 6);

                    if (ev.y == bot_y && ev.x >= 2 && ev.x <= 2 + bar_w) {
                        double pct = static_cast<double>(ev.x - 2) / static_cast<double>(bar_w);
                        audio.seek_absolute_percent(pct);
                        need_status = true;
                        return;
                    }

                    if (ev.y >= 1 && ev.y <= view_h && ev.x < g.split_x) {
                        int clicked = scroll + (ev.y - 1);
                        if (clicked < static_cast<int>(entries.size())) {
                            idx = clicked;
                            if (entries[idx].is_dir) {
                                try {
                                    path = fs::canonical(entries[idx].path);
                                    scan();
                                } catch (...) {}
                            } else if (entries[idx].is_audio) {
                                audio.load(entries[idx].path);
                            }
                            need_redraw = true;
                        }
                        return;
                    }
                }
            }
            return;
        }

        if (ch == ':') {
            is_command_mode = true;
            command_query.clear();
            curs_set(1);
            need_status = true;
            return;
        }

        if (ch == '/') {
            is_searching = true;
            search_query.clear();
            curs_set(1);
            need_status = true;
            return;
        }

        if (ch == '?') {
            search_next(false);
            need_status = true;
            return;
        }

        if (ch >= '0' && ch <= '9') {
            audio.seek_absolute_percent((ch - '0') * 0.10);
            need_status = true;
            return;
        }

        switch (ch) {
            case 'q': case 'Q':
                running = false;
                break;

            case 'r': case 'R':
                audio.toggle_reverse();
                need_status = true;
                break;

            case KEY_BACKSPACE:
            case 127:
            case '\b':
                if (path.has_parent_path() && path != path.parent_path()) {
                    try {
                        path = fs::canonical(path.parent_path());
                    } catch (...) {
                        path = path.parent_path();
                    }
                    scan();
                    need_redraw = true;
                }
                break;

            case KEY_UP: case 'k':
                if (idx > 0) idx--;
                need_status = true;
                break;
            case KEY_DOWN: case 'j':
                if (idx + 1 < static_cast<int>(entries.size())) idx++;
                need_status = true;
                break;

            case KEY_PPAGE:
                idx = std::max(0, idx - 5);
                need_status = true;
                break;
            case KEY_NPAGE:
                idx = std::min(static_cast<int>(entries.size()) - 1, idx + 5);
                need_status = true;
                break;

            case KEY_SPREVIOUS: case 'g':
                idx = 0;
                need_status = true;
                break;
            case KEY_SNEXT: case 'G':
                if (!entries.empty()) idx = static_cast<int>(entries.size()) - 1;
                need_status = true;
                break;

            case ' ':
                if (entries.empty()) break;
                if (entries[idx].is_dir) {
                    try {
                        path = fs::canonical(entries[idx].path);
                        scan();
                    } catch (...) {}
                } else if (entries[idx].is_audio) {
                    audio.load(entries[idx].path);
                }
                need_redraw = true;
                break;

            case KEY_HOME:
                audio.reset_modifiers();
                need_status = true;
                break;

            case 'p': case 'c':
                audio.toggle_pause();
                need_status = true;
                break;
            case 'v':
                audio.stop();
                need_redraw = true;
                break;
            case 'm':
                audio.toggle_mute();
                need_status = true;
                break;

            case 'l': mode = PlayMode::LOOP; need_status = true; break;
            case 's': mode = PlayMode::SEQUENTIAL; need_status = true; break;
            case 'f': mode = PlayMode::NORMAL; need_status = true; break;

            case KEY_LEFT:
                audio.seek_relative(-3.0);
                need_status = true;
                break;
            case KEY_RIGHT:
                audio.seek_relative(3.0);
                need_status = true;
                break;
            case KEY_SLEFT:
                audio.seek_relative(-1.0);
                need_status = true;
                break;
            case KEY_SRIGHT:
                audio.seek_relative(1.0);
                need_status = true;
                break;

            case '[':
                audio.speed = std::clamp(audio.speed.load() - 0.05, 0.10, 3.0);
                need_status = true;
                break;
            case ']':
                audio.speed = std::clamp(audio.speed.load() + 0.05, 0.10, 3.0);
                need_status = true;
                break;
            case '{':
                audio.speed = std::clamp(audio.speed.load() - 0.01, 0.10, 3.0);
                need_status = true;
                break;
            case '}':
                audio.speed = std::clamp(audio.speed.load() + 0.01, 0.10, 3.0);
                need_status = true;
                break;

            case '-':
                audio.volume = std::clamp(audio.volume.load() - 0.05, 0.0, 2.0);
                need_status = true;
                break;
            case '+': case '=':
                audio.volume = std::clamp(audio.volume.load() + 0.05, 0.0, 2.0);
                need_status = true;
                break;
            default:
                break;
        }
    }

private:
    fs::path path;
    std::vector<FileItem> entries;
    std::string blank_sixel_seq;
    std::string search_query;
    std::string last_query;
    std::string command_query;
    int idx = 0;
    int scroll = 0;
};

int main() {
    std::setlocale(LC_ALL, "");
    av_log_set_level(AV_LOG_QUIET);

    AudioEngine audio;
    MothApp app;

    bool running = true;
    double last_pts = -1.0;
    bool last_playing = false;
    bool last_paused = false;
    bool last_muted = false;
    bool last_reverse = false;
    double last_vol = -1.0;
    double last_spd = -1.0;

    while (running) {
        app.handle_input(audio, running);

        if (audio.track_finished.exchange(false)) {
            if (app.mode == PlayMode::LOOP) {
                if (!audio.last_played_path.empty()) {
                    audio.load(audio.last_played_path);
                }
            } else if (app.mode == PlayMode::SEQUENTIAL) {
                app.play_next_track(audio);
            }
        }

        bool cur_playing = audio.is_playing.load();
        bool cur_paused  = audio.is_paused.load();
        bool cur_muted   = audio.is_muted.load();
        bool cur_reverse = audio.is_reverse.load();
        double cur_pts   = audio.cur_pts.load();
        double cur_vol   = audio.volume.load();
        double cur_spd   = audio.speed.load();

        bool pts_moved = std::abs(cur_pts - last_pts) >= 0.20;
        bool state_changed = (cur_playing != last_playing) ||
                             (cur_paused  != last_paused)  ||
                             (cur_muted   != last_muted)   ||
                             (cur_reverse != last_reverse) ||
                             (std::abs(cur_vol - last_vol) > 0.001) ||
                             (std::abs(cur_spd - last_spd) > 0.001) ||
                             audio.metadata_updated.load();

        if (app.need_redraw || app.need_status || pts_moved || state_changed) {
            app.render(audio);
            last_pts     = cur_pts;
            last_playing = cur_playing;
            last_paused  = cur_paused;
            last_muted   = cur_muted;
            last_reverse = cur_reverse;
            last_vol     = cur_vol;
            last_spd     = cur_spd;
        }
    }

    return 0;
}