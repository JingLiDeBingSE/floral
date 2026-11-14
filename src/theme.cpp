#include "theme.h"
#include "log.h"

#include <cstdio>
#include <string>

namespace UI {
    UINT32 BG, PANEL, HOVER, SELECT, BORDER;
    UINT32 TEXT, TEXT2, TEXT3, BTN_HOVER, BTN_CLOSE;
    UINT32 SHADOW, ACCENT, ACCENT_TEXT, GHOST, CARD;
    UINT32 DISC;
}

/*
 * ─────────────────────────────────────────────────────────────
 *  Theme table
 *
 *  Adding a palette = adding an entry to kThemes. Field order (see ThemeDef):
 *      name
 *      BG         window background
 *      PANEL      panel/card base
 *      HOVER      hover base
 *      SELECT     selected base (also for accent buttons)
 *      BORDER     border/divider
 *      TEXT       primary text
 *      TEXT2      secondary text
 *      TEXT3      dimmest text (time, placeholder)
 *      BTN_HOVER  window button hover
 *      BTN_CLOSE  close button
 *      SHADOW     shadow color (also progress bar fill)
 *      ACCENT     accent color (hover highlight)
 *      ACCENT_TEXT accent text (current item, icons)
 *      GHOST      subtle base (input field, inactive button)
 *      CARD       one end of card gradient (reserved)
 *
 *  Colors TBD; for now keep only one "current design" entry as the default.
 * ─────────────────────────────────────────────────────────────
 */
const ThemeDef kThemes[] = {
    // Default (current design, sole entry for now)
    { L"默认", 0xFCE4EC, 0xFDF7FA, 0xFAE3EB, 0xF8BBD0, 0xEFD2DD,
      0x3F2B31, 0x8B6571, 0xA8828D, 0xF8BBD0, 0xB88995,
      0x966C77, 0xC25A78, 0x7A5C64, 0xF6EDF1, 0xE9C4CE,
      0x2A2026 },

    // Forget-me-not  peiseka #2484
    // EBE1E6 / D7C9DE / C4B0D5 / B098CD / 9D80C4
    { L"勿忘我", 0xEBE1E6, 0xFFFFFF, 0xD7C9DE, 0xC4B0D5, 0xD7C9DE,
      0x3A2F45, 0x6B5C78, 0x8E7F9C, 0xC4B0D5, 0x9D80C4,
      0x9D80C4, 0xB098CD, 0x6A4F87, 0xEDE6F2, 0xB098CD,
      0x2B2438 },

    // summer  peiseka #3015
    // 0CC8CF / 4AC6FF / 3BAAFF / D0FFB5 / A6FFCE
    // Background lightened from D0FFB5 for text readability
    { L"夏日", 0xE9FFDE, 0xFFFFFF, 0xD9F7E8, 0xA6FFCE, 0xCDEFE3,
      0x14343A, 0x3A6B72, 0x6C969C, 0xA6FFCE, 0x3BAAFF,
      0x1E3A44, 0x7FD8E0 },

    // Rice  peiseka #1826
    // F4F6E9 / E3E5D8 / C0C0B4 / B8B7B3 / 5E544A
    { L"米饭", 0xF4F6E9, 0xFFFFFF, 0xE3E5D8, 0xC0C0B4, 0xDCDDD2,
      0x2E2A25, 0x5E544A, 0x8B8681, 0xC0C0B4, 0x5E544A,
      0x5E544A, 0xB8B7B3, 0x4A423A, 0xEDEEE6, 0xC0C0B4,
      0x2B2620 },

    // Cotton Candy Story  peiseka #1467
    // D4EAFA / F8D4ED / EAB7DB / F7D4ED / D4E9F8
    { L"棉花糖", 0xEDF4FB, 0xFFFFFF, 0xF7D4ED, 0xEAB7DB, 0xE3DAF2,
      0x3B2B44, 0x6E5A78, 0x92819B, 0xEAB7DB, 0x9C7FA8,
      0xB79AC9, 0xF8D4ED, 0x7A5A8C, 0xF1E9F7, 0xEAB7DB,
      0x2E2438 },

    // Industrial Revolution  peiseka #2261 (dark)
    // 0B0C07 / 37372D / 746F60 / EAE6E6 / A49B90
    { L"工业", 0x1A1A15, 0x26261F, 0x2E2E26, 0x3A3A30, 0x3F3F35,
      0xEAE6E6, 0xA49B90, 0x746F60, 0x3A3A30, 0x746F60,
      0x0B0C07, 0xA49B90, 0xEAE6E6, 0x22221C, 0x37372D,
      0x0B0C07 },

    // Ruins of History  peiseka #1799
    // 493B38 / 80716A / BDA77E / 987C6E / 6F5C4D
    // Palette is dark overall; background lightened from BDA77E to parchment
    { L"废墟", 0xEFE6D8, 0xFFFFFF, 0xE4D8C6, 0xBDA77E, 0xDCCFBC,
      0x3A2E2A, 0x6F5C4D, 0x987C6E, 0xBDA77E, 0x493B38,
      0x6F5C4D, 0x987C6E, 0x493B38, 0xF3EBDF, 0xBDA77E,
      0x2B211E },
};

namespace {

int g_cur = 0;
int g_gen = 0;

std::wstring ThemeFile() {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n ? std::wstring(buf, n) : L".") + L"\\KugouPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\theme.txt";
}

void Apply(const ThemeDef& t) {
    UI::BG          = t.BG;
    UI::PANEL       = t.PANEL;
    UI::HOVER       = t.HOVER;
    UI::SELECT      = t.SELECT;
    UI::BORDER      = t.BORDER;
    UI::TEXT        = t.TEXT;
    UI::TEXT2       = t.TEXT2;
    UI::TEXT3       = t.TEXT3;
    UI::BTN_HOVER   = t.BTN_HOVER;
    UI::BTN_CLOSE   = t.BTN_CLOSE;
    UI::SHADOW      = t.SHADOW;
    UI::ACCENT      = t.ACCENT;
    UI::ACCENT_TEXT = t.ACCENT_TEXT;
    UI::GHOST       = t.GHOST;
    UI::CARD        = t.CARD;
    UI::DISC        = t.DISC;
}

}  // namespace

int ThemeCount() { return (int)(sizeof(kThemes) / sizeof(kThemes[0])); }

const wchar_t* ThemeName(int i) {
    if (i < 0 || i >= ThemeCount()) i = 0;
    return kThemes[i].name;
}

int ThemeCurrent() { return g_cur; }
int ThemeGeneration() { return g_gen; }

void ThemeApply(int index) {
    if (index < 0) index = 0;
    if (index >= ThemeCount()) index = ThemeCount() - 1;
    g_cur = index;
    Apply(kThemes[g_cur]);
    g_gen++;
    Logf("APP", "主题 = %s", Narrow(ThemeName(g_cur)).c_str());
}

void ThemeNext() {
    if (ThemeCount() <= 1) return;          // nothing to do with a single theme
    ThemeApply((g_cur + 1) % ThemeCount());
    FILE* f = nullptr;
    _wfopen_s(&f, ThemeFile().c_str(), L"wb");
    if (f) {
        char b[8];
        sprintf(b, "%d", g_cur);
        fputs(b, f);
        fclose(f);
    }
}

void ThemeLoad() {
    FILE* f = nullptr;
    _wfopen_s(&f, ThemeFile().c_str(), L"rb");
    int idx = 0;
    if (f) {
        char b[8] = { 0 };
        if (fread(b, 1, 7, f) > 0) idx = atoi(b);
        fclose(f);
    }
    ThemeApply(idx);
}
