#pragma once
#include <windows.h>

/*
 * 主题系统
 *
 * 布局常量是编译期的；颜色是运行期变量，切换主题时直接改 UI:: 下这些值，
 * 所有 `UI::BG` 之类的调用点不用动。
 */

struct ThemeDef {
    const wchar_t* name;
    UINT32 BG, PANEL, HOVER, SELECT, BORDER, TEXT, TEXT2, TEXT3, BTN_HOVER, BTN_CLOSE;
    UINT32 SHADOW, ACCENT, ACCENT_TEXT, GHOST, CARD;   // 阴影 / 强调 / 强调文字 / 弱底 / 卡片
    UINT32 DISC, DISC_GROOVE;                          // 唱片底色 / 纹路
};

namespace UI {
    /* 布局 */
    constexpr float PAD     = 24.0f;    // 内容左右边距
    constexpr float TITLE_H = 52.0f;    // 自绘标题栏
    constexpr float GAP     = 14.0f;    // 格子间距
    constexpr float R_TILE  = 16.0f;    // 封面圆角
    constexpr float R_BTN   = 8.0f;

    constexpr float BTN_W = 44.0f;
    constexpr float BTN_H = 30.0f;
    constexpr int   BTN_COUNT = 3;

    /* 颜色（运行期，由 ThemeApply 赋值） */
    extern UINT32 BG, PANEL, HOVER, SELECT, BORDER;
    extern UINT32 TEXT, TEXT2, TEXT3, BTN_HOVER, BTN_CLOSE;
    extern UINT32 SHADOW, ACCENT, ACCENT_TEXT, GHOST, CARD;
    extern UINT32 DISC, DISC_GROOVE;
}

int            ThemeCount();
const wchar_t* ThemeName(int i);
int            ThemeCurrent();
void           ThemeApply(int index);   // 切到第 i 套配色
void           ThemeNext();             // 下一套（并保存）
void           ThemeLoad();             // 启动时读取上次的选择
int            ThemeGeneration();       // 每次切换 +1，用来重建依赖主题的画刷
