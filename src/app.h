#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>
#include <vector>

/* ───────── Data ───────── */
struct Song {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int durationSec = 0;
    std::wstring cover;     // Local cover path; may be empty
    std::string  coverUrl;  // Cover URL; downloaded on demand when not local
    std::string  hash;      // Kugou hash
    long long    mixId = 0; // mixsongid; use as album_audio_id when resolving stream URL
    long long    albumId = 0;
};

/* ───────── Playlists ───────── */
struct Playlist {
    std::wstring name;
    std::string  gcid;        // Kugou global_collection_id
    int          count = 0;
    std::string  coverUrl;
    bool         isDefault = false;
};

/* ───────── Pages ───────── */
enum class Page { Playlists, Grid, Player };

/* ───────── Hit regions ───────── */
enum HitKind {
    HK_NONE = 0,
    HK_MIN, HK_MAX, HK_CLOSE,
    HK_TILE,        // Cover tile
    HK_BACK,        // Player page back
    HK_PLAY,        // Play/pause
    HK_PREV,        // Previous track
    HK_NEXT,        // Next track
    HK_PROGRESS,    // Progress bar
    HK_SETTINGS,    // Open settings
    HK_SET_CLOSE,   // Close settings
    HK_SET_BG,      // Backdrop outside settings panel (click closes)
    HK_SET_CARD,    // Settings card itself (swallow clicks; don't reach songs behind)
    HK_SET_CLEAR,   // Clear cache
    HK_SET_THEME,   // Toggle theme
    HK_SET_LOGIN,   // Sign in again (platform picker first)
    HK_SET_LANG_ZH, // Settings: switch UI language to Chinese
    HK_SET_LANG_EN, // Settings: switch UI language to English
    HK_LOGIN_PICK,  // Which platform was picked
    HK_LOGIN_TIP,   // Title bar sign-in status
    HK_PLAYER,      // Open player page
    HK_VOL,         // Volume button
    HK_VOL_TRACK,   // Volume slider
    HK_SRC,         // Source switch (Kugou / NetEase Cloud Music)
    HK_TOG,         // Listen-together button
    HK_TOG_HOST,    // Create room
    HK_TOG_COPY,    // Copy room code
    HK_TOG_INPUT,   // Room code input
    HK_TOG_JOIN,    // Join
    HK_TOG_LEAVE,   // Leave listen-together
    HK_TOG_SWAP,    // Change local address (deprecated placeholder)
    HK_TOG_CARD,    // Card blank area: swallow clicks; don't close panel
    HK_SCROLL,      // Song picker scrollbar
    HK_PL,          // Playlist picker circle
    HK_PL_PICK,     // Title bar "Songs": back to playlist page
    HK_PICKER,      // Title bar button: back to playlist picker
    HK_QUAL,        // Quality button
    HK_QUAL_ITEM    // Quality option
};
struct Hit {
    D2D1_RECT_F r{};
    int kind = HK_NONE;
    int index = -1;
};

struct App {
    std::vector<Song> songs;
    std::vector<Playlist> playlists;

    /* Playlist picker: pan + drag inertia */
    float plX = 0.0f, plY = 0.0f;             // Current pan
    float plTargetX = 0.0f, plTargetY = 0.0f; // Eased target
    float plVelX = 0.0f, plVelY = 0.0f;       // Velocity after release
    int   plHover = -1;                       // Hovered playlist
    int   plActive = -1;                      // Selected (about to expand)
    float plSelT = 0.0f;                      // Selection anim 0→1 (center + expand)
    bool  plDragging = false;
    POINT plDragStartMouse = { 0, 0 };
    float plDragStartX = 0.0f, plDragStartY = 0.0f;
    bool  plDragMoved = false;

    Page page = Page::Grid;
    float pageAnim = 0.0f;              // 0 = grid, 1 = player

    int hoverTile = -1;
    std::vector<float> tileAnim;        // Per-tile hover anim 0..1

    int current = -1;                   // Currently playing track
    bool playing = false;
    double pos = 0.0;                   // Playback position (seconds)

    float scroll = 0.0f;
    float scrollTarget = 0.0f;      // Smooth scroll: actual value follows this
    float maxScroll = 0.0f;

    POINT mouse = { -1, -1 };
    float width = 960.0f;
    float height = 640.0f;

    std::vector<Hit> hits;
};

extern App g_app;

#include "theme.h"

enum { WB_MIN = 0, WB_MAX = 1, WB_CLOSE = 2 };

void InitData();
extern std::wstring g_likesName;

/* Embedded resource: default playlist data (used by data.cpp) */
bool LoadEmbeddedLikes(std::string& out);
