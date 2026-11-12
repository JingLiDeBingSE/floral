#pragma once
#include <windows.h>

/*
 * Theme system
 *
 * Layout constants are compile-time; colors are runtime variables. Switching
 * themes just rewrites these UI:: values, so call sites like `UI::BG` stay untouched.
 */

struct ThemeDef {
    const wchar_t* name;
    UINT32 BG, PANEL, HOVER, SELECT, BORDER, TEXT, TEXT2, TEXT3, BTN_HOVER, BTN_CLOSE;
    UINT32 SHADOW, ACCENT, ACCENT_TEXT, GHOST, CARD;   // shadow / accent / accent text / subtle fill / card
    UINT32 DISC, DISC_GROOVE;                          // disc base / grooves
};

namespace UI {
    /* Layout */
    constexpr float PAD     = 24.0f;    // horizontal content padding
    constexpr float TITLE_H = 52.0f;    // custom title bar height
    constexpr float GAP     = 14.0f;    // tile gap
    constexpr float R_TILE  = 16.0f;    // cover corner radius
    constexpr float R_BTN   = 8.0f;

    constexpr float BTN_W = 44.0f;
    constexpr float BTN_H = 30.0f;
    constexpr int   BTN_COUNT = 3;

    /* Colors (runtime; assigned by ThemeApply) */
    extern UINT32 BG, PANEL, HOVER, SELECT, BORDER;
    extern UINT32 TEXT, TEXT2, TEXT3, BTN_HOVER, BTN_CLOSE;
    extern UINT32 SHADOW, ACCENT, ACCENT_TEXT, GHOST, CARD;
    extern UINT32 DISC, DISC_GROOVE;
}

int            ThemeCount();
const wchar_t* ThemeName(int i);
int            ThemeCurrent();
void           ThemeApply(int index);   // switch to theme i
void           ThemeNext();             // next theme (and persists it)
void           ThemeLoad();             // load last selection at startup
int            ThemeGeneration();       // +1 per switch; rebuild theme-dependent brushes
