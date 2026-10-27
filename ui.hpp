#pragma once
#include "common.hpp"
#include "audio.hpp"
#include <ncurses.h>

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
    int last_sixel_w = 0, last_sixel_h = 0, last_sixel_row = 0, last_sixel_col = 0;
    static constexpr double RIGHT_FRAC = 0.40;

    std::string del_target_name, del_target_path;
    bool del_target_is_dir = false;

    MothApp();
    ~MothApp();
    void render(AudioEngine& audio);
    void handle_input(AudioEngine& audio, bool& running);
    
    // Moved to public so main.cpp can access it for sequential playback
    void play_next_track(AudioEngine& audio);

private:
    fs::path path;
    std::vector<FileItem> entries;
    std::string blank_sixel_seq;
    std::string search_query, last_query, command_query;
    int idx = 0, scroll = 0;

    void scan();
    void search_next(bool from_current);
    static std::string sec_to_str(double t);
    
    struct PanelGeom { int split_x, side_w, cover_px_w, cover_px_h, art_box_y, art_rows, meta_y; };
    PanelGeom compute_geom(int max_y, int max_x) const;
    
    void emit_sixel(const TrackMetadata& meta, int split_x, int art_box_y, int art_rows, int side_w);
    void draw_entry_line(int y, int entry_idx, int split_x, bool is_selected);
    static void draw_meta_line(int& meta_y, int split_x, int side_w, int max_y, const char* label, const std::string& val);
    void draw_status(AudioEngine& audio, int max_x, int y);
    void draw_progress_bar(AudioEngine& audio, int max_x, int y);
    void render_full(AudioEngine& audio);
    void render_light(AudioEngine& audio);
    void render_del_dialog(int max_y, int max_x);
    static void render_help_dialog(int max_y, int max_x);
    static void render_about_dialog(int max_y, int max_x);
    void execute_cmd(const std::string& line, AudioEngine& audio, bool& running);
};