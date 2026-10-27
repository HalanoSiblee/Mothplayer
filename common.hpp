#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <clocale>
#include <cwchar>
#include <cstdint>

namespace fs = std::filesystem;

enum class PlayMode : uint8_t { NORMAL = 0, LOOP = 1, SEQUENTIAL = 2 };

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

static inline std::string truncate_utf8(const std::string& str, int max_cols, int* out_cols = nullptr) {
    if (max_cols <= 0 || str.empty()) {
        if (out_cols) *out_cols = 0;
        return "";
    }
    bool is_ascii = true;
    for (unsigned char c : str) {
        if (c >= 0x80) { is_ascii = false; break; }
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

static inline int utf8_display_width(const std::string& str) {
    if (str.empty()) return 0;
    bool is_ascii = true;
    for (unsigned char c : str) {
        if (c >= 0x80) { is_ascii = false; break; }
    }
    if (is_ascii) return static_cast<int>(str.size());

    std::wstring wstr(str.size(), L'\0');
    size_t converted = std::mbstowcs(&wstr[0], str.c_str(), str.size());
    if (converted == static_cast<size_t>(-1)) return static_cast<int>(str.length());
    int cols = 0;
    for (size_t i = 0; i < converted; ++i) {
        int w = wcwidth(wstr[i]);
        if (w > 0) cols += w;
    }
    return cols;
}

static inline std::string trim_str(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}