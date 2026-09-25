#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <iostream>
#include <vector>
#include <string>
#include <string_view>
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
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <unordered_map>
#include <alsa/asoundlib.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;
#ifndef APP_V
#define APP_V "0.1.000"
#endif
static int g_wake_fds[2] = {
    -1, -1
};
[[gnu::always_inline]]static inline void wake_ui()noexcept {
    if (__builtin_expect(g_wake_fds[1] >= 0, 1)) {
        char c = 1;
        [[maybe_unused]]ssize_t r = ::write(g_wake_fds[1], &c, 1);
    }
}

static void drain_wake_pipe()noexcept {
    if (g_wake_fds[0] < 0)return;
    char buf[128];
    while (::read(g_wake_fds[0], buf, sizeof(buf)) > 0) {
    }
}

static constexpr uint32_t COLOR_BG = 0xE6000000;
static constexpr uint32_t COLOR_ACCENT = 0xFF89B4FA;
static constexpr uint32_t COLOR_FG = 0xFFCDD6F4;
static constexpr uint32_t COLOR_ACCENT_FG = 0xFF1E1E2E;
static constexpr uint32_t COLOR_DIM = 0xFF6C7086;
static constexpr uint32_t COLOR_DIALOG_BG = 0xF2313244;
static constexpr uint32_t COLOR_DIALOG_FG = 0xFFCDD6F4;
static constexpr uint32_t COLOR_DIALOG_BORDER = 0xFF7F849C;
static constexpr uint32_t COLOR_DANGER_BG = 0xF2F38BA8;
static constexpr uint32_t COLOR_DANGER_FG = 0xFF1E1E2E;
static constexpr uint32_t COLOR_DANGER_BORDER = 0xFFF38BA8;
static constexpr const char *FONT_PRIMARY = "monospace-11";
static constexpr const char *FONT_FALLBACK1 = "fixed-11";
[[gnu::const]]static XRenderColor argb_to_xrender(uint32_t c)noexcept {
    XRenderColor xr;
    xr.alpha = ((c >> 24) & 0xFF) * 0x0101;
    xr.red = ((c >> 16) & 0xFF) * 0x0101;
    xr.green = ((c >> 8) & 0xFF) * 0x0101;
    xr.blue = (c & 0xFF) * 0x0101;
    return xr;
}

static std::string truncate_utf8(const std::string & str, int max_cols, int *out_cols = nullptr) {
    if (max_cols <= 0 || str.empty()) {
        if (out_cols) * out_cols = 0;
        return {
        };
    }
    bool is_ascii = true;
    for (unsigned char c : str)if (c >= 0x80) {
        is_ascii = false;
        break;
    }
    if (is_ascii) {
        int len = std::min((int)str.size(), max_cols);
        if (out_cols) * out_cols = len;
        return str.substr(0, len);
    }
    std::wstring wstr(str.size(), L'\0');
    size_t converted = std::mbstowcs( &wstr[0], str.c_str(), str.size());
    if (converted == (size_t) - 1) {
        std::string res;
        int cols = 0;
        for (char c : str) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                if (cols >= max_cols)break;
                cols++;
            }
            res += c;
        }
        if (out_cols) * out_cols = cols;
        return res;
    }
    wstr.resize(converted);
    int cur_cols = 0;
    size_t char_count = 0;
    for (wchar_t wc : wstr) {
        int w = wcwidth(wc);
        if (w < 0)w = 0;
        if (cur_cols + w > max_cols)break;
        cur_cols += w;
        char_count++;
    }
    if (out_cols) * out_cols = cur_cols;
    wstr.resize(char_count);
    std::string out(wstr.size() * 4 + 1, '\0');
    size_t back_len = std::wcstombs( &out[0], wstr.c_str(), out.size());
    if (back_len == (size_t) - 1)return {
    };
    out.resize(back_len);
    return out;
}

static int utf8_display_width(const std::string & str) {
    if (str.empty())return 0;
    bool is_ascii = true;
    for (unsigned char c : str)if (c >= 0x80) {
        is_ascii = false;
        break;
    }
    if (is_ascii)return (int)str.size();
    std::wstring wstr(str.size(), L'\0');
    size_t converted = std::mbstowcs( &wstr[0], str.c_str(), str.size());
    if (converted == (size_t) - 1)return (int)str.length();
    int cols = 0;
    for (size_t i = 0; i < converted; ++i) {
        int w = wcwidth(wstr[i]);
        if (w > 0)cols += w;
    }
    return cols;
}

static std::string trim_str(const std::string & s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)return {
    };
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static int x_error_handler(Display * d, XErrorEvent * e) {
    char buf[256];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "[X error] %s (opcode=%d resource=0x%lx minor=%d)\n", buf, e->request_code, e->resourceid, e->minor_code);
    return 0;
}

enum class PlayMode : uint8_t {
    NORMAL = 0, LOOP = 1, SEQUENTIAL = 2
};
struct FileItem {
    std::string name, path;
    bool is_dir = false, is_audio = false;
};
struct TrackMetadata {
    std::string title, artist, album, codec_name;
    int64_t bit_rate = 0;
    int sample_rate = 0, channels = 0;
    std::vector<uint8_t> cover_rgb;
    int cover_w = 0, cover_h = 0;
    bool has_cover = false;
    void release_memory() {
        std::vector<uint8_t>().swap(cover_rgb);
        cover_w = cover_h = 0;
        has_cover = false;
    }
};
static void silent_alsa_handler(const char * , int, const char * , int, const char * , ... ) {
}

