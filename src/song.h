#pragma once
#include <string>

/* Platforms, in the order of the source switch */
enum { SRC_KUGOU = 0, SRC_NETEASE = 1, SRC_QQ = 2 };

struct Song {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int durationSec = 0;
    std::wstring cover;     // Local cover path; may be empty
    std::string  coverUrl;  // Cover URL; downloaded on demand when not local
    std::string  hash;      // Song id on its platform: Kugou hash, NetEase id or QQ mid
    long long    mixId = 0; // mixsongid; use as album_audio_id when resolving stream URL
    long long    albumId = 0;
    int          source = -1;   // SRC_* of the platform the id belongs to, set when the list is loaded
};
