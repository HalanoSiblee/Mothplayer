#include "audio.hpp"
#include "ui.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

int main() {
    std::setlocale(LC_ALL, "");
    av_log_set_level(AV_LOG_QUIET);

    AudioEngine audio;
    MothApp app;

    bool running = true;
    double last_pts = -1.0;
    bool last_playing = false, last_paused = false, last_muted = false, last_reverse = false;
    double last_vol = -1.0, last_spd = -1.0;

    while (running) {
        app.handle_input(audio, running);

        if (audio.track_finished.exchange(false)) {
            if (app.mode == PlayMode::LOOP) {
                if (!audio.last_played_path.empty()) audio.load(audio.last_played_path);
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
        bool state_changed = (cur_playing != last_playing) || (cur_paused != last_paused) ||
                             (cur_muted != last_muted) || (cur_reverse != last_reverse) ||
                             (std::abs(cur_vol - last_vol) > 0.001) ||
                             (std::abs(cur_spd - last_spd) > 0.001) ||
                             audio.metadata_updated.load();

        if (app.need_redraw || app.need_status || pts_moved || state_changed) {
            app.render(audio);
            last_pts = cur_pts; last_playing = cur_playing; last_paused = cur_paused;
            last_muted = cur_muted; last_reverse = cur_reverse;
            last_vol = cur_vol; last_spd = cur_spd;
        }
    }
    return 0;
}