class AudioEngine {
    public:
    std::atomic<bool> is_playing {
        false
    };
    std::atomic<bool> is_paused {
        false
    };
    std::atomic<bool> is_muted {
        false
    };
    std::atomic<bool> is_reverse {
        false
    };
    std::atomic<bool> stop_requested {
        false
    };
    std::atomic<bool> metadata_updated {
        false
    };
    std::atomic<bool> track_finished {
        false
    };
    std::atomic<double> seek_req {
        -1.0
    };
    std::atomic<double> volume {
        1.0
    };
    std::atomic<double> speed {
        1.0
    };
    std::atomic<double> cur_pts {
        0.0
    };
    std::atomic<double> duration {
        0.0
    };
    std::string last_played_path;
    std::mutex meta_mutex;
    TrackMetadata active_meta;
    std::atomic<uint64_t> cover_gen {
        0
    };
    std::atomic<int> cover_target_w {
        200
    };
    std::atomic<int> cover_target_h {
        200
    };
    AudioEngine() {
        snd_lib_error_set_handler(silent_alsa_handler);
        int err = snd_pcm_open( &pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0)pcm = nullptr;
        else snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 150000);
        worker = std::thread( &AudioEngine::run, this);
    }
    ~AudioEngine() {
        stop_requested.store(true, std::memory_order_relaxed);
        abort_decode.store(true, std::memory_order_relaxed);
        is_playing.store(false, std::memory_order_relaxed);
        if (pcm)snd_pcm_drop(pcm);
        if (worker.joinable())worker.join();
        if (pcm)snd_pcm_close(pcm);
    }
    void load(const std::string & path) {
        std::lock_guard<std::mutex> lk(cmd_mutex);
        queued_path = path;
        track_switch = true;
    }
    void toggle_pause() {
        if (is_playing)is_paused = !is_paused;
    }
    void toggle_mute() {
        is_muted = !is_muted;
    }
    void toggle_reverse() {
        is_reverse = !is_reverse;
    }
    void stop() {
        is_playing = false;
        is_paused = false;
        abort_decode.store(true, std::memory_order_relaxed);
        ++cover_gen;
        if (pcm) {
            snd_pcm_drop(pcm);
            snd_pcm_prepare(pcm);
        }
        {
            std::lock_guard<std::mutex> lk(pcm_mutex);
            std::vector<int16_t>().swap(pcm_data);
        }
        total_frames.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            active_meta.release_memory();
            active_meta = TrackMetadata {
            };
            last_played_path.clear();
        }
        metadata_updated.store(true, std::memory_order_release);
        wake_ui();
    }
    void seek_relative(double s) {
        if (is_playing)seek_req = std::clamp(cur_pts.load() + s, 0.0, duration.load());
    }
    void seek_absolute_percent(double r) {
        if (is_playing && duration.load() > 0.0)seek_req = std::clamp(r, 0.0, 1.0) * duration.load();
    }
    void reset_modifiers() {
        volume = 1.0;
        speed = 1.0;
        is_muted = false;
        is_reverse = false;
    }
    private:
    snd_pcm_t * pcm = nullptr;
    std::thread worker;
    std::mutex cmd_mutex;
    std::string queued_path;
    bool track_switch = false;
    std::vector<int16_t> pcm_data;
    std::mutex pcm_mutex;
    std::atomic<size_t> total_frames {
        0
    };
    std::atomic<bool> decode_complete {
        false
    };
    std::atomic<bool> abort_decode {
        false
    };
    void extract_album_art_async(const std::string & path, int tw, int th, uint64_t my_gen) {
        AVFormatContext * fctx = nullptr;
        if (avformat_open_input( &fctx, path.c_str(), nullptr, nullptr) < 0)return;
        if (avformat_find_stream_info(fctx, nullptr) < 0) {
            avformat_close_input( &fctx);
            return;
        }
        std::vector<uint8_t> rgb;
        bool has_cover = false;
        int ow = 0, oh = 0;
        for (unsigned int i = 0; i < fctx->nb_streams; ++i) {
            if (!(fctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC))continue;
            AVPacket pkt = fctx->streams[i]->attached_pic;
            const AVCodec * c = avcodec_find_decoder(fctx->streams[i]->codecpar->codec_id);
            if (!c)break;
            AVCodecContext * cctx = avcodec_alloc_context3(c);
            if (!cctx)break;
            avcodec_parameters_to_context(cctx, fctx->streams[i]->codecpar);
            if (avcodec_open2(cctx, c, nullptr) == 0) {
                if (avcodec_send_packet(cctx, &pkt) == 0) {
                    AVFrame * vf = av_frame_alloc();
                    if (vf && avcodec_receive_frame(cctx, vf) == 0) {
                        int sw = vf->width, sh = vf->height;
                        double sc = std::min((double)tw / std::max(1, sw), (double)th / std::max(1, sh));
                        int out_w = std::max(1, (int)(sw * sc));
                        int out_h = std::max(1, (int)(sh * sc));
                        SwsContext * sctx = sws_getContext(sw, sh, (AVPixelFormat)vf->format, out_w, out_h, AV_PIX_FMT_RGB24, SWS_LANCZOS, nullptr, nullptr, nullptr);
                        if (sctx) {
                            rgb.assign((size_t)out_w * out_h * 3, 0);
                            uint8_t *dst[1] = {
                                rgb.data()
                            };
                            int strides[1] = {
                                out_w * 3
                            };
                            sws_scale(sctx, vf->data, vf->linesize, 0, sh, dst, strides);
                            sws_freeContext(sctx);
                            has_cover = true;
                            ow = out_w;
                            oh = out_h;
                        }
                    }
                    if (vf)av_frame_free( &vf);
                }
            }
            avcodec_free_context( &cctx);
            break;
        }
        avformat_close_input( &fctx);
        if (my_gen != cover_gen.load(std::memory_order_relaxed))return;
        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            if (last_played_path != path)return;
            active_meta.cover_rgb = std::move(rgb);
            active_meta.has_cover = has_cover;
            active_meta.cover_w = ow;
            active_meta.cover_h = oh;
            metadata_updated.store(true, std::memory_order_release);
        }
        wake_ui();
    }
    void fill_pcm_buffer(const std::string & path) {
        AVFormatContext * fctx = nullptr;
        if (avformat_open_input( &fctx, path.c_str(), nullptr, nullptr) < 0) {
            decode_complete = true;
            return;
        }
        if (avformat_find_stream_info(fctx, nullptr) < 0) {
            avformat_close_input( &fctx);
            decode_complete = true;
            return;
        }
        int ai = -1;
        for (unsigned int i = 0; i < fctx->nb_streams; ++i)if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ai = i;
            break;
        }
        if (ai == -1) {
            avformat_close_input( &fctx);
            decode_complete = true;
            return;
        }
        AVCodecParameters * par = fctx->streams[ai]->codecpar;
        const AVCodec * dec = avcodec_find_decoder(par->codec_id);
        if (!dec) {
            avformat_close_input( &fctx);
            decode_complete = true;
            return;
        }
        AVCodecContext * cctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cctx, par);
        if (avcodec_open2(cctx, dec, nullptr) < 0) {
            avcodec_free_context( &cctx);
            avformat_close_input( &fctx);
            decode_complete = true;
            return;
        }
        constexpr int out_rate = 44100;
        SwrContext * swr = swr_alloc();
        av_opt_set_chlayout(swr, "in_chlayout", &cctx->ch_layout, 0);
        av_opt_set_int(swr, "in_sample_rate", cctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", cctx->sample_fmt, 0);
        AVChannelLayout out_ch;
        av_channel_layout_default( &out_ch, 2);
        av_opt_set_chlayout(swr, "out_chlayout", &out_ch, 0);
        av_opt_set_int(swr, "out_sample_rate", out_rate, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        swr_init(swr);
        av_channel_layout_uninit( &out_ch);
        AVPacket * pkt = av_packet_alloc();
        AVFrame * frm = av_frame_alloc();
        std::vector<int16_t> tbuf(out_rate * 2);
        size_t tcount = 0;
        constexpr size_t MAX_FRAMES = 44100ULL * 7200ULL;
        auto flush_to_pcm = [ & ]() {
            std::lock_guard<std::mutex> lk(pcm_mutex);
            pcm_data.insert(pcm_data.end(), tbuf.begin(), tbuf.begin() + tcount);
            total_frames.store(pcm_data.size() / 2, std::memory_order_relaxed);
            tcount = 0;
        };
        while (!abort_decode.load(std::memory_order_relaxed) && !stop_requested.load(std::memory_order_relaxed)) {
            if (total_frames.load(std::memory_order_relaxed) >= MAX_FRAMES)break;
            if (av_read_frame(fctx, pkt) < 0)break;
            if (pkt->stream_index == ai) {
                if (avcodec_send_packet(cctx, pkt) == 0) {
                    while (avcodec_receive_frame(cctx, frm) == 0) {
                        int n = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) + frm->nb_samples, out_rate, cctx->sample_rate, AV_ROUND_UP);
                        if (n > 0) {
                            if (tcount + (size_t)n * 2 > tbuf.size())tbuf.resize(tcount + (size_t)n * 2 + out_rate);
                            uint8_t *op[1] = {
                                reinterpret_cast<uint8_t * >(tbuf.data() + tcount)
                            };
                            int conv = swr_convert(swr, op, n, const_cast<const uint8_t * * >(frm->extended_data), frm->nb_samples);
                            if (conv > 0)tcount += (size_t)conv * 2;
                        }
                        if (tcount >= 16384)flush_to_pcm();
                    }
                }
            }
            av_packet_unref(pkt);
        }
        if (!abort_decode.load(std::memory_order_relaxed) && !stop_requested.load(std::memory_order_relaxed)) {
            avcodec_send_packet(cctx, nullptr);
            while (avcodec_receive_frame(cctx, frm) == 0) {
                int n = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) + frm->nb_samples, out_rate, cctx->sample_rate, AV_ROUND_UP);
                if (n > 0) {
                    if (tcount + (size_t)n * 2 > tbuf.size())tbuf.resize(tcount + (size_t)n * 2 + out_rate);
                    uint8_t *op[1] = {
                        reinterpret_cast<uint8_t * >(tbuf.data() + tcount)
                    };
                    int conv = swr_convert(swr, op, n, const_cast<const uint8_t * * >(frm->extended_data), frm->nb_samples);
                    if (conv > 0)tcount += (size_t)conv * 2;
                }
            }
        }
        if (tcount > 0)flush_to_pcm();
        av_frame_free( &frm);
        av_packet_free( &pkt);
        swr_free( &swr);
        avcodec_free_context( &cctx);
        avformat_close_input( &fctx);
        decode_complete.store(true, std::memory_order_release);
        wake_ui();
    }
    void run() {
        while (!stop_requested.load(std::memory_order_relaxed)) {
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
    void decode_loop(const std::string & path) {
        AVFormatContext * fctx = nullptr;
        if (avformat_open_input( &fctx, path.c_str(), nullptr, nullptr) < 0)return;
        if (avformat_find_stream_info(fctx, nullptr) < 0) {
            avformat_close_input( &fctx);
            return;
        }
        int ai = -1;
        for (unsigned int i = 0; i < fctx->nb_streams; ++i)if (fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ai = i;
            break;
        }
        if (ai == -1) {
            avformat_close_input( &fctx);
            return;
        }
        AVCodecParameters * par = fctx->streams[ai]->codecpar;
        const AVCodec * dec = avcodec_find_decoder(par->codec_id);
        AVCodecContext * cctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cctx, par);
        if (avcodec_open2(cctx, dec, nullptr) < 0) {
            avcodec_free_context( &cctx);
            avformat_close_input( &fctx);
            return;
        }
        TrackMetadata lm;
        AVDictionaryEntry * tag = nullptr;
        if ((tag = av_dict_get(fctx->metadata, "title", nullptr, 0)))lm.title = tag->value;
        if ((tag = av_dict_get(fctx->metadata, "artist", nullptr, 0)))lm.artist = tag->value;
        if ((tag = av_dict_get(fctx->metadata, "album", nullptr, 0)))lm.album = tag->value;
        lm.codec_name = dec->name;
        lm.bit_rate = fctx->bit_rate;
        lm.sample_rate = cctx->sample_rate;
        lm.channels = cctx->ch_layout.nb_channels;
        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            active_meta.release_memory();
            last_played_path = path;
            active_meta = std::move(lm);
            metadata_updated.store(true, std::memory_order_release);
        }
        wake_ui();
        duration.store((fctx->duration != AV_NOPTS_VALUE) ? (double)fctx->duration / AV_TIME_BASE : 0.0, std::memory_order_relaxed);
        uint64_t my_gen = ++cover_gen;
        int tw = std::clamp(cover_target_w.load(std::memory_order_relaxed), 48, 800);
        int th = std::clamp(cover_target_h.load(std::memory_order_relaxed), 48, 800);
        std::thread([this, path, my_gen, tw, th]() {
            extract_album_art_async(path, tw, th, my_gen);
        }).detach();
        avcodec_free_context( &cctx);
        avformat_close_input( &fctx);
        abort_decode.store(false, std::memory_order_relaxed);
        decode_complete.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(pcm_mutex);
            std::vector<int16_t>().swap(pcm_data);
        }
        total_frames.store(0, std::memory_order_relaxed);
        std::thread dec_th( &AudioEngine::fill_pcm_buffer, this, path);
        while (!decode_complete.load(std::memory_order_relaxed) && total_frames.load(std::memory_order_relaxed) < 4096 && !abort_decode.load(std::memory_order_relaxed) && !stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (pcm)snd_pcm_prepare(pcm);
        is_playing.store(true, std::memory_order_release);
        is_paused.store(false, std::memory_order_relaxed);
        double pos = 0.0;
        if (is_reverse.load(std::memory_order_relaxed)) {
            size_t tf0 = total_frames.load(std::memory_order_relaxed);
            pos = tf0 > 0 ? (double)(tf0 - 1) : 0.0;
        }
        constexpr int CHUNK = 1024;
        std::vector<int16_t> out(CHUNK * 2);
        bool eof = false;
        while (is_playing.load(std::memory_order_relaxed) && !stop_requested.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lk(cmd_mutex);
                if (track_switch) {
                    eof = false;
                    break;
                }
            }
            if (is_paused.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            double s = seek_req.exchange( -1.0);
            if (s >= 0.0)pos = std::clamp(s * 44100.0, 0.0, (double)total_frames.load(std::memory_order_relaxed));
            bool rev = is_reverse.load(std::memory_order_relaxed);
            double spd = std::clamp(speed.load(std::memory_order_relaxed), 0.10, 3.0);
            double vol = is_muted.load(std::memory_order_relaxed) ? 0.0 : volume.load(std::memory_order_relaxed);
            size_t tf = total_frames.load(std::memory_order_relaxed);
            if (!rev) {
                if (pos >= tf) {
                    if (decode_complete.load(std::memory_order_acquire)) {
                        eof = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
            } else if (pos <= 0.0) {
                eof = true;
                break;
            }
            int rendered = 0;
            {
                std::lock_guard<std::mutex> lk(pcm_mutex);
                size_t cur_tf = pcm_data.size() / 2;
                if (cur_tf == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                for (int i = 0; i < CHUNK; ++i) {
                    if (!rev && pos >= cur_tf) {
                        if (decode_complete.load(std::memory_order_acquire))eof = true;
                        break;
                    }
                    if (rev && pos < 0.0) {
                        eof = true;
                        break;
                    }
                    double f = pos;
                    int64_t i0 = (int64_t)std::floor(f);
                    if (i0 < 0)i0 = 0;
                    if ((size_t)i0 >= cur_tf)i0 = cur_tf - 1;
                    int64_t i1 = ((size_t)(i0 + 1) < cur_tf) ? i0 + 1 : i0;
                    double frac = f - std::floor(f);
                    int32_t l0 = pcm_data[i0 * 2], r0 = pcm_data[i0 * 2 + 1];
                    int32_t l1 = pcm_data[i1 * 2], r1 = pcm_data[i1 * 2 + 1];
                    int32_t l = (int32_t)(l0 + (l1 - l0) * frac);
                    int32_t r = (int32_t)(r0 + (r1 - r0) * frac);
                    l = (int32_t)(l * vol);
                    r = (int32_t)(r * vol);
                    out[rendered * 2] = (int16_t)std::clamp(l, -32768, 32767);
                    out[rendered * 2 + 1] = (int16_t)std::clamp(r, -32768, 32767);
                    rendered++;
                    if (!rev)pos += spd;
                    else pos -= spd;
                }
            }
            cur_pts.store(std::clamp(pos / 44100.0, 0.0, duration.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            if (pcm && rendered > 0) {
                int left = rendered;
                int16_t *p = out.data();
                while (left > 0 && is_playing.load(std::memory_order_relaxed) && !stop_requested.load(std::memory_order_relaxed)) {
                    snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, left);
                    if (w < 0) {
                        w = snd_pcm_recover(pcm, w, 1);
                        if (w < 0)break;
                    } else {
                        p += w * 2;
                        left -= w;
                    }
                }
            }
            if (eof)break;
        }
        abort_decode.store(true, std::memory_order_relaxed);
        if (dec_th.joinable())dec_th.join();
        is_playing.store(false, std::memory_order_relaxed);
        if (eof && !stop_requested.load(std::memory_order_relaxed)) {
            track_finished.store(true, std::memory_order_release);
            wake_ui();
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
    bool seek_dragging = false;
    PlayMode mode = PlayMode::NORMAL;
    int last_idx = -1, last_scroll = -1;
    static constexpr double RIGHT_FRAC = 0.40;
    std::string del_target_name, del_target_path;
    bool del_target_is_dir = false;
    Display * dpy = nullptr;
    int scr = 0;
    Window win = 0;
    GC gc = nullptr;
    Atom wmDelete = 0;
    int x_fd = -1;
    Visual * draw_vis = nullptr;
    Colormap draw_cmap = 0;
    int draw_depth = 24;
    bool have_argb_window = false;
    Pixmap backbuf = 0;
    Picture backbuf_pic = 0;
    Picture win_pic = 0;
    XftFont * font = nullptr;
    std::unordered_map<FcChar32, XftFont * > glyph_font_cache;
    XftDraw * xft_draw = nullptr;
    int CELL_W = 10, CELL_H = 18, FONT_ASCENT = 14;
    XIM xim = nullptr;
    XIC xic = nullptr;
    std::unordered_map<uint32_t, XftColor> xft_colors;
    XImage * cur_cover_img = nullptr;
    std::string cur_cover_path;
    int cur_cover_w = 0, cur_cover_h = 0;
    int W = 1200, H = 760;
    MothApp() {
        dpy = XOpenDisplay(nullptr);
        if (!dpy) {
            fprintf(stderr, "Cannot open X display\n");
            std::exit(1);
        }
        scr = DefaultScreen(dpy);
        x_fd = ConnectionNumber(dpy);
        wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        font = XftFontOpenName(dpy, scr, FONT_PRIMARY);
        if (!font)font = XftFontOpenName(dpy, scr, FONT_FALLBACK1);
        if (!font) {
            fprintf(stderr, "Cannot load any font\n");
            std::exit(1);
        }
        XGlyphInfo gi;
        XftTextExtentsUtf8(dpy, font, (const FcChar8 * )"M", 1, &gi);
        CELL_W = std::max(4, (int)gi.xOff);
        CELL_H = std::max(8, font->height);
        FONT_ASCENT = font->ascent;
        W = CELL_W * 120;
        H = CELL_H * 40;
        Atom cm_atom = XInternAtom(dpy, "_NET_WM_CM_S0", False);
        bool have_compositor = (XGetSelectionOwner(dpy, cm_atom) != None);
        XVisualInfo vinfo;
        if (have_compositor && XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) {
            have_argb_window = true;
            draw_vis = vinfo.visual;
            draw_depth = 32;
            draw_cmap = XCreateColormap(dpy, RootWindow(dpy, scr), draw_vis, AllocNone);
        } else {
            have_argb_window = false;
            draw_vis = DefaultVisual(dpy, scr);
            draw_depth = DefaultDepth(dpy, scr);
            draw_cmap = DefaultColormap(dpy, scr);
        }
        const long evmask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
        if (have_argb_window) {
            XSetWindowAttributes attrs {
            };
            attrs.colormap = draw_cmap;
            attrs.background_pixel = 0;
            attrs.border_pixel = 0;
            attrs.bit_gravity = NorthWestGravity;
            attrs.event_mask = evmask;
            win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0, draw_depth, InputOutput, draw_vis, CWColormap | CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask, &attrs);
        } else {
            win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0, BlackPixel(dpy, scr), BlackPixel(dpy, scr));
            XSelectInput(dpy, win, evmask);
        }
        XStoreName(dpy, win, "Moth Player");
        XSetWMProtocols(dpy, win, &wmDelete, 1);
        XMapWindow(dpy, win);
        recreate_backbuffer();
        XRenderPictFormat * win_fmt = XRenderFindVisualFormat(dpy, draw_vis);
        win_pic = XRenderCreatePicture(dpy, win, win_fmt, 0, nullptr);
        xim = XOpenIM(dpy, nullptr, nullptr, nullptr);
        if (xim) {
            xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win, nullptr);
        }
        path = fs::current_path();
        scan();
    }
    ~MothApp() {
        if (cur_cover_img) {
            XDestroyImage(cur_cover_img);
            cur_cover_img = nullptr;
        }
        for (auto &kv : xft_colors)XftColorFree(dpy, draw_vis, draw_cmap, &kv.second);
        xft_colors.clear();
        for (auto & [cp, f] : glyph_font_cache)if (f && f != font)XftFontClose(dpy, f);
        glyph_font_cache.clear();
        if (xft_draw) {
            XftDrawDestroy(xft_draw);
            xft_draw = nullptr;
        }
        if (backbuf_pic) {
            XRenderFreePicture(dpy, backbuf_pic);
            backbuf_pic = 0;
        }
        if (backbuf) {
            XFreePixmap(dpy, backbuf);
            backbuf = 0;
        }
        if (gc) {
            XFreeGC(dpy, gc);
            gc = nullptr;
        }
        if (win_pic) {
            XRenderFreePicture(dpy, win_pic);
            win_pic = 0;
        }
        if (font) {
            XftFontClose(dpy, font);
            font = nullptr;
        }
        if (xic) {
            XDestroyIC(xic);
            xic = nullptr;
        }
        if (xim) {
            XCloseIM(xim);
            xim = nullptr;
        }
        if (win) {
            XDestroyWindow(dpy, win);
            win = 0;
        }
        if (have_argb_window && draw_cmap) {
            XFreeColormap(dpy, draw_cmap);
            draw_cmap = 0;
        }
        if (dpy) {
            XCloseDisplay(dpy);
            dpy = nullptr;
        }
    }
    void recreate_backbuffer() {
        if (xft_draw) {
            XftDrawDestroy(xft_draw);
            xft_draw = nullptr;
        }
        if (backbuf_pic) {
            XRenderFreePicture(dpy, backbuf_pic);
            backbuf_pic = 0;
        }
        if (backbuf) {
            XFreePixmap(dpy, backbuf);
            backbuf = 0;
        }
        if (gc) {
            XFreeGC(dpy, gc);
            gc = nullptr;
        }
        backbuf = XCreatePixmap(dpy, win, W, H, draw_depth);
        gc = XCreateGC(dpy, backbuf, 0, nullptr);
        XRenderPictFormat * fmt = XRenderFindVisualFormat(dpy, draw_vis);
        backbuf_pic = XRenderCreatePicture(dpy, backbuf, fmt, 0, nullptr);
        xft_draw = XftDrawCreate(dpy, backbuf, draw_vis, draw_cmap);
        invalidate_cover_cache();
    }
    XftColor * get_xft_color(uint32_t argb) {
        auto it = xft_colors.find(argb);
        if (it != xft_colors.end())return & it->second;
        XRenderColor xr = argb_to_xrender(argb);
        XftColor xc {
        };
        if (!XftColorAllocValue(dpy, draw_vis, draw_cmap, &xr, &xc)) {
            xc.pixel = 0;
            xc.color.red = xc.color.green = xc.color.blue = xc.color.alpha = 0xFFFF;
        }
        auto[iter, _] = xft_colors.emplace(argb, xc);
        return & iter->second;
    }
    int cols()const {
        return W / CELL_W;
    }
    int rows()const {
        return H / CELL_H;
    }
    void clear_all() {
        XRenderColor bg = argb_to_xrender(COLOR_BG);
        XRenderFillRectangle(dpy, PictOpSrc, backbuf_pic, &bg, 0, 0, W, H);
    }
    void fill_rect_px(int x, int y, int w, int h, uint32_t argb) {
        if (w <= 0 || h <= 0)return;
        XRenderColor c = argb_to_xrender(argb);
        XRenderFillRectangle(dpy, PictOpSrc, backbuf_pic, &c, x, y, w, h);
    }
    void fill_cells(int col, int row, int wcols, int hrows, uint32_t argb) {
        fill_rect_px(col * CELL_W, row * CELL_H, wcols * CELL_W, hrows * CELL_H, argb);
    }
    XftFont * pick_font(FcChar32 cp) {
        if (XftCharExists(dpy, font, cp))return font;
        auto it = glyph_font_cache.find(cp);
        if (it != glyph_font_cache.end())return it->second;
        FcPattern * pat = FcPatternCreate();
        FcCharSet * cs = FcCharSetCreate();
        FcCharSetAddChar(cs, cp);
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
        FcConfigSubstitute(nullptr, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult result = FcResultNoMatch;
        FcPattern * match = FcFontMatch(nullptr, pat, &result);
        FcPatternDestroy(pat);
        FcCharSetDestroy(cs);
        XftFont * chosen = font;
        if (match) {
            XftFont * candidate = XftFontOpenPattern(dpy, match);
            if (candidate && XftCharExists(dpy, candidate, cp)) {
                chosen = candidate;
            } else if (candidate) {
                XftFontClose(dpy, candidate);
            }
        }
        glyph_font_cache.emplace(cp, chosen);
        return chosen;
    }
    void text_at(int col, int row, const std::string & s, uint32_t argb) {
        if (s.empty() || !xft_draw)return;
        XftColor * xc = get_xft_color(argb);
        int x = col * CELL_W;
        int y = row * CELL_H + FONT_ASCENT;
        const unsigned char *p = (const unsigned char * )s.data();
        const unsigned char *end = p + s.size();
        while (p < end) {
            FcChar32 cp;
            int n = FcUtf8ToUcs4((const FcChar8 * )p, &cp, (int)(end - p));
            if (n <= 0)break;
            XftFont * run_font = pick_font(cp);
            const unsigned char *run_start = p;
            p += n;
            while (p < end) {
                FcChar32 cp2;
                int n2 = FcUtf8ToUcs4((const FcChar8 * )p, &cp2, (int)(end - p));
                if (n2 <= 0 || pick_font(cp2) != run_font)break;
                p += n2;
            }
            int run_bytes = (int)(p - run_start);
            XftDrawStringUtf8(xft_draw, xc, run_font, x, y, (const FcChar8 * )run_start, run_bytes);
            std::string run((const char * )run_start, run_bytes);
            x += utf8_display_width(run) * CELL_W;
        }
    }
    void scan() {
        entries.clear();
        if (path.has_parent_path() && path != path.parent_path())entries.push_back( {
            "..", (path / "..").lexically_normal().string(), true, false
        });
        std::vector<FileItem> dirs, regular;
        try {
            for (const auto &e : fs::directory_iterator(path)) {
                if (e.is_directory()) {
                    dirs.push_back( {
                        e.path().filename().string(), e.path().string(), true, false
                    });
                } else {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a")regular.push_back( {
                        e.path().filename().string(), e.path().string(), false, true
                    });
                }
            }
        } catch ( ... ) {
        }
        std::sort(dirs.begin(), dirs.end(), [](auto &a, auto &b) {
            return a.name < b.name;
        });
        std::sort(regular.begin(), regular.end(), [](auto &a, auto &b) {
            return a.name < b.name;
        });
        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), regular.begin(), regular.end());
        if (idx >= (int)entries.size())idx = std::max(0, (int)entries.size() - 1);
        last_idx = -1;
        last_scroll = -1;
        need_redraw = true;
    }
    void go_up_dir() {
        if (!path.has_parent_path() || path == path.parent_path())return;
        std::string came_from = path.filename().string();
        fs::path parent = path.parent_path();
        try {
            path = fs::canonical(parent);
        } catch ( ... ) {
            path = parent;
        }
        scan();
        if (!came_from.empty()) {
            for (size_t i = 0; i < entries.size(); ++i)if (entries[i].name == came_from && entries[i].is_dir) {
                idx = (int)i;
                break;
            }
        }
        need_redraw = true;
    }
    void enter_dir(const fs::path & p) {
        try {
            path = fs::canonical(p);
        } catch ( ... ) {
            path = p;
        }
        scan();
        need_redraw = true;
    }
    void play_next_track(AudioEngine & audio) {
        if (entries.empty())return;
        int n = idx + 1;
        while (n < (int)entries.size() && !entries[n].is_audio)n++;
        if (n < (int)entries.size()) {
            idx = n;
            audio.load(entries[idx].path);
        } else audio.stop();
        need_redraw = true;
    }
    static bool ci_find(const std::string & str, const std::string & sub) {
        if (sub.empty())return true;
        auto it = std::search(str.begin(), str.end(), sub.begin(), sub.end(), [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
        return it != str.end();
    }
    void search_next(bool from_current = false) {
        if (last_query.empty() || entries.empty())return;
        int start = from_current ? idx : (idx + 1) % entries.size();
        for (size_t off = 0; off < entries.size(); ++off) {
            int cur = (start + off) % entries.size();
            if (ci_find(entries[cur].name, last_query)) {
                idx = cur;
                need_redraw = true;
                return;
            }
        }
    }
    static std::string sec_to_str(double t) {
        int s = (int)t;
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
        return b;
    }
    struct PanelGeom {
        int split_x, side_w;
        int cover_px_w, cover_px_h;
        int art_box_y, art_rows, meta_y;
    };
    PanelGeom compute_geom(int max_y, int max_x)const {
        PanelGeom g {
        };
        int right_w = std::clamp((int)(max_x * RIGHT_FRAC), 28, std::max(28, max_x - 22));
        g.split_x = std::max(22, max_x - right_w);
        g.side_w = max_x - g.split_x - 1;
        g.art_box_y = 1;
        const int meta_lines = 7;
        g.meta_y = std::max(g.art_box_y + 4, (max_y - 2) - meta_lines);
        g.art_rows = std::max(3, g.meta_y - (g.art_box_y + 1));
        g.cover_px_w = std::clamp((g.side_w - 4) * CELL_W, 48, 512);
        g.cover_px_h = std::clamp(g.art_rows * CELL_H, 48, 512);
        return g;
    }
    void invalidate_cover_cache() {
        if (cur_cover_img) {
            XDestroyImage(cur_cover_img);
            cur_cover_img = nullptr;
        }
        cur_cover_w = cur_cover_h = 0;
    }
    XImage * make_ximage_rgb(const uint8_t *rgb, int w, int h) {
        if (w <= 0 || h <= 0)return nullptr;
        XImage * img = XCreateImage(dpy, draw_vis, draw_depth, ZPixmap, 0, nullptr, w, h, 32, 0);
        if (!img)return nullptr;
        img->data = (char * )calloc((size_t)img->bytes_per_line, h);
        if (!img->data) {
            XDestroyImage(img);
            return nullptr;
        }
        const bool lsb = (img->byte_order == LSBFirst);
        for (int y = 0; y < h; ++y) {
            char *row = img->data + (size_t)y * img->bytes_per_line;
            const uint8_t *s = rgb + (size_t)y * w * 3;
            for (int x = 0; x < w; ++x) {
                uint8_t r = s[x * 3], g = s[x * 3 + 1], b = s[x * 3 + 2];
                if (img->bits_per_pixel == 32) {
                    if (lsb) {
                        row[x * 4] = (char)b;
                        row[x * 4 + 1] = (char)g;
                        row[x * 4 + 2] = (char)r;
                        row[x * 4 + 3] = (char)0xFF;
                    } else {
                        row[x * 4] = (char)0xFF;
                        row[x * 4 + 1] = (char)r;
                        row[x * 4 + 2] = (char)g;
                        row[x * 4 + 3] = (char)b;
                    }
                } else if (img->bits_per_pixel == 24) {
                    if (lsb) {
                        row[x * 3] = (char)b;
                        row[x * 3 + 1] = (char)g;
                        row[x * 3 + 2] = (char)r;
                    } else {
                        row[x * 3] = (char)r;
                        row[x * 3 + 1] = (char)g;
                        row[x * 3 + 2] = (char)b;
                    }
                } else {
                    XPutPixel(img, x, y, (r << 16) | (g << 8) | b);
                }
            }
        }
        return img;
    }
    void draw_cover(const TrackMetadata & meta, int split_x, int art_box_y, int art_rows, int side_w) {
        const int base_row = art_box_y + 1;
        const int base_col = split_x + 2;
        const int area_cols = std::max(4, side_w - 2);
        for (int r = 0; r < art_rows; ++r)fill_cells(base_col, base_row + r, area_cols, 1, COLOR_BG);
        if (!meta.has_cover || meta.cover_rgb.empty() || meta.cover_w <= 0) {
            std::string msg = "no artwork";
            int mc = utf8_display_width(msg);
            text_at(base_col + std::max(0, (area_cols - mc) / 2), base_row + std::max(0, art_rows / 2), msg, COLOR_DIM);
            return;
        }
        if (!cur_cover_img || cur_cover_w != meta.cover_w || cur_cover_h != meta.cover_h) {
            invalidate_cover_cache();
            cur_cover_img = make_ximage_rgb(meta.cover_rgb.data(), meta.cover_w, meta.cover_h);
            cur_cover_w = meta.cover_w;
            cur_cover_h = meta.cover_h;
        }
        if (!cur_cover_img)return;
        int img_cols = std::max(1, (meta.cover_w + CELL_W - 1) / CELL_W);
        if (img_cols > area_cols)img_cols = area_cols;
        int off_c = std::max(0, (area_cols - img_cols) / 2);
        XPutImage(dpy, backbuf, gc, cur_cover_img, 0, 0, (base_col + off_c) * CELL_W, base_row * CELL_H, meta.cover_w, meta.cover_h);
    }
    void draw_entry_line(int y, int entry_idx, int split_x, bool is_selected) {
        if (split_x <= 0)return;
        fill_cells(0, y, split_x, 1, is_selected ? COLOR_ACCENT : COLOR_BG);
        if (entry_idx >= 0 && entry_idx < (int)entries.size()) {
            const auto &it = entries[entry_idx];
            char prefix = it.is_dir ? '/' : ' ';
            int max_cols = std::max(0, split_x - 3);
            int dw = 0;
            std::string name = truncate_utf8(it.name, max_cols, &dw);
            int pad = std::max(0, split_x - 2 - dw);
            std::string line;
            line.reserve((size_t)split_x + 4);
            line += ' ';
            line += prefix;
            line += name;
            line.append((size_t)pad, ' ');
            text_at(0, y, line, is_selected ? COLOR_ACCENT_FG : COLOR_FG);
        }
    }
    void draw_meta_line(int &meta_y, int split_x, int side_w, int max_y, const char *label, const std::string & val) {
        if (meta_y >= max_y - 2)return;
        int remaining = cols() - (split_x + 1);
        fill_cells(split_x + 1, meta_y, remaining, 1, COLOR_BG);
        int max_val = std::max(0, side_w - 10);
        std::string vs = truncate_utf8(val, max_val);
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "%-8s %s", label, vs.c_str());
        if (n > 0)text_at(split_x + 2, meta_y, std::string(buf, (size_t)n), COLOR_FG);
        meta_y++;
    }
    std::string build_status(AudioEngine & audio)const {
        std::string status;
        if (audio.is_playing) {
            if (audio.is_paused)status = audio.is_reverse ? "PAUSED[REV]" : "PAUSED ";
            else status = audio.is_reverse ? "REV-PLAY" : "PLAYING ";
        } else {
            status = audio.is_reverse ? "STOPPED[REV]" : "STOPPED";
        }
        std::string mode_str = (mode == PlayMode::LOOP) ? "LOOP" : (mode == PlayMode::SEQUENTIAL ? "SEQ" : "NORMAL");
        int vol = (int)(audio.volume.load() * 100.0);
        int spd = (int)std::round(audio.speed.load() * 100.0);
        char buf[512];
        if (audio.is_muted.load()) {
            snprintf(buf, sizeof(buf), " [%s|%s] %s/%s | Vol: MUTE | Speed: %3d%% [F1: Help] ", status.c_str(), mode_str.c_str(), sec_to_str(audio.cur_pts.load()).c_str(), sec_to_str(audio.duration.load()).c_str(), spd);
        } else {
            snprintf(buf, sizeof(buf), " [%s|%s] %s/%s | Vol: %3d%% | Speed: %3d%% [F1: Help] ", status.c_str(), mode_str.c_str(), sec_to_str(audio.cur_pts.load()).c_str(), sec_to_str(audio.duration.load()).c_str(), vol, spd);
        }
        return std::string(buf);
    }
    void draw_status(AudioEngine & audio, int max_x, int y) {
        std::string s = build_status(audio);
        fill_cells(0, y, max_x, 1, COLOR_ACCENT);
        text_at(0, y, s, COLOR_ACCENT_FG);
    }
    void progress_geom(int max_x, int y, int &x0, int &x1, int &cy, int &bar_w)const {
        bar_w = std::max(4, max_x - 6);
        x0 = CELL_W;
        x1 = CELL_W * (2 + bar_w);
        cy = y * CELL_H + CELL_H / 2;
    }
    double x_to_ratio(int mx, int max_x, int y)const {
        int x0, x1, cy, bar_w;
        progress_geom(max_x, y, x0, x1, cy, bar_w);
        if (mx < x0)mx = x0;
        if (mx > x1)mx = x1;
        return (double)(mx - x0) / (double)std::max(1, x1 - x0);
    }
    void draw_progress_bar(AudioEngine & audio, int max_x, int y) {
        fill_cells(0, y, max_x, 1, COLOR_BG);
        if (is_command_mode) {
            std::string c = truncate_utf8(command_query, std::max(0, max_x - 4));
            text_at(1, y, ":" + c, COLOR_FG);
            int cur = 2 + utf8_display_width(c);
            if (cur < max_x - 1) {
                int cx = cur * CELL_W;
                fill_rect_px(cx, y * CELL_H + 3, 2, CELL_H - 6, COLOR_ACCENT);
            }
        } else if (is_searching) {
            std::string q = truncate_utf8(search_query, std::max(0, max_x - 4));
            text_at(1, y, "/" + q, COLOR_FG);
            int cur = 2 + utf8_display_width(q);
            if (cur < max_x - 1) {
                int cx = cur * CELL_W;
                fill_rect_px(cx, y * CELL_H + 3, 2, CELL_H - 6, COLOR_ACCENT);
            }
        } else {
            int x0, x1, cy, bar_w;
            progress_geom(max_x, y, x0, x1, cy, bar_w);
            double dur = audio.duration.load();
            double ratio = (dur > 0.0) ? std::clamp(audio.cur_pts.load() / dur, 0.0, 1.0) : 0.0;
            int filled = (int)std::llround(ratio * bar_w);
            fill_rect_px(x0 - 2, cy - CELL_H / 3, 2, CELL_H * 2 / 3, COLOR_FG);
            fill_rect_px(x1, cy - CELL_H / 3, 2, CELL_H * 2 / 3, COLOR_FG);
            fill_rect_px(x0, cy, x1 - x0, 1, COLOR_DIM);
            int fx_end = x0 + (x1 - x0) * filled / bar_w;
            fill_rect_px(x0, cy, fx_end - x0, 1, COLOR_ACCENT);
            int thumb_x = x0 + (x1 - x0) * filled / bar_w;
            fill_rect_px(thumb_x - 2, cy - CELL_H / 3, 4, CELL_H * 2 / 3, COLOR_ACCENT);
        }
    }
    void draw_dialog_frame(int lx, int ty, int dw, int dh, uint32_t bg, uint32_t border) {
        int x0 = lx * CELL_W;
        int y0 = ty * CELL_H;
        int x1 = (lx + dw) * CELL_W;
        int y1 = (ty + dh) * CELL_H;
        fill_rect_px(x0, y0, x1 - x0, y1 - y0, bg);
        fill_rect_px(x0, y0, x1 - x0, 1, border);
        fill_rect_px(x0, y1 - 1, x1 - x0, 1, border);
        fill_rect_px(x0, y0, 1, y1 - y0, border);
        fill_rect_px(x1 - 1, y0, 1, y1 - y0, border);
    }
    void render_del_dialog(int max_y, int max_x) {
        int dw = std::min(60, max_x - 4), dh = 9;
        int ty = (max_y - dh) / 2, lx = (max_x - dw) / 2;
        draw_dialog_frame(lx, ty, dw, dh, COLOR_DANGER_BG, COLOR_DANGER_BORDER);
        auto c = [ & ](int r, const std::string & s) {
            int sc = utf8_display_width(s);
            text_at(lx + (dw - sc) / 2, ty + r, s, COLOR_DANGER_FG);
        };
        c(1, "Delete");
        c(3, "Are you sure you wanna del this file?");
        c(5, "> " + truncate_utf8(del_target_name, dw - 8) + " <");
        c(7, "[Y] Yes, Delete!     [N / Esc] Cancel");
    }
    void render_help_dialog(int max_y, int max_x) {
        int dw = std::min(64, max_x - 4), dh = std::min(24, max_y - 2);
        int ty = (max_y - dh) / 2, lx = (max_x - dw) / 2;
        draw_dialog_frame(lx, ty, dw, dh, COLOR_DIALOG_BG, COLOR_DIALOG_BORDER);
        {
            std::string t = "Key bindings";
            int tc = utf8_display_width(t);
            text_at(lx + (dw - tc) / 2, ty + 1, t, COLOR_ACCENT);
        }
        int row = ty + 3;
        auto item = [ & ](const char *keys, const char *desc) {
            if (row < ty + dh - 1) {
                char b[256];
                snprintf(b, sizeof(b), "%-17s  %s", keys, desc);
                text_at(lx + 3, row, b, COLOR_DIALOG_FG);
                row++;
            }
        };
        item("Space", "Play / Enter Directory");
        item("Backspace", "Go to Parent Directory");
        item("p / c", "Pause / Resume");
        item("r", "Toggle Reverse Playback");
        item(":", "Vim Cmd: :speed, :vol, :seek, :del");
        item("v", "Stop playback");
        item("m", "Toggle Mute");
        item("Left / Right", "Seek -3s / +3s");
        item("S-Left / S-Right", "Seek -1s / +1s");
        item("0 - 9", "Instant Seek 0% - 90%");
        item("[ / ]", "Varispeed -5% / +5%");
        item("{ / }", "Varispeed -1% / +1%");
        item("LMB / RMB on Vol", "Decrease / Increase Volume");
        item("LMB / RMB on Speed", "Decrease / Increase Speed");
        item("Drag on Progress", "Scrub");
        item("l / s / f", "Loop / Sequential / Normal");
        item("PgUp / PgDn", "Scroll 5 items");
        item("g / G", "Jump to Top / Bottom");
        item("Mouse Wheel", "Smooth Up / Down scroll");
        item("/ | ?", "Find track | Next match");
        item("Home", "Reset Modifiers");
        item("F1 / Esc", "Close Help");
        item("F2", "About");
    }
    void render_about_dialog(int max_y, int max_x) {
        int dw = std::min(56, max_x - 4), dh = std::min(15, max_y - 2);
        int ty = (max_y - dh) / 2, lx = (max_x - dw) / 2;
        draw_dialog_frame(lx, ty, dw, dh, COLOR_DIALOG_BG, COLOR_DIALOG_BORDER);
        {
            std::string t = "About";
            int tc = utf8_display_width(t);
            text_at(lx + (dw - tc) / 2, ty + 1, t, COLOR_ACCENT);
        }
        int row = ty + 3;
        auto line = [ & ](const char *s) {
            if (row < ty + dh - 1) {
                text_at(lx + 3, row, s, COLOR_DIALOG_FG);
                row++;
            }
        };
        line("Moth Player - X11 audio player");
        line("");
        line("Audio   : FFmpeg decode + ALSA");
        line("Cover   : FFmpeg -> XImage ARGB32");
        line("UI      : Xft + Xrender RGBA");
        line("");
        line("F1 Help   |  F2 About  |  q Quit");
        line("Made with <3 by @HalanoSiblee The Smart moth");
        line("");
        line(APP_V);
    }
    [[gnu::hot]]void render_full(AudioEngine & audio) {
        int max_x = cols(), max_y = rows();
        clear_all();
        if (max_y < 8 || max_x < 30) {
            text_at(0, 0, "Window too small.", COLOR_FG);
            XRenderComposite(dpy, PictOpSrc, backbuf_pic, None, win_pic, 0, 0, 0, 0, 0, 0, W, H);
            XFlush(dpy);
            return;
        }
        auto g = compute_geom(max_y, max_x);
        const int split_x = g.split_x;
        const int view_h = max_y - 3;
        audio.cover_target_w = g.cover_px_w;
        audio.cover_target_h = g.cover_px_h;
        if (idx < scroll)scroll = idx;
        if (idx >= scroll + view_h)scroll = idx - view_h + 1;
        {
            int mc = std::max(0, max_x - 11);
            std::string hp = truncate_utf8(path.string(), mc);
            std::string hdr = "Browser: " + hp;
            int hc = utf8_display_width(hdr);
            int pad = std::max(0, max_x - hc);
            fill_cells(0, 0, max_x, 1, COLOR_ACCENT);
            hdr.append((size_t)pad, ' ');
            text_at(0, 0, hdr, COLOR_ACCENT_FG);
        }
        for (int i = 0; i < view_h; ++i)draw_entry_line(i + 1, scroll + i, split_x, (scroll + i) == idx);
        for (int y = 1; y < max_y - 2; ++y) {
            int remaining = max_x - (split_x + 1);
            fill_cells(split_x + 1, y, remaining, 1, COLOR_BG);
        }
        {
            int sep_x = split_x * CELL_W + CELL_W / 2;
            int sep_y0 = CELL_H;
            int sep_y1 = (max_y - 2) * CELL_H;
            fill_rect_px(sep_x, sep_y0, 1, sep_y1 - sep_y0, COLOR_DIM);
        }
        TrackMetadata meta;
        std::string cur_path;
        {
            std::lock_guard<std::mutex> lk(audio.meta_mutex);
            meta = audio.active_meta;
            cur_path = audio.last_played_path;
        }
        text_at(g.art_box_y, split_x + 2, "[ META ]", COLOR_ACCENT);
        bool meta_updated = audio.metadata_updated.exchange(false, std::memory_order_acq_rel);
        if (cur_path != cur_cover_path || meta_updated) {
            invalidate_cover_cache();
            cur_cover_path = cur_path;
        }
        if (!show_help && !show_about && !show_del_confirm)draw_cover(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
        int my = g.meta_y;
        draw_meta_line(my, split_x, g.side_w, max_y, "Title:", meta.title.empty() ? "none" : meta.title);
        draw_meta_line(my, split_x, g.side_w, max_y, "Artist:", meta.artist.empty() ? "none" : meta.artist);
        draw_meta_line(my, split_x, g.side_w, max_y, "Album:", meta.album.empty() ? "none" : meta.album);
        draw_meta_line(my, split_x, g.side_w, max_y, "Codec:", meta.codec_name);
        draw_meta_line(my, split_x, g.side_w, max_y, "Rate:", std::to_string(meta.sample_rate) + " Hz");
        draw_meta_line(my, split_x, g.side_w, max_y, "Bitrate:", std::to_string(meta.bit_rate / 1000) + " kb/s");
        draw_meta_line(my, split_x, g.side_w, max_y, "Ch:", std::to_string(meta.channels));
        draw_status(audio, max_x, max_y - 2);
        draw_progress_bar(audio, max_x, max_y - 1);
        if (show_help)render_help_dialog(max_y, max_x);
        else if (show_about)render_about_dialog(max_y, max_x);
        else if (show_del_confirm)render_del_dialog(max_y, max_x);
        last_idx = idx;
        last_scroll = scroll;
        XRenderComposite(dpy, PictOpSrc, backbuf_pic, None, win_pic, 0, 0, 0, 0, 0, 0, W, H);
        XFlush(dpy);
    }
    void render(AudioEngine & audio) {
        render_full(audio);
        need_redraw = false;
        need_status = false;
    }
    void execute_cmd(const std::string & line, AudioEngine & audio, bool &running) {
        std::string t = trim_str(line);
        if (t.empty())return;
        std::string cmd, args;
        size_t sp = t.find_first_of(" \t");
        if (sp != std::string::npos) {
            cmd = t.substr(0, sp);
            args = trim_str(t.substr(sp + 1));
        } else cmd = t;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
        if (cmd == "q" || cmd == "quit" || cmd == "exit") {
            running = false;
        } else if (cmd == "speed" || cmd == "spd") {
            if (!args.empty())try {
                double v;
                if (args.back() == '%')v = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                else {
                    v = std::stod(args);
                    if (v > 3.0 && v <= 300.0)v /= 100.0;
                }
                audio.speed = std::clamp(v, 0.10, 3.0);
            } catch ( ... ) {
            }
        } else if (cmd == "vol" || cmd == "volume" || cmd == "v") {
            if (!args.empty())try {
                double v;
                if (args.back() == '%')v = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                else {
                    v = std::stod(args);
                    if (v > 2.0 && v <= 200.0)v /= 100.0;
                }
                audio.volume = std::clamp(v, 0.0, 2.0);
                audio.is_muted = false;
            } catch ( ... ) {
            }
        } else if (cmd == "seek" || cmd == "seeking" || cmd == "s") {
            if (!args.empty())try {
                if (args.front() == '+' || args.front() == '-')audio.seek_relative(std::stod(args));
                else if (args.back() == '%')audio.seek_absolute_percent(std::stod(args.substr(0, args.size() - 1)) / 100.0);
                else if (args.find(':') != std::string::npos) {
                    size_t col = args.find(':');
                    int m = std::stoi(args.substr(0, col));
                    double s = std::stod(args.substr(col + 1));
                    double target = m * 60 + s;
                    if (audio.duration.load() > 0.0)audio.seek_req = std::clamp(target, 0.0, audio.duration.load());
                } else {
                    double s = std::stod(args);
                    if (audio.duration.load() > 0.0)audio.seek_req = std::clamp(s, 0.0, audio.duration.load());
                }
            } catch ( ... ) {
            }
        } else if (cmd == "reversing" || cmd == "reverse" || cmd == "rev") {
            if (args.empty())audio.toggle_reverse();
            else {
                std::string a = args;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                if (a == "on" || a == "1" || a == "true")audio.is_reverse = true;
                else if (a == "off" || a == "0" || a == "false")audio.is_reverse = false;
                else audio.toggle_reverse();
            }
        } else if (cmd == "del" || cmd == "delete" || cmd == "rm") {
            if (!entries.empty() && idx >= 0 && idx < (int)entries.size() && entries[idx].name != "..") {
                show_del_confirm = true;
                del_target_name = entries[idx].name;
                del_target_path = entries[idx].path;
                del_target_is_dir = entries[idx].is_dir;
                invalidate_cover_cache();
                cur_cover_path.clear();
            }
        } else if (cmd == "rename" || cmd == "mv") {
            if (!args.empty() && !entries.empty() && idx >= 0 && idx < (int)entries.size() && entries[idx].name != "..") {
                std::string nn = args;
                if (nn.size() >= 2 && ((nn.front() == '"' && nn.back() == '"') || (nn.front() == '\'' && nn.back() == '\'')))nn = nn.substr(1, nn.size() - 2);
                fs::path cp(nn);
                std::string cn = cp.filename().string();
                if (!cn.empty()) {
                    fs::path op = entries[idx].path;
                    fs::path np = op.parent_path() / cn;
                    std::error_code ec;
                    fs::rename(op, np, ec);
                    if (!ec) {
                        if (audio.last_played_path == op.string()) {
                            std::lock_guard<std::mutex> lk(audio.meta_mutex);
                            audio.last_played_path = np.string();
                        }
                        scan();
                        for (size_t i = 0; i < entries.size(); ++i)if (entries[i].name == cn) {
                            idx = (int)i;
                            break;
                        }
                    }
                }
            }
        } else if (cmd == "help" || cmd == "h") {
            show_help = true;
        }
    }
    bool dispatch_key(KeySym ks, const std::string & utf8_in, unsigned int state, AudioEngine & audio, bool &running) {
        need_status = true;
        char ch = utf8_in.empty() ? 0 : utf8_in[0];
        bool shift = (state & ShiftMask) != 0;
        if (ks == XK_F1) {
            show_about = false;
            show_help = !show_help;
            invalidate_cover_cache();
            cur_cover_path.clear();
            need_redraw = true;
            return true;
        }
        if (ks == XK_F2) {
            show_help = false;
            show_about = !show_about;
            invalidate_cover_cache();
            cur_cover_path.clear();
            need_redraw = true;
            return true;
        }
        if (show_del_confirm) {
            if (ch == 'y' || ch == 'Y' || ks == XK_Return || ks == XK_KP_Enter) {
                bool was_current = (audio.last_played_path == del_target_path);
                if (was_current)audio.stop();
                std::error_code ec;
                if (del_target_is_dir)fs::remove_all(del_target_path, ec);
                else fs::remove(del_target_path, ec);
                show_del_confirm = false;
                invalidate_cover_cache();
                cur_cover_path.clear();
                scan();
                if (was_current) {
                    std::lock_guard<std::mutex> lk(audio.meta_mutex);
                    audio.last_played_path.clear();
                }
                need_redraw = true;
                return true;
            }
            if (ch == 'n' || ch == 'N' || ks == XK_Escape || ch == 'q' || ch == 'Q') {
                show_del_confirm = false;
                need_redraw = true;
                return true;
            }
            return true;
        }
        if (is_command_mode) {
            if (ks == XK_Escape) {
                is_command_mode = false;
                command_query.clear();
                need_redraw = true;
                return true;
            }
            if (ks == XK_Return || ks == XK_KP_Enter) {
                is_command_mode = false;
                execute_cmd(command_query, audio, running);
                command_query.clear();
                need_redraw = true;
                return true;
            }
            if (ks == XK_BackSpace) {
                if (!command_query.empty())command_query.pop_back();
                else is_command_mode = false;
                need_status = true;
                return true;
            }
            if (!utf8_in.empty() && (unsigned char)utf8_in[0] >= 32) {
                command_query += utf8_in;
                need_status = true;
                return true;
            }
            return true;
        }
        if (show_help || show_about) {
            if (ks == XK_Escape || ch == 'q' || ch == ' ' || ks == XK_Return || ks == XK_KP_Enter) {
                show_help = false;
                show_about = false;
                invalidate_cover_cache();
                cur_cover_path.clear();
                need_redraw = true;
                return true;
            }
            return true;
        }
        if (is_searching) {
            if (ks == XK_Escape) {
                is_searching = false;
                need_redraw = true;
                return true;
            }
            if (ks == XK_Return || ks == XK_KP_Enter) {
                is_searching = false;
                if (!search_query.empty()) {
                    last_query = search_query;
                    search_next(true);
                }
                need_redraw = true;
                return true;
            }
            if (ks == XK_BackSpace) {
                if (!search_query.empty())search_query.pop_back();
                need_status = true;
                return true;
            }
            if (!utf8_in.empty() && (unsigned char)utf8_in[0] >= 32) {
                search_query += utf8_in;
                need_status = true;
                return true;
            }
            return true;
        }
        if (ch == ':') {
            is_command_mode = true;
            command_query.clear();
            need_status = true;
            return true;
        }
        if (ch == '/') {
            is_searching = true;
            search_query.clear();
            need_status = true;
            return true;
        }
        if (ch == '?') {
            search_next(false);
            need_status = true;
            return true;
        }
        if (ch >= '0' && ch <= '9') {
            audio.seek_absolute_percent((ch - '0') * 0.10);
            need_status = true;
            return true;
        }
        switch (ks) {
            case XK_q:
            case XK_Q:
                running = false;
                return true;
            case XK_r:
            case XK_R:
                audio.toggle_reverse();
                need_status = true;
                return true;
            case XK_BackSpace:
                go_up_dir();
                return true;
            case XK_Up:
            case XK_k:
                if (idx > 0)idx--;
                need_status = true;
                return true;
            case XK_Down:
            case XK_j:
                if (idx + 1 < (int)entries.size())idx++;
                need_status = true;
                return true;
            case XK_Page_Up:
                idx = std::max(0, idx - 5);
                need_status = true;
                return true;
            case XK_Page_Down:
                idx = std::min((int)entries.size() - 1, idx + 5);
                need_status = true;
                return true;
            case XK_g:
                idx = 0;
                need_status = true;
                return true;
            case XK_G:
                if (!entries.empty())idx = (int)entries.size() - 1;
                need_status = true;
                return true;
            case XK_space:
                if (entries.empty())return true;
                if (entries[idx].name == "..")go_up_dir();
                else if (entries[idx].is_dir)enter_dir(entries[idx].path);
                else if (entries[idx].is_audio)audio.load(entries[idx].path);
                need_redraw = true;
                return true;
            case XK_Home:
                audio.reset_modifiers();
                need_status = true;
                return true;
            case XK_p:
            case XK_c:
                audio.toggle_pause();
                need_status = true;
                return true;
            case XK_v:
                audio.stop();
                need_redraw = true;
                return true;
            case XK_m:
                audio.toggle_mute();
                need_status = true;
                return true;
            case XK_l:
                mode = PlayMode::LOOP;
                need_status = true;
                return true;
            case XK_s:
                mode = PlayMode::SEQUENTIAL;
                need_status = true;
                return true;
            case XK_f:
                mode = PlayMode::NORMAL;
                need_status = true;
                return true;
            case XK_Left:
                audio.seek_relative(shift ? -1.0 : -3.0);
                need_status = true;
                return true;
            case XK_Right:
                audio.seek_relative(shift ? 1.0 : 3.0);
                need_status = true;
                return true;
            case XK_bracketleft:
                audio.speed = std::clamp(audio.speed.load() - 0.05, 0.10, 3.0);
                need_status = true;
                return true;
            case XK_bracketright:
                audio.speed = std::clamp(audio.speed.load() + 0.05, 0.10, 3.0);
                need_status = true;
                return true;
            case XK_braceleft:
                audio.speed = std::clamp(audio.speed.load() - 0.01, 0.10, 3.0);
                need_status = true;
                return true;
            case XK_braceright:
                audio.speed = std::clamp(audio.speed.load() + 0.01, 0.10, 3.0);
                need_status = true;
                return true;
            case XK_minus:
            case XK_KP_Subtract:
                audio.volume = std::clamp(audio.volume.load() - 0.05, 0.0, 2.0);
                need_status = true;
                return true;
            case XK_plus:
            case XK_equal:
            case XK_KP_Add:
                audio.volume = std::clamp(audio.volume.load() + 0.05, 0.0, 2.0);
                need_status = true;
                return true;
            default:
                break;
        }
        return false;
    }
    void handle_mouse_press(XButtonEvent & e, AudioEngine & audio) {
        int max_x = cols(), max_y = rows();
        int cc = e.x / CELL_W;
        int cr = e.y / CELL_H;
        if (e.button == 4) {
            idx = std::max(0, idx - 1);
            need_status = true;
            return;
        }
        if (e.button == 5) {
            idx = std::min((int)entries.size() - 1, idx + 1);
            need_status = true;
            return;
        }
        if (cr == max_y - 2 && (e.button == 1 || e.button == 3)) {
            std::string status = build_status(audio);
            size_t vp = status.find("Vol:");
            size_t sp = status.find("Speed:");
            if (vp != std::string::npos && sp != std::string::npos) {
                size_t ve = status.find('|', vp);
                if (ve == std::string::npos)ve = status.size();
                size_t se = status.find('[', sp);
                if (se == std::string::npos)se = status.size();
                if (cc >= (int)vp && cc < (int)ve) {
                    if (e.button == 1)audio.volume = std::clamp(audio.volume.load() - 0.05, 0.0, 2.0);
                    else audio.volume = std::clamp(audio.volume.load() + 0.05, 0.0, 2.0);
                    audio.is_muted = false;
                    need_status = true;
                    return;
                }
                if (cc >= (int)sp && cc < (int)se) {
                    if (e.button == 1)audio.speed = std::clamp(audio.speed.load() - 0.05, 0.10, 3.0);
                    else audio.speed = std::clamp(audio.speed.load() + 0.05, 0.10, 3.0);
                    need_status = true;
                    return;
                }
            }
            return;
        }
        if (e.button != 1)return;
        if (cr == max_y - 1) {
            seek_dragging = true;
            double r = x_to_ratio(e.x, max_x, max_y - 1);
            audio.seek_absolute_percent(r);
            need_status = true;
            return;
        }
        auto g = compute_geom(max_y, max_x);
        int view_h = max_y - 3;
        if (cr >= 1 && cr <= view_h && cc < g.split_x) {
            int clicked = scroll + (cr - 1);
            if (clicked < (int)entries.size()) {
                idx = clicked;
                if (entries[idx].name == "..")go_up_dir();
                else if (entries[idx].is_dir)enter_dir(entries[idx].path);
                else if (entries[idx].is_audio)audio.load(entries[idx].path);
                need_redraw = true;
            }
        }
    }
    void handle_mouse_release(XButtonEvent & e, AudioEngine & ) {
        if (e.button == 1 && seek_dragging) {
            seek_dragging = false;
        }
    }
    void handle_mouse_motion(XMotionEvent & e, AudioEngine & audio) {
        if (!seek_dragging)return;
        int max_x = cols(), max_y = rows();
        int cr = e.y / CELL_H;
        if (cr != max_y - 1)return;
        double r = x_to_ratio(e.x, max_x, max_y - 1);
        audio.seek_absolute_percent(r);
        need_status = true;
    }
    void on_resize(int nw, int nh) {
        if (nw == W && nh == H)return;
        W = nw;
        H = nh;
        recreate_backbuffer();
        cur_cover_path.clear();
        need_redraw = true;
    }
    private:
    fs::path path;
    std::vector<FileItem> entries;
    std::string search_query, last_query, command_query;
    int idx = 0, scroll = 0;
};
int main() {
    std::setlocale(LC_ALL, "");
    XSetErrorHandler(x_error_handler);
    av_log_set_level(AV_LOG_QUIET);
    if (pipe2(g_wake_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        fprintf(stderr, "pipe2 failed: %s\n", strerror(errno));
        return 1;
    }
    AudioEngine audio;
    MothApp app;
    bool running = true;
    double last_pts = -1.0;
    bool last_playing = false, last_paused = false, last_muted = false, last_reverse = false;
    double last_vol = -1.0, last_spd = -1.0;
    while (running) {
        while (XPending(app.dpy)) {
            XEvent ev;
            XNextEvent(app.dpy, &ev);
            if (XFilterEvent( &ev, app.win))continue;
            switch (ev.type) {
                case Expose:
                    app.need_redraw = true;
                    break;
                case ConfigureNotify:
                    app.on_resize(ev.xconfigure.width, ev.xconfigure.height);
                    break;
                case KeyPress:
                    {
                        KeySym ks = NoSymbol;
                        char buf[64] = {
                            0
                    };
                    int len = 0;
                    if (app.xic) {
                        Status st;
                        len = Xutf8LookupString(app.xic, &ev.xkey, buf, sizeof(buf) - 1, &ks, &st);
                    }
                    if (len == 0 && !app.xic) {
                        len = XLookupString( &ev.xkey, buf, sizeof(buf) - 1, &ks, nullptr);
                    }
                    buf[std::max(0, len)] = 0;
                    std::string u8(buf, std::max(0, len));
                    app.dispatch_key(ks, u8, ev.xkey.state, audio, running);
                    break;
                }
                case ButtonPress:
                    app.handle_mouse_press(ev.xbutton, audio);
                    break;
                case ButtonRelease:
                    app.handle_mouse_release(ev.xbutton, audio);
                    break;
                case MotionNotify:
                    app.handle_mouse_motion(ev.xmotion, audio);
                    break;
                case ClientMessage:
                    if ((Atom)ev.xclient.data.l[0] == app.wmDelete)running = false;
                    break;
                default:
                    break;
            }
        }
        if (!running)break;
        if (audio.track_finished.exchange(false, std::memory_order_acq_rel)) {
            if (app.mode == PlayMode::LOOP) {
                if (!audio.last_played_path.empty())audio.load(audio.last_played_path);
            } else if (app.mode == PlayMode::SEQUENTIAL) {
                app.play_next_track(audio);
            }
        }
        bool cp = audio.is_playing.load(std::memory_order_relaxed);
        bool cu = audio.is_paused.load(std::memory_order_relaxed);
        bool cm = audio.is_muted.load(std::memory_order_relaxed);
        bool crv = audio.is_reverse.load(std::memory_order_relaxed);
        double cpt = audio.cur_pts.load(std::memory_order_relaxed);
        double cv = audio.volume.load(std::memory_order_relaxed);
        double cs = audio.speed.load(std::memory_order_relaxed);
        bool pts_moved = std::abs(cpt - last_pts) >= 0.20;
        bool changed = (cp != last_playing) || (cu != last_paused) || (cm != last_muted) || (crv != last_reverse) || (std::abs(cv - last_vol) > 0.001) || (std::abs(cs - last_spd) > 0.001) || audio.metadata_updated.load(std::memory_order_acquire);
        if (app.need_redraw || app.need_status || pts_moved || changed || app.seek_dragging) {
            app.render(audio);
            last_pts = cpt;
            last_playing = cp;
            last_paused = cu;
            last_muted = cm;
            last_reverse = crv;
            last_vol = cv;
            last_spd = cs;
        }
        bool active = (cp && !cu) || app.seek_dragging;
        struct pollfd fds[2] = {
            {
                app.x_fd, POLLIN, 0
            }, {
                g_wake_fds[0], POLLIN, 0
            },
        };
        int timeout = active ? 16 : -1;
        int pr = ::poll(fds, 2, timeout);
        if (pr < 0 && errno == EINTR)continue;
        if (pr > 0 && (fds[1].revents & POLLIN))drain_wake_pipe();
    }
    return 0;
}
