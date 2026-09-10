#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
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
#define APP_V "0.0.000"
#endif
enum class PlayMode : uint8_t {
    NORMAL = 0,
    LOOP = 1,
    SEQUENTIAL = 2
};

#pragma pack(push, 1)
struct FileItem {
    std::string name;
    std::string path;
    uint8_t is_dir   : 1;
    uint8_t is_audio : 1;
    uint8_t reserved : 6;
};
#pragma pack(pop)

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
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> metadata_updated{false};
    std::atomic<bool> track_finished{false};
    std::atomic<double> seek_req{-1.0};
    std::atomic<double> volume{1.0f};
    std::atomic<double> speed{1.0f};
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

    void stop() {
        is_playing = false;
        is_paused = false;
        ++cover_gen;
        if (pcm) snd_pcm_drop(pcm);
        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            active_meta = TrackMetadata{};
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
    }

private:
    snd_pcm_t* pcm = nullptr;
    std::thread worker;
    std::mutex cmd_mutex;
    std::string queued_path;
    bool track_switch = false;

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
                        int out_w = std::max(1, static_cast<int>(src_w * scale));
                        int out_h = std::max(1, static_cast<int>(src_h * scale));
                        out_w &= ~1;
                        out_h &= ~1;
                        if (out_w < 2) out_w = 2;
                        if (out_h < 2) out_h = 2;

                        SwsContext* sws = sws_getContext(
                            src_w, src_h, (AVPixelFormat)vframe->format,
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

        if (my_gen != cover_gen.load() || last_played_path != path) return;

        {
            std::lock_guard<std::mutex> lk(meta_mutex);
            active_meta.sixel_art = std::move(sixel);
            active_meta.has_cover = has_cover;
            active_meta.cover_w = cover_out_w;
            active_meta.cover_h = cover_out_h;
            metadata_updated = true;
        }
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
            last_played_path = path;
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
            active_meta = local_meta;
            metadata_updated = true;
        }

        duration = (fctx->duration != AV_NOPTS_VALUE) ? (double)fctx->duration / AV_TIME_BASE : 0.0;

        std::string path_copy = path;
        uint64_t my_gen = ++cover_gen;
        int tw = cover_target_w.load();
        int th = cover_target_h.load();
        if (tw < 48) tw = 48;
        if (th < 48) th = 48;
        if (tw > 400) tw = 400;
        if (th > 400) th = 400;
        std::thread([this, path_copy, my_gen, tw, th]() {
            extract_album_art_async(path_copy, tw, th, my_gen);
        }).detach();

        SwrContext* swr = nullptr;
        int out_rate = 44100;
        double active_speed = speed.load();

        auto configure_swr = [&](double spd) {
            if (swr) swr_free(&swr);
            swr = swr_alloc();
            int in_rate = static_cast<int>(std::round(cctx->sample_rate * spd));
            av_opt_set_chlayout(swr, "in_chlayout", &cctx->ch_layout, 0);
            av_opt_set_int(swr, "in_sample_rate", in_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", cctx->sample_fmt, 0);

            AVChannelLayout out_ch;
            av_channel_layout_default(&out_ch, 2);
            av_opt_set_chlayout(swr, "out_chlayout", &out_ch, 0);
            av_opt_set_int(swr, "out_sample_rate", out_rate, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            swr_init(swr);
            av_channel_layout_uninit(&out_ch);
        };

        configure_swr(active_speed);

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frm = av_frame_alloc();
        is_playing = true;
        is_paused = false;

        int max_resample_buf = out_rate / 2;
        std::vector<int16_t> audio_buf(max_resample_buf * 2);

        bool natural_eof = true;

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
                int64_t target_pts = static_cast<int64_t>(s / av_q2d(fctx->streams[audio_idx]->time_base));
                av_seek_frame(fctx, audio_idx, target_pts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(cctx);
            }

            double cur_spd = speed.load();
            if (std::abs(cur_spd - active_speed) > 0.001) {
                active_speed = cur_spd;
                configure_swr(active_speed);
            }

            if (av_read_frame(fctx, pkt) < 0) break;

            if (pkt->stream_index == audio_idx) {
                if (avcodec_send_packet(cctx, pkt) == 0) {
                    while (avcodec_receive_frame(cctx, frm) == 0) {
                        if (frm->pts != AV_NOPTS_VALUE) {
                            cur_pts = frm->pts * av_q2d(fctx->streams[audio_idx]->time_base);
                        }

                        int samples = av_rescale_rnd(swr_get_delay(swr, cctx->sample_rate) +
                                                      frm->nb_samples, out_rate, cctx->sample_rate, AV_ROUND_UP);

                        if (samples > max_resample_buf) {
                            max_resample_buf = samples;
                            audio_buf.resize(max_resample_buf * 2);
                        }

                        uint8_t* out_ptrs[1] = { reinterpret_cast<uint8_t*>(audio_buf.data()) };
                        int converted = swr_convert(swr, out_ptrs, samples,
                                                    (const uint8_t**)frm->extended_data, frm->nb_samples);

                        if (converted > 0) {
                            double v = is_muted.load() ? 0.0 : volume.load();
                            for (int i = 0; i < converted * 2; ++i) {
                                int32_t val = static_cast<int32_t>(audio_buf[i] * v);
                                audio_buf[i] = static_cast<int16_t>(std::clamp(val, -32768, 32767));
                            }

                            if (!pcm) continue;
                            int frames_left = converted;
                            int16_t* p = audio_buf.data();
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
                    }
                }
            }
            av_packet_unref(pkt);
        }

        is_playing = false;
        av_frame_free(&frm);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&cctx);
        avformat_close_input(&fctx);

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
    bool show_help = false;
    bool show_about = false;
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

        blank_sixel_seq = generate_blank_sixel(160, 160);

        path = fs::current_path();
        scan();
    }

    ~MothApp() {
        if (sixel_on_screen) {
            int max_y, max_x;
            getmaxyx(stdscr, max_y, max_x);
            int right_w = static_cast<int>(max_x * RIGHT_FRAC);
            if (right_w < 28) right_w = 28;
            if (right_w > max_x - 22) right_w = max_x - 22;
            int split_x = max_x - right_w;
            if (split_x < 22) split_x = 22;
            std::cout << "\033[s\033[2;" << (split_x + 4) << "H"
                      << blank_sixel_seq << "\033[u" << std::flush;
        }
        endwin();
    }

    void scan() {
        entries.clear();
        if (path.has_parent_path()) {
            entries.push_back({ "..", (path / "..").lexically_normal().string(), 1, 0, 0 });
        }

        std::vector<FileItem> dirs, regular;
        try {
            for (const auto& e : fs::directory_iterator(path)) {
                if (e.is_directory()) {
                    dirs.push_back({ e.path().filename().string(), e.path().string(), 1, 0, 0 });
                } else {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a") {
                        regular.push_back({ e.path().filename().string(), e.path().string(), 0, 1, 0 });
                    }
                }
            }
        } catch (...) {}

        std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b){ return a.name < b.name; });
        std::sort(regular.begin(), regular.end(), [](const auto& a, const auto& b){ return a.name < b.name; });

        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), regular.begin(), regular.end());
        idx = 0;
        scroll = 0;
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

    std::string sec_to_str(double t) {
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
        int right_w = static_cast<int>(max_x * RIGHT_FRAC);
        if (right_w < 28) right_w = 28;
        if (right_w > max_x - 22) right_w = max_x - 22;
        g.split_x = max_x - right_w;
        if (g.split_x < 22) g.split_x = 22;
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
            int img_cols = 10;
            if (meta.cover_w > 0)
                img_cols = std::max(1, (meta.cover_w + 9) / 10);
            if (img_cols > area_cols) img_cols = area_cols;
            int off_c = (area_cols - img_cols) / 2;
            if (off_c < 0) off_c = 0;

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
            const char* msg = "(No Artwork)";
            int msg_x = base_col + std::max(0, (area_cols - 13) / 2);
            int msg_y = base_row + std::max(0, art_rows / 2);
            mvprintw(msg_y, msg_x, "%s", msg);
            last_sixel_w = 0;
            last_sixel_h = 0;
            last_sixel_row = 0;
            last_sixel_col = 0;
            sixel_on_screen = false;
        }
    }

    void draw_entry_line(int y, int entry_idx, int split_x, bool is_selected) {
        move(y, 0);
        if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size())) {
            const auto& item = entries[entry_idx];
            if (is_selected) attron(COLOR_PAIR(1) | A_REVERSE);
            else attron(COLOR_PAIR(1));

            char prefix = item.is_dir ? '/' : ' ';
            mvprintw(y, 0, " %c%-*.*s", prefix, split_x - 3, split_x - 3, item.name.c_str());

            if (is_selected) attroff(COLOR_PAIR(1) | A_REVERSE);
            else attroff(COLOR_PAIR(1));
        } else {
            mvprintw(y, 0, "%*s", split_x, "");
        }
    }

    void draw_status(AudioEngine& audio, int max_x, int y) {
        attron(COLOR_PAIR(1) | A_REVERSE);
        move(y, 0);
        clrtoeol();

        std::string status = audio.is_playing ? (audio.is_paused ? "PAUSED " : "PLAYING") : "STOPPED";
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
        mvprintw(y, 1, "%.*s", max_x - 3, stat_buf);
        attroff(COLOR_PAIR(1) | A_REVERSE);
    }

    void draw_progress_bar(AudioEngine& audio, int max_x, int y) {
        move(y, 0);
        clrtoeol();

        if (is_searching) {
            mvprintw(y, 1, "/%s", search_query.c_str());
        } else {
            double dur = audio.duration.load();
            double ratio = (dur > 0.0) ? std::clamp(audio.cur_pts.load() / dur, 0.0, 1.0) : 0.0;
            int bar_w = std::max(4, max_x - 6);
            int filled = static_cast<int>(ratio * bar_w);

            std::string bar;
            bar.reserve(bar_w + 3);
            bar.push_back('[');
            bar.append(filled, '=');
            if (filled < bar_w) {
                bar.push_back('>');
                bar.append(bar_w - filled - 1, '-');
            }
            bar.push_back(']');
            mvaddnstr(y, 1, bar.data(), static_cast<int>(bar.size()));
        }
    }

    // ------------------------------------------------------------------
    // Full structural render
    // ------------------------------------------------------------------
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

        if (show_help || show_about) attron(A_DIM);

        auto g = compute_geom(max_y, max_x);
        const int split_x = g.split_x;
        const int view_h  = max_y - 3;
        audio.cover_target_w = g.cover_px_w;
        audio.cover_target_h = g.cover_px_h;

        if (idx < scroll) scroll = idx;
        if (idx >= scroll + view_h) scroll = idx - view_h + 1;

        // 1. Directory Header
        attron(COLOR_PAIR(1) | A_REVERSE);
        move(0, 0);
        clrtoeol();
        mvprintw(0, 1, "Browser: %-.*s", max_x - 11, path.string().c_str());
        attroff(COLOR_PAIR(1) | A_REVERSE);

        // 2. Left panel – full list
        for (int i = 0; i < view_h; ++i) {
            int entry_idx = scroll + i;
            draw_entry_line(i + 1, entry_idx, split_x, entry_idx == idx);
        }
        last_idx = idx;
        last_scroll = scroll;

        // Vertical divider & Full clear of right panel to prevent dialog marks
        for (int y = 1; y < max_y - 2; ++y) {
            move(y, split_x + 1);
            clrtoeol();
            mvaddch(y, split_x, ACS_VLINE);
        }

        // 3. Right panel: Cover at top, Metadata at bottom
        TrackMetadata meta;
        {
            std::lock_guard<std::mutex> lk(audio.meta_mutex);
            meta = audio.active_meta;
        }

        mvprintw(g.art_box_y, split_x + 2, "[ META ]");

        bool cover_changed = (audio.last_played_path != last_cover_path);
        if (cover_changed || audio.metadata_updated.load()) {
            if (!show_help && !show_about) {
                emit_sixel(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
            }
            last_cover_path = audio.last_played_path;
            audio.metadata_updated = false;
        }

        // Render metadata strictly at the bottom
        int meta_y = g.meta_y;
        auto draw_meta_line = [&](const char* label, const std::string& val) {
            if (meta_y >= max_y - 2) return;
            move(meta_y, split_x + 2);
            clrtoeol();
            mvaddch(meta_y, split_x, ACS_VLINE);
            mvprintw(meta_y, split_x + 2, "%-8s %.*s",
                     label, std::max(0, g.side_w - 9), val.c_str());
            meta_y++;
        };

        draw_meta_line("Title:",  meta.title.empty()  ? "(none)" : meta.title);
        draw_meta_line("Artist:", meta.artist.empty() ? "(none)" : meta.artist);
        draw_meta_line("Album:",  meta.album.empty()  ? "(none)" : meta.album);
        draw_meta_line("Codec:",  meta.codec_name);
        draw_meta_line("Rate:",   std::to_string(meta.sample_rate) + " Hz");
        draw_meta_line("Bitrate:",std::to_string(meta.bit_rate / 1000) + " kb/s");
        draw_meta_line("Ch:",     std::to_string(meta.channels));

        // 4 + 5. Status + progress
        draw_status(audio, max_x, max_y - 2);
        draw_progress_bar(audio, max_x, max_y - 1);

        if (show_help || show_about) attroff(A_DIM);

        if (show_help) {
            render_help_dialog(max_y, max_x);
        } else if (show_about) {
            render_about_dialog(max_y, max_x);
        }

        refresh();
    }

    // ------------------------------------------------------------------
    // Lightweight update path
    // ------------------------------------------------------------------
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
            {
                std::lock_guard<std::mutex> lk(audio.meta_mutex);
                meta = audio.active_meta;
            }
            if (!show_help && !show_about) {
                emit_sixel(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
            }
            last_cover_path = audio.last_played_path;
            audio.metadata_updated = false;

            int meta_y = g.meta_y;
            auto draw_meta_line = [&](const char* label, const std::string& val) {
                if (meta_y >= max_y - 2) return;
                move(meta_y, split_x + 2);
                clrtoeol();
                mvaddch(meta_y, split_x, ACS_VLINE);
                mvprintw(meta_y, split_x + 2, "%-8s %.*s",
                         label, std::max(0, g.side_w - 9), val.c_str());
                meta_y++;
            };
            draw_meta_line("Title:",  meta.title.empty()  ? "(none)" : meta.title);
            draw_meta_line("Artist:", meta.artist.empty() ? "(none)" : meta.artist);
            draw_meta_line("Album:",  meta.album.empty()  ? "(none)" : meta.album);
            draw_meta_line("Codec:",  meta.codec_name);
            draw_meta_line("Rate:",   std::to_string(meta.sample_rate) + " Hz");
            draw_meta_line("Bitrate:",std::to_string(meta.bit_rate / 1000) + " kb/s");
            draw_meta_line("Ch:",     std::to_string(meta.channels));
        }

        if (show_help) {
            render_help_dialog(max_y, max_x);
        } else if (show_about) {
            render_about_dialog(max_y, max_x);
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

    void render_help_dialog(int max_y, int max_x) {
        int dlg_w = std::min(60, max_x - 4);
        int dlg_h = std::min(21, max_y - 2);
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
        draw_help_item("p / c", "Pause / Resume");
        draw_help_item("v", "Stop playback");
        draw_help_item("m", "Toggle Mute");
        draw_help_item("Left / Right", "Seek -3s / +3s");
        draw_help_item("S-Left / S-Right", "Seek -1s / +1s");
        draw_help_item("0 - 9", "Instant Seek 0% - 90%");
        draw_help_item("[ / ]", "Varispeed -10% / +10%");
        draw_help_item("{ / }", "Varispeed -1% / +1%");
        draw_help_item("l / s / f", "Loop / Sequential / Normal");
        draw_help_item("PgUp / PgDn", "Scroll 5 items");
        draw_help_item("g / G", "Jump to Top / Bottom");
        draw_help_item("Mouse Wheel", "Smooth Up / Down scroll");
        draw_help_item("/ | SHIFT+/", "Find track | Next match");
        draw_help_item("Home", "Reset Vol & Speed");
        draw_help_item("F1 / Esc", "Close Help");
        draw_help_item("F2", "About");
    }

    void render_about_dialog(int max_y, int max_x) {
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
            if (ch == 27 || ch == 'q' || ch == ' ' || ch == 10) {
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
            if (ch == 10 || ch == ' ') {
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
                    int right_w = static_cast<int>(max_x * RIGHT_FRAC);
                    if (right_w < 28) right_w = 28;
                    if (right_w > max_x - 22) right_w = max_x - 22;
                    int split_x = max_x - right_w;
                    if (split_x < 22) split_x = 22;
                    int view_h = max_y - 3;
                    int bot_y = max_y - 1;
                    int bar_w = std::max(4, max_x - 6);

                    if (ev.y == bot_y && ev.x >= 2 && ev.x <= 2 + bar_w) {
                        double pct = static_cast<double>(ev.x - 2) / static_cast<double>(bar_w);
                        audio.seek_absolute_percent(pct);
                        need_status = true;
                        return;
                    }

                    if (ev.y >= 1 && ev.y <= view_h && ev.x < split_x) {
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
    int idx = 0;
    int scroll = 0;
};

int main() {
    av_log_set_level(AV_LOG_QUIET);

    AudioEngine audio;
    MothApp app;

    bool running = true;
    double last_pts = -1.0;
    bool last_playing = false;
    bool last_paused = false;
    bool last_muted = false;
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
        double cur_pts   = audio.cur_pts.load();
        double cur_vol   = audio.volume.load();
        double cur_spd   = audio.speed.load();

        bool pts_moved = std::abs(cur_pts - last_pts) >= 0.20;
        bool state_changed = (cur_playing != last_playing) ||
                             (cur_paused  != last_paused)  ||
                             (cur_muted   != last_muted)   ||
                             (std::abs(cur_vol - last_vol) > 0.001) ||
                             (std::abs(cur_spd - last_spd) > 0.001) ||
                             audio.metadata_updated.load();

        if (app.need_redraw || app.need_status || pts_moved || state_changed) {
            app.render(audio);
            last_pts     = cur_pts;
            last_playing = cur_playing;
            last_paused  = cur_paused;
            last_muted   = cur_muted;
            last_vol     = cur_vol;
            last_spd     = cur_spd;
        }
    }

    return 0;
}