#include "ui.hpp"
#include <sixel.h>
#include <chrono>
#ifndef BUTTON4_PRESSED
#define BUTTON4_PRESSED 00040000000L
#endif
#ifndef BUTTON5_PRESSED
#define BUTTON5_PRESSED 00200000000L
#endif
#ifndef BUTTON_CTRL
#define BUTTON_CTRL 0x02000000L
#endif
#ifndef APP_VERSION
#define APP_VERSION "1.0.000"
#endif
extern "C" {
#include <libavformat/avformat.h>
}

static int sixel_write_callback(char *data, int size, void *priv) {
    auto *buf = static_cast<std::string * >(priv);
    buf->append(data, size);
    return size;
}

std::string encode_rgb_to_sixel_libsixel(uint8_t *rgb_data, int w, int h) {
    if (!rgb_data || w <= 0 || h <= 0)return "";
    std::string sixel_buffer;
    sixel_output_t * output = nullptr;
    sixel_dither_t * dither = nullptr;
    if (SIXEL_FAILED(sixel_output_new( &output, sixel_write_callback, &sixel_buffer, nullptr)))return "";
    if (SIXEL_FAILED(sixel_dither_new( &dither, 256, nullptr))) {
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

MothApp::MothApp() {
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

MothApp::~MothApp() {
    if (sixel_on_screen) {
        int max_x = getmaxx(stdscr);
        int right_w = std::clamp(static_cast<int>(max_x * RIGHT_FRAC), 28, std::max(28, max_x - 22));
        int split_x = std::max(22, max_x - right_w);
        std::cout << "\033[s\033[2;" << (split_x + 4) << "H" << blank_sixel_seq << "\033[u" << std::flush;
    }
    endwin();
}

void MothApp::scan() {
    entries.clear();
    if (path.has_parent_path() && path != path.parent_path()) {
        entries.push_back( {
            "..", (path / "..").lexically_normal().string(), true, false
        });
    }
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
                if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a") {
                    regular.push_back( {
                        e.path().filename().string(), e.path().string(), false, true
                    });
                }
            }
        }
    } catch ( ... ) {
    }
    std::sort(dirs.begin(), dirs.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
    std::sort(regular.begin(), regular.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), regular.begin(), regular.end());
    if (idx >= static_cast<int>(entries.size()))idx = std::max(0, static_cast<int>(entries.size()) - 1);
    last_idx = -1;
    last_scroll = -1;
    need_redraw = true;
}

void MothApp::play_next_track(AudioEngine & audio) {
    if (entries.empty())return;
    int next_idx = idx + 1;
    while (next_idx < static_cast<int>(entries.size()) && !entries[next_idx].is_audio)next_idx++;
    if (next_idx < static_cast<int>(entries.size())) {
        idx = next_idx;
        audio.load(entries[idx].path);
    } else {
        audio.stop();
    }
    need_redraw = true;
}

void MothApp::search_next(bool from_current) {
    if (last_query.empty() || entries.empty())return;
    int start = from_current ? idx : (idx + 1) % entries.size();
    for (size_t offset = 0; offset < entries.size(); ++offset) {
        int cur = (start + offset) % entries.size();
        auto it = std::search(entries[cur].name.begin(), entries[cur].name.end(), last_query.begin(), last_query.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
        if (it != entries[cur].name.end()) {
            idx = cur;
            need_redraw = true;
            return;
        }
    }
}

std::string MothApp::sec_to_str(double t) {
    int s = static_cast<int>(t);
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
    return b;
}

MothApp::PanelGeom MothApp::compute_geom(int max_y, int max_x)const {
    PanelGeom g {
    };
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

void MothApp::emit_sixel(const TrackMetadata & meta, int split_x, int art_box_y, int art_rows, int side_w) {
    const int base_row = art_box_y + 1;
    const int base_col = split_x + 2;
    const int area_cols = std::max(4, side_w - 2);
    if (sixel_on_screen && last_sixel_w > 0 && last_sixel_h > 0) {
        std::string eraser = generate_blank_sixel(last_sixel_w, last_sixel_h);
        if (!eraser.empty()) {
            int er = last_sixel_row > 0 ? last_sixel_row : base_row;
            int ec = last_sixel_col > 0 ? last_sixel_col : base_col;
            std::cout << "\033[s\033[" << er << ";" << ec << "H" << eraser << "\033[u" << std::flush;
        }
        sixel_on_screen = false;
    }
    for (int r = 0; r < art_rows; ++r) {
        move(base_row + r, base_col);
        for (int c = 0; c < area_cols; ++c)addch(' ');
    }
    if (meta.has_cover && !meta.cover_rgb.empty()) {
        std::string sixel_art = encode_rgb_to_sixel_libsixel(const_cast<uint8_t * >(meta.cover_rgb.data()), meta.cover_w, meta.cover_h);
        if (!sixel_art.empty()) {
            int img_cols = meta.cover_w > 0 ? std::max(1, (meta.cover_w + 9) / 10) : 10;
            if (img_cols > area_cols)img_cols = area_cols;
            int off_c = std::max(0, (area_cols - img_cols) / 2);
            const int row = base_row;
            const int col = base_col + off_c;
            std::cout << "\033[s\033[" << row << ";" << col << "H" << sixel_art << "\033[u" << std::flush;
            last_sixel_w = meta.cover_w > 0 ? meta.cover_w : 100;
            last_sixel_h = meta.cover_h > 0 ? meta.cover_h : 100;
            last_sixel_row = row;
            last_sixel_col = col;
            sixel_on_screen = true;
            return;
        }
    }
    int msg_x = base_col + std::max(0, (area_cols - 13) / 2);
    int msg_y = base_row + std::max(0, art_rows / 2);
    mvprintw(msg_y, msg_x, "(No Artwork)");
    last_sixel_w = 0;
    last_sixel_h = 0;
    last_sixel_row = 0;
    last_sixel_col = 0;
    sixel_on_screen = false;
}

void MothApp::toggle_ab_loop(AudioEngine & audio) {
    double a = audio.ab_a.load();
    double b = audio.ab_b.load();
    if (a < 0.0) {
        audio.ab_a = audio.cur_pts.load();
        audio.ab_b = -1.0;
    } else if (b < 0.0) {
        double new_b = audio.cur_pts.load();
        if (new_b < a) {
            audio.ab_a = new_b;
            audio.ab_b = a;
        } else {
            audio.ab_b = new_b;
        }
    } else {
        audio.ab_a = -1.0;
        audio.ab_b = -1.0;
    }
    need_redraw = true;
    need_status = true;
}

void MothApp::draw_entry_line(int y, int entry_idx, int split_x, bool is_selected) {
    if (split_x <= 0)return;
    move(y, 0);
    if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size())) {
        const auto &item = entries[entry_idx];
        if (is_selected)attron(COLOR_PAIR(1) | A_REVERSE);
        else attron(COLOR_PAIR(1));
        char prefix = item.is_dir ? '/' : ' ';
        int max_text_cols = std::max(0, split_x - 3);
        int disp_w = 0;
        std::string disp_name = truncate_utf8(item.name, max_text_cols, &disp_w);
        int pad = std::max(0, split_x - 2 - disp_w);
        mvprintw(y, 0, " %c%s%*s", prefix, disp_name.c_str(), pad, "");
        if (is_selected)attroff(COLOR_PAIR(1) | A_REVERSE);
        else attroff(COLOR_PAIR(1));
    } else {
        mvprintw(y, 0, "%*s", split_x, "");
    }
}

void MothApp::draw_meta_line(int &meta_y, int split_x, int side_w, int max_y, const char *label, const std::string & val) {
    if (meta_y >= max_y - 2)return;
    move(meta_y, split_x + 2);
    clrtoeol();
    mvaddch(meta_y, split_x, ACS_VLINE);
    int val_max_cols = std::max(0, side_w - 10);
    std::string val_str = truncate_utf8(val, val_max_cols);
    mvprintw(meta_y, split_x + 2, "%-8s %s", label, val_str.c_str());
    meta_y++;
}

void MothApp::draw_status(AudioEngine & audio, int max_x, int y) {
    attron(COLOR_PAIR(1) | A_REVERSE);
    move(y, 0);
    clrtoeol();
    std::string status;
    if (audio.is_playing) {
        if (audio.is_paused)status = audio.is_reverse ? "PAUSED[REV]" : "PAUSED ";
        else status = audio.is_reverse ? "REV-PLAY" : "PLAYING ";
    } else {
        status = audio.is_reverse ? "STOPPED[REV]" : "STOPPED";
    }
    std::string mode_str = (mode == PlayMode::LOOP) ? "LOOP" : (mode == PlayMode::SEQUENTIAL ? "SEQ" : "NORMAL");
    int vol = static_cast<int>(audio.volume.load() * 100.0);
    int spd = static_cast<int>(std::round(audio.speed.load() * 100.0));
    char stat_buf[256];
    if (audio.is_muted.load()) {
        snprintf(stat_buf, sizeof(stat_buf), " [%s|%s] %s/%s | Vol: MUTE | Speed: %3d%% [F1: Help] ", status.c_str(), mode_str.c_str(), sec_to_str(audio.cur_pts.load()).c_str(), sec_to_str(audio.duration.load()).c_str(), spd);
    } else {
        snprintf(stat_buf, sizeof(stat_buf), " [%s|%s] %s/%s | Vol: %3d%% | Speed: %3d%% [F1: Help] ", status.c_str(), mode_str.c_str(), sec_to_str(audio.cur_pts.load()).c_str(), sec_to_str(audio.duration.load()).c_str(), vol, spd);
    }
    mvprintw(y, 1, "%.*s", std::max(0, max_x - 3), stat_buf);
    attroff(COLOR_PAIR(1) | A_REVERSE);
}

void MothApp::draw_progress_bar(AudioEngine & audio, int max_x, int y) {
    move(y, 0);
    clrtoeol();
    if (is_command_mode) {
        std::string disp_cmd = truncate_utf8(command_query, std::max(0, max_x - 4));
        mvprintw(y, 1, ":%s", disp_cmd.c_str());
        int cur_x = 2 + utf8_display_width(disp_cmd);
        if (cur_x < max_x - 1)move(y, cur_x);
    } else if (is_searching) {
        std::string disp_query = truncate_utf8(search_query, std::max(0, max_x - 4));
        mvprintw(y, 1, "/%s", disp_query.c_str());
        int cur_x = 2 + utf8_display_width(disp_query);
        if (cur_x < max_x - 1)move(y, cur_x);
    } else {
        double dur = audio.duration.load();
        double ratio = (dur > 0.0) ? std::clamp(audio.cur_pts.load() / dur, 0.0, 1.0) : 0.0;
        int bar_w = std::max(4, max_x - 6);
        int filled = static_cast<int>(ratio * bar_w);
        mvaddch(y, 1, '[');
        if (filled > 0)mvhline(y, 2, '=', filled);
        if (filled < bar_w) {
            mvaddch(y, 2 + filled, audio.is_reverse ? '<' : '>');
            if (bar_w - filled - 1 > 0)mvhline(y, 2 + filled + 1, '-', bar_w - filled - 1);
        }
        mvaddch(y, 2 + bar_w, ']');
        double a = audio.ab_a.load();
        double b = audio.ab_b.load();
        if (a >= 0.0 && dur > 0.0) {
            int x_a = 2 + static_cast<int>((a / dur) * bar_w);
            if (x_a >= 2 && x_a < 2 + bar_w) {
                attron(A_BOLD);
                mvaddch(y, x_a, '+');
                attroff(A_BOLD);
            }
        }
        if (b >= 0.0 && dur > 0.0) {
            int x_b = 2 + static_cast<int>((b / dur) * bar_w);
            if (x_b >= 2 && x_b < 2 + bar_w) {
                attron(A_BOLD);
                mvaddch(y, x_b, '+');
                attroff(A_BOLD);
            }
        }
    }
}

void MothApp::render_full(AudioEngine & audio) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    if (max_y < 8 || max_x < 30) {
        move(0, 0);
        clrtoeol();
        mvprintw(0, 0, "Terminal window too small.");
        refresh();
        return;
    }
    if (show_help || show_about || show_del_confirm)attron(A_DIM);
    auto g = compute_geom(max_y, max_x);
    const int split_x = g.split_x;
    const int view_h = max_y - 3;
    audio.cover_target_w = g.cover_px_w;
    audio.cover_target_h = g.cover_px_h;
    if (idx < scroll)scroll = idx;
    if (idx >= scroll + view_h)scroll = idx - view_h + 1;
    attron(COLOR_PAIR(1) | A_REVERSE);
    move(0, 0);
    clrtoeol();
    int max_header_cols = std::max(0, max_x - 11);
    std::string header_path = truncate_utf8(path.string(), max_header_cols);
    mvprintw(0, 1, "Browser: %s", header_path.c_str());
    attroff(COLOR_PAIR(1) | A_REVERSE);
    for (int i = 0; i < view_h; ++i) {
        draw_entry_line(i + 1, scroll + i, split_x, (scroll + i) == idx);
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
        if (!show_help && !show_about && !show_del_confirm)emit_sixel(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
        last_cover_path = cur_track_path;
        audio.metadata_updated = false;
    }
    int meta_y = g.meta_y;
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Title:", meta.title.empty() ? "(none)" : meta.title);
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Artist:", meta.artist.empty() ? "(none)" : meta.artist);
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Album:", meta.album.empty() ? "(none)" : meta.album);
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Codec:", meta.codec_name);
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Rate:", std::to_string(meta.sample_rate) + " Hz");
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Bitrate:", std::to_string(meta.bit_rate / 1000) + " kb/s");
    draw_meta_line(meta_y, split_x, g.side_w, max_y, "Ch:", std::to_string(meta.channels));
    draw_status(audio, max_x, max_y - 2);
    draw_progress_bar(audio, max_x, max_y - 1);
    if (show_help || show_about || show_del_confirm)attroff(A_DIM);
    if (show_help)render_help_dialog(max_y, max_x);
    else if (show_about)render_about_dialog(max_y, max_x);
    else if (show_del_confirm)render_del_dialog(max_y, max_x);
    refresh();
}

void MothApp::render_light(AudioEngine & audio) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    if (max_y < 8 || max_x < 30)return;
    auto g = compute_geom(max_y, max_x);
    const int split_x = g.split_x;
    const int view_h = max_y - 3;
    audio.cover_target_w = g.cover_px_w;
    audio.cover_target_h = g.cover_px_h;
    bool scroll_changed = false;
    if (idx < scroll) {
        scroll = idx;
        scroll_changed = true;
    }
    if (idx >= scroll + view_h) {
        scroll = idx - view_h + 1;
        scroll_changed = true;
    }
    if (idx != last_idx || scroll != last_scroll) {
        if (scroll_changed || last_scroll != scroll) {
            for (int i = 0; i < view_h; ++i) {
                draw_entry_line(i + 1, scroll + i, split_x, (scroll + i) == idx);
            }
        } else {
            int old_y = (last_idx - last_scroll) + 1;
            int new_y = (idx - scroll) + 1;
            if (old_y >= 1 && old_y <= view_h)draw_entry_line(old_y, last_idx, split_x, false);
            if (new_y >= 1 && new_y <= view_h)draw_entry_line(new_y, idx, split_x, true);
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
        if (!show_help && !show_about && !show_del_confirm)emit_sixel(meta, split_x, g.art_box_y, g.art_rows, g.side_w);
        last_cover_path = cur_track_path;
        audio.metadata_updated = false;
        int meta_y = g.meta_y;
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Title:", meta.title.empty() ? "(none)" : meta.title);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Artist:", meta.artist.empty() ? "(none)" : meta.artist);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Album:", meta.album.empty() ? "(none)" : meta.album);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Codec:", meta.codec_name);
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Rate:", std::to_string(meta.sample_rate) + " Hz");
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Bitrate:", std::to_string(meta.bit_rate / 1000) + " kb/s");
        draw_meta_line(meta_y, split_x, g.side_w, max_y, "Ch:", std::to_string(meta.channels));
    }
    if (show_help)render_help_dialog(max_y, max_x);
    else if (show_about)render_about_dialog(max_y, max_x);
    else if (show_del_confirm)render_del_dialog(max_y, max_x);
    refresh();
}

void MothApp::render(AudioEngine & audio) {
    if (need_redraw) {
        render_full(audio);
        need_redraw = false;
        need_status = false;
    } else {
        render_light(audio);
        need_status = false;
    }
}

void MothApp::render_del_dialog(int max_y, int max_x) {
    int dlg_w = std::min(60, max_x - 4);
    int dlg_h = 9;
    int top_y = (max_y - dlg_h) / 2;
    int left_x = (max_x - dlg_w) / 2;
    attron(COLOR_PAIR(2) | A_BOLD);
    for (int y = 0; y < dlg_h; ++y) {
        move(top_y + y, left_x);
        for (int x = 0; x < dlg_w; ++x) {
            if (y == 0 && x == 0)addch(ACS_ULCORNER);
            else if (y == 0 && x == dlg_w - 1)addch(ACS_URCORNER);
            else if (y == dlg_h - 1 && x == 0)addch(ACS_LLCORNER);
            else if (y == dlg_h - 1 && x == dlg_w - 1)addch(ACS_LRCORNER);
            else if (y == 0 || y == dlg_h - 1)addch(ACS_HLINE);
            else if (x == 0 || x == dlg_w - 1)addch(ACS_VLINE);
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

void MothApp::render_help_dialog(int max_y, int max_x) {
    int dlg_w = std::min(64, max_x - 4);
    int dlg_h = std::min(24, max_y - 2);
    int top_y = (max_y - dlg_h) / 2;
    int left_x = (max_x - dlg_w) / 2;
    attron(A_BOLD);
    for (int y = 0; y < dlg_h; ++y) {
        move(top_y + y, left_x);
        for (int x = 0; x < dlg_w; ++x) {
            if (y == 0 && x == 0)addch(ACS_ULCORNER);
            else if (y == 0 && x == dlg_w - 1)addch(ACS_URCORNER);
            else if (y == dlg_h - 1 && x == 0)addch(ACS_LLCORNER);
            else if (y == dlg_h - 1 && x == dlg_w - 1)addch(ACS_LRCORNER);
            else if (y == 0 || y == dlg_h - 1)addch(ACS_HLINE);
            else if (x == 0 || x == dlg_w - 1)addch(ACS_VLINE);
            else addch(' ');
        }
    }
    mvprintw(top_y, left_x + (dlg_w - 14) / 2, " KEY BINDINGS ");
    attroff(A_BOLD);
    int row = top_y + 2;
    auto item = [ & ](const char *keys, const char *desc) {
        if (row < top_y + dlg_h - 1) {
            attron(A_BOLD);
            mvprintw(row, left_x + 3, "%-17s", keys);
            attroff(A_BOLD);
            printw(": %s", desc);
            row++;
        }
    };
    item("Space / Enter", "Play / Enter Directory");
    item("Backspace / RMB", "Go to Parent Directory");
    item("p / c", "Pause / Resume");
    item("r", "Toggle Reverse Playback");
    item(":", "Vim Cmd (:speed, :vol, :seek, :del)");
    item("v", "Stop playback");
    item("m", "Toggle Mute");
    item("Left / Right", "Seek -3s / +3s");
    item("S-Left / S-Right", "Seek -1s / +1s");
    item("0 - 9", "Instant Seek 0% - 90%");
    item("o / RMB on bar", "Set A-B Loop (3 presses to clear)");
    item("[ / ]", "Varispeed -5% / +5%");
    item("{ / }", "Varispeed -1% / +1%");
    item("l / s / f", "Loop / Sequential / Normal");
    item("PgUp / PgDn", "Scroll 5 items");
    item("g / G", "Jump to Top / Bottom");
    item("Mouse Wheel", "Smooth Up / Down scroll");
    item("/ | ?", "Find track | Next match");
    item("Home", "Reset Modifiers & A-B Loop");
    item("Ctrl + LMB", "Global Scrub (anywhere)");
    item("F1 / Esc", "Close Help");
    item("F2", "About");
}

void MothApp::render_about_dialog(int max_y, int max_x) {
    int dlg_w = std::min(56, max_x - 4);
    int dlg_h = std::min(15, max_y - 2);
    int top_y = (max_y - dlg_h) / 2;
    int left_x = (max_x - dlg_w) / 2;
    attron(A_BOLD);
    for (int y = 0; y < dlg_h; ++y) {
        move(top_y + y, left_x);
        for (int x = 0; x < dlg_w; ++x) {
            if (y == 0 && x == 0)addch(ACS_ULCORNER);
            else if (y == 0 && x == dlg_w - 1)addch(ACS_URCORNER);
            else if (y == dlg_h - 1 && x == 0)addch(ACS_LLCORNER);
            else if (y == dlg_h - 1 && x == dlg_w - 1)addch(ACS_LRCORNER);
            else if (y == 0 || y == dlg_h - 1)addch(ACS_HLINE);
            else if (x == 0 || x == dlg_w - 1)addch(ACS_VLINE);
            else addch(' ');
        }
    }
    mvprintw(top_y, left_x + (dlg_w - 9) / 2, " ABOUT ");
    attroff(A_BOLD);
    int row = top_y + 2;
    auto line = [ & ](const char *s) {
        if (row < top_y + dlg_h - 1)mvprintw(row++, left_x + 3, "%.*s", dlg_w - 6, s);
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
    line("v" APP_VERSION);
}

void MothApp::execute_cmd(const std::string & line, AudioEngine & audio, bool &running) {
    std::string trimmed = trim_str(line);
    if (trimmed.empty())return;
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
            try {
                double val = 1.0;
                if (args.back() == '%')val = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                else {
                    val = std::stod(args);
                    if (val > 3.0 && val <= 300.0)val /= 100.0;
                }
                audio.speed = std::clamp(val, 0.10, 3.0);
            } catch ( ... ) {
            }
        }
    } else if (cmd == "vol" || cmd == "volume" || cmd == "v") {
        if (!args.empty()) {
            try {
                double val = 1.0;
                if (args.back() == '%')val = std::stod(args.substr(0, args.size() - 1)) / 100.0;
                else {
                    val = std::stod(args);
                    if (val > 2.0 && val <= 200.0)val /= 100.0;
                }
                audio.volume = std::clamp(val, 0.0, 2.0);
                audio.is_muted = false;
            } catch ( ... ) {
            }
        }
    } else if (cmd == "seek" || cmd == "seeking" || cmd == "s") {
        if (!args.empty()) {
            try {
                if (args.front() == '+' || args.front() == '-')audio.seek_relative(std::stod(args));
                else if (args.back() == '%')audio.seek_absolute_percent(std::stod(args.substr(0, args.size() - 1)) / 100.0);
                else if (args.find(':') != std::string::npos) {
                    size_t colon = args.find(':');
                    int m = std::stoi(args.substr(0, colon));
                    double s = std::stod(args.substr(colon + 1));
                    double target = m * 60 + s;
                    if (audio.duration.load() > 0.0)audio.seek_req = std::clamp(target, 0.0, audio.duration.load());
                } else {
                    double s = std::stod(args);
                    if (audio.duration.load() > 0.0)audio.seek_req = std::clamp(s, 0.0, audio.duration.load());
                }
            } catch ( ... ) {
            }
        }
    } else if (cmd == "reversing" || cmd == "reverse" || cmd == "rev") {
        if (args.empty()) {
            audio.toggle_reverse();
        } else {
            std::string a = args;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            if (a == "on" || a == "1" || a == "true")audio.is_reverse = true;
            else if (a == "off" || a == "0" || a == "false")audio.is_reverse = false;
            else audio.toggle_reverse();
        }
    } else if (cmd == "del" || cmd == "delete" || cmd == "rm") {
        if (!entries.empty() && idx >= 0 && idx < static_cast<int>(entries.size())) {
            if (entries[idx].name != "..") {
                flash();
                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                attron(COLOR_PAIR(2));
                for (int y = 0; y < max_y; ++y)mvhline(y, 0, ' ', max_x);
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
                if (new_name.front() == '"' && new_name.back() == '"' && new_name.size() >= 2)new_name = new_name.substr(1, new_name.size() - 2);
                else if (new_name.front() == '\'' && new_name.back() == '\'' && new_name.size() >= 2)new_name = new_name.substr(1, new_name.size() - 2);
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

void MothApp::handle_input(AudioEngine & audio, bool &running) {
    int ch = getch();
    if (ch == ERR)return;
    need_status = true;
    if (ch == KEY_RESIZE) {
        last_cover_path.clear();
        sixel_on_screen = false;
        need_redraw = true;
        return;
    }
    if (show_del_confirm) {
        if (ch == 'y' || ch == 'Y' || ch == 10 || ch == 13 || ch == KEY_ENTER) {
            audio.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::error_code ec;
            if (del_target_is_dir)fs::remove_all(del_target_path, ec);
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
            if (!command_query.empty())command_query.pop_back();
            else {
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
            if (!search_query.empty())search_query.pop_back();
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
        if (getmouse( &ev) == OK) {
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
            if (ev.bstate & (BUTTON3_CLICKED | BUTTON3_PRESSED)) {
                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                if (ev.y == max_y - 1) {
                    toggle_ab_loop(audio);
                } else {
                    if (path.has_parent_path() && path != path.parent_path()) {
                        try {
                            path = fs::canonical(path.parent_path());
                        } catch ( ... ) {
                            path = path.parent_path();
                        }
                        scan();
                        idx = 0;
                        scroll = 0;
                        need_redraw = true;
                    }
                }
                return;
            }
            if (ev.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
                int max_y, max_x;
                getmaxyx(stdscr, max_y, max_x);
                auto g = compute_geom(max_y, max_x);
                int view_h = max_y - 3;
                int bot_y = max_y - 1;
                int bar_w = std::max(4, max_x - 6);
                if (ev.bstate & BUTTON_CTRL) {
                    double pct = static_cast<double>(ev.x - 2) / static_cast<double>(bar_w);
                    pct = std::clamp(pct, 0.0, 1.0);
                    audio.seek_absolute_percent(pct);
                    need_status = true;
                    return;
                }
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
                        if (entries[idx].name == "..") {
                            if (path.has_parent_path() && path != path.parent_path()) {
                                try {
                                    path = fs::canonical(path.parent_path());
                                } catch ( ... ) {
                                    path = path.parent_path();
                                }
                                scan();
                                idx = 0;
                                scroll = 0;
                            }
                        } else if (entries[idx].is_dir) {
                            try {
                                path = fs::canonical(entries[idx].path);
                                scan();
                                idx = 0;
                                scroll = 0;
                            } catch ( ... ) {
                            }
                        } else if (entries[idx].is_audio) {
                            audio.load(entries[idx].path);
                        }
                        need_redraw = true;
                    }
                }
                return;
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
        case 'q':
        case 'Q':
            running = false;
            break;
        case 'r':
        case 'R':
            audio.toggle_reverse();
            need_status = true;
            break;
        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (path.has_parent_path() && path != path.parent_path()) {
                try {
                    path = fs::canonical(path.parent_path());
            } catch ( ... ) {
                path = path.parent_path();
            }
            scan();
            idx = 0;
            scroll = 0;
            need_redraw = true;
        }
        break;
        case KEY_UP:
        case 'k':
            if (idx > 0)idx--;
            need_status = true;
            break;
        case KEY_DOWN:
        case 'j':
            if (idx + 1 < static_cast<int>(entries.size()))idx++;
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
        case KEY_SPREVIOUS:
        case 'g':
            idx = 0;
            need_status = true;
            break;
        case KEY_SNEXT:
        case 'G':
            if (!entries.empty())idx = static_cast<int>(entries.size()) - 1;
            need_status = true;
            break;
        case ' ':
        case '\n':
        case '\r':
        case KEY_ENTER:
            if (entries.empty())break;
            if (entries[idx].is_dir) {
                try {
                    path = fs::canonical(entries[idx].path);
                    scan();
                    idx = 0;
                    scroll = 0;
            } catch ( ... ) {
            }
        } else if (entries[idx].is_audio) {
            audio.load(entries[idx].path);
        }
        need_redraw = true;
        break;
        case KEY_HOME:
            audio.reset_modifiers();
            need_status = true;
            break;
        case 'p':
        case 'c':
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
        case 'l':
            mode = PlayMode::LOOP;
            need_status = true;
            break;
        case 's':
            mode = PlayMode::SEQUENTIAL;
            need_status = true;
            break;
        case 'f':
            mode = PlayMode::NORMAL;
            need_status = true;
            break;
        case KEY_LEFT:
            audio.seek_relative( -3.0);
            need_status = true;
            break;
        case KEY_RIGHT:
            audio.seek_relative(3.0);
            need_status = true;
            break;
        case KEY_SLEFT:
            audio.seek_relative( -1.0);
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
        case '+':
        case '=':
            audio.volume = std::clamp(audio.volume.load() + 0.05, 0.0, 2.0);
            need_status = true;
            break;
        case 'o':
        case 'O':
            toggle_ab_loop(audio);
            need_status = true;
            break;
        default:
            break;
    }
}