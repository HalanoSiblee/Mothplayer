#pragma once
#include "common.hpp"
#include <thread>
#include <atomic>
#include <mutex>

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

    AudioEngine();
    ~AudioEngine();

    void load(const std::string& path);
    void toggle_pause();
    void toggle_mute();
    void toggle_reverse();
    void stop();
    void seek_relative(double seconds);
    void seek_absolute_percent(double ratio);
    void reset_modifiers();

private:
    void* pcm = nullptr; // snd_pcm_t*
    std::thread worker;
    std::mutex cmd_mutex;
    std::string queued_path;
    bool track_switch = false;

    std::vector<int16_t> pcm_data;
    std::mutex pcm_mutex;
    std::atomic<size_t> total_frames{0};
    std::atomic<bool> decode_complete{false};
    std::atomic<bool> abort_decode{false};

    void extract_album_art_async(const std::string& path, int target_w, int target_h, uint64_t my_gen);
    void fill_pcm_buffer(const std::string& path);
    void run();
    void decode_loop(const std::string& path);
};