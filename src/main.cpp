#include "app.h"
#include "resource.h"
#include "kugou.h"
#include "together.h"
#include "log.h"
#include "lang.h"
#include "audio.h"

#include <wininet.h>
#include <thread>
#include <mutex>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <mutex>
#include <set>
#include <deque>
#include <chrono>
#include <atomic>
#include <algorithm>

App g_app;
static HWND g_hwnd = nullptr;      // 主窗口（用于主动重绘）

/* ══════════════════════════════════════════
   绘制基础设施
   ══════════════════════════════════════════ */
static ID2D1Factory*          g_d2d    = nullptr;
static IDWriteFactory*        g_dw     = nullptr;
static IWICImagingFactory*    g_wic    = nullptr;
static ID2D1RenderTarget*     g_rt     = nullptr;
static ID2D1SolidColorBrush*  g_brush  = nullptr;
static ID2D1HwndRenderTarget* g_hwndRt = nullptr;

/** 整层透明度（用于页面切换淡入淡出） */
static float g_alpha = 1.0f;

/** 额外的整体透明度倍率（运动模糊的拖影用） */
static float g_alphaMul = 1.0f;
static float g_plEnter  = 1.0f;   // 歌单页淡入 0→1
static float g_gridEnter = 1.0f;  // 选歌页进场 0→1
static bool      g_scrollDrag = false;   // 拖滚动条中

/** 鼠标动过、需要重画（由定时器统一驱动，避免高频鼠标事件打爆重绘） */
static bool g_mouseDirty = false;

/* 面板进出场的统一包装：淡入 + 围绕锚点 0.96→1 缩放；t 小时不接收点击 */
struct PanelFx {
    float  oldMul;
    size_t hitMark;
    float  t;
    PanelFx(float tt, float ax, float ay) {
        t = tt;
        oldMul = g_alphaMul;
        hitMark = g_app.hits.size();
        g_alphaMul = oldMul * (t * t * (3.0f - 2.0f * t));
        if (g_rt) {
            float sc = 0.96f + 0.04f * t;
            g_rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(ax, ay)));
        }
    }
    ~PanelFx() {
        if (g_rt) g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        g_alphaMul = oldMul;
        // 只裁剪面板内部的按钮：面板刚出现时按钮还小，先不响应\n        if (t < 0.55f && g_app.hits.size() > hitMark) g_app.hits.resize(hitMark);
    }
};
static std::atomic<bool> g_coverDirty{ false };   // 后台线程解好封面后置位，驱动定时器重绘

/** 滚动后要等鼠标真正动一下，才允许悬停放大（免得滚完自己弹开） */
static bool g_hoverArmed = true;

/** 滚动瞬时速度（像素/秒），用于驱动运动模糊 */
static float g_scrollVel = 0.0f;
static float g_prevScroll = 0.0f;

/* 调试用：--fps 在标题栏显示实测帧率 */
static bool g_showFps = false;
static int  g_paintCount = 0;

/* 开屏动画：logo+文字浮现渐隐 → 曲目从右侧飞入排好 → 标题栏浮现 */
static float g_introT = 0.0f;
static bool  g_introDone = false;
static std::atomic<bool> g_bootRefreshPending{ false };   // 启动时还要刷新歌单
static bool  g_loadDone  = false;    // 启动资源加载完成（歌单 + 封面）
static float g_loadT     = 0.0f;

static const float SPLASH_IN       = 0.40f;   // logo 淡入
static const float SPLASH_HOLD_END = 0.75f;   // 停留到这一刻
static const float SPLASH_OUT      = 1.10f;   // 淡出结束（曲目随即开始）
static const float INTRO_TILE_START = 1.10f;
static const float INTRO_STAGGER    = 0.05f;  // 每块间隔
static const float INTRO_FLY        = 0.42f;  // 单块飞行时长
static const float INTRO_BAR        = 0.85f;  // 标题栏开始浮现（相对曲目开始）

static D2D1_COLOR_F Col(UINT32 rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f,
                        ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f,
                        a * g_alpha * g_alphaMul);
}
static D2D1_RECT_F Rc(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}
static bool Inside(const D2D1_RECT_F& r, const POINT& p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

/* 封面配色：8 组莫兰迪渐变色块（浅 → 中） */
struct CoverPair { UINT32 a, b; };
static const CoverPair COVERS[8] = {
    { 0xF9E1E8, 0xE2B3C2 },   // 玫瑰
    { 0xF3DCE6, 0xD2A8BE },   // 藕荷
    { 0xE6DDEB, 0xC3B2D2 },   // 灰紫
    { 0xDCE6EF, 0xB2C4D8 },   // 雾蓝
    { 0xE3EBE0, 0xBACBB6 },   // 灰绿
    { 0xEFE6D9, 0xD6C3A8 },   // 沙
    { 0xF0DED5, 0xD3B3A2 },   // 灰橙
    { 0xE9E3E9, 0xC0B0BE },   // 灰粉
};

/* 封面图片缓存 */
struct CoverSrc { std::wstring path; IWICFormatConverter* conv = nullptr; };
static std::vector<CoverSrc> g_coverSrcs;

struct CoverBmp {
    IWICFormatConverter* src = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    ID2D1BitmapBrush* brush = nullptr;   // 圆角填充用，避免每帧建 Layer
};
static std::vector<CoverBmp> g_coverBmps;

/* 文本格式缓存 */
struct FmtEntry { float size; DWRITE_FONT_WEIGHT weight; IDWriteTextFormat* f; };
static std::vector<FmtEntry> g_fmts;

static IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT weight) {
    for (auto& e : g_fmts)
        if (e.size == size && e.weight == weight) return e.f;
    IDWriteTextFormat* f = nullptr;
    if (FAILED(g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &f)))
        return nullptr;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_fmts.push_back({ size, weight, f });
    return f;
}

/** 两个颜色按比例混合 */
static UINT32 MixColor(UINT32 a, UINT32 b, float t) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (UINT32)((r << 16) | (g << 8) | bl);
}

/** 用当前主题色生成 8 组占位渐变（没有封面时用） */
static ID2D1LinearGradientBrush* g_covBrush[8] = { nullptr };
static void BuildCoverBrushes(ID2D1RenderTarget* rt) {
    for (int i = 0; i < 8; i++) {
        if (g_covBrush[i]) { g_covBrush[i]->Release(); g_covBrush[i] = nullptr; }
    }
    if (!rt) return;

    for (int i = 0; i < 8; i++) {
        // 浅端在 SELECT/ACCENT 之间来回，深端压到底色系
        float t1 = 0.25f + 0.10f * (i % 4);
        float t2 = 0.55f + 0.08f * ((i / 2) % 3);
        UINT32 lite = MixColor(UI::ACCENT, UI::PANEL, t1);
        UINT32 deep = MixColor(UI::SHADOW, UI::SELECT, t2);

        D2D1_GRADIENT_STOP stops[2];
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(((lite >> 16) & 0xFF) / 255.0f,
                                     ((lite >> 8) & 0xFF) / 255.0f,
                                     (lite & 0xFF) / 255.0f, 1.0f);
        stops[1].position = 1.0f;
        stops[1].color = D2D1::ColorF(((deep >> 16) & 0xFF) / 255.0f,
                                     ((deep >> 8) & 0xFF) / 255.0f,
                                     (deep & 0xFF) / 255.0f, 1.0f);

        ID2D1GradientStopCollection* sc = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, 2, &sc))) continue;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES p = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0, 0), D2D1::Point2F(1, 1));
        rt->CreateLinearGradientBrush(p, sc, &g_covBrush[i]);
        sc->Release();
    }
}

/* 封面占位渐变画刷：8 个，按渲染目标缓存，避免每帧重建 */

/* 封面淡入：记录每张图"第一次能画"的时刻，150ms 内从 0 淡到 1 */
static std::map<std::wstring, ULONGLONG> g_coverSeen;

static bool  g_noCoverFade = false;     // 截图模式：不淡入，直接满
static float g_coverFadeOverride = -1.0f;  // 截图模式：定格淡入进度

static void MarkCoverReady(const std::wstring& path) {
    if (path.empty()) return;
    if (g_coverSeen.find(path) == g_coverSeen.end())
        g_coverSeen[path] = GetTickCount64();
}

static float CoverFade(const std::wstring& path) {
    if (g_coverFadeOverride >= 0.0f) return g_coverFadeOverride;
    if (g_noCoverFade || path.empty()) return 1.0f;
    auto it = g_coverSeen.find(path);
    if (it == g_coverSeen.end()) return 1.0f;        // 没记录：当作早就好了
    float t = (float)(GetTickCount64() - it->second) / 150.0f;
    return t >= 1.0f ? 1.0f : t;
}
static void Bind(ID2D1RenderTarget* rt) {
    if (g_rt == rt) return;
    g_rt = rt;
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    for (int i = 0; i < 8; i++) {
        if (g_covBrush[i]) { g_covBrush[i]->Release(); g_covBrush[i] = nullptr; }
    }
    // 位图/画刷都绑定在渲染目标上，换目标要重建
    for (auto& b : g_coverBmps) {
        if (b.brush) b.brush->Release();
        if (b.bmp) b.bmp->Release();
    }
    g_coverBmps.clear();
    g_coverSeen.clear();
    if (!rt) return;

    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_brush);

    BuildCoverBrushes(rt);
}

static void FillRect(const D2D1_RECT_F& r, UINT32 c, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->FillRectangle(r, g_brush);
}
static void FillRound(const D2D1_RECT_F& r, float rad, UINT32 c, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_brush);
}
static void FillRoundGrad(const D2D1_RECT_F& r, float rad, UINT32 c1, UINT32 c2, float a = 1.0f) {
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f; stops[0].color = Col(c1, a);
    stops[1].position = 1.0f; stops[1].color = Col(c2, a);
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(g_rt->CreateGradientStopCollection(stops, 2, &sc))) return;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES p = D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom));
    ID2D1LinearGradientBrush* b = nullptr;
    if (SUCCEEDED(g_rt->CreateLinearGradientBrush(p, sc, &b))) {
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b);
        b->Release();
    }
    sc->Release();
}
static void FillRectGradV(const D2D1_RECT_F& r, UINT32 c1, float a1, UINT32 c2, float a2) {
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f; stops[0].color = Col(c1, a1);
    stops[1].position = 1.0f; stops[1].color = Col(c2, a2);
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(g_rt->CreateGradientStopCollection(stops, 2, &sc))) return;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES p = D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom));
    ID2D1LinearGradientBrush* b = nullptr;
    if (SUCCEEDED(g_rt->CreateLinearGradientBrush(p, sc, &b))) {
        g_rt->FillRectangle(r, b);
        b->Release();
    }
    sc->Release();
}

static void FillRoundGradV(const D2D1_RECT_F& r, float rad,
                           UINT32 c1, float a1, UINT32 c2, float a2) {
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f; stops[0].color = Col(c1, a1);
    stops[1].position = 1.0f; stops[1].color = Col(c2, a2);
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(g_rt->CreateGradientStopCollection(stops, 2, &sc))) return;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES p = D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom));
    ID2D1LinearGradientBrush* b = nullptr;
    if (SUCCEEDED(g_rt->CreateLinearGradientBrush(p, sc, &b))) {
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b);
        b->Release();
    }
    sc->Release();
}

static void HLine(float x1, float x2, float y, UINT32 c, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y), g_brush, 1.0f);
}
static void StrokeRound(const D2D1_RECT_F& r, float rad, UINT32 c, float w = 1.0f, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_brush, w);
}
static void StrokeRect(const D2D1_RECT_F& r, UINT32 c, float w = 1.0f, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->DrawRectangle(r, g_brush, w);
}
static void Line(float x1, float y1, float x2, float y2, UINT32 c, float w = 1.0f, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush, w);
}
static void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r,
                 UINT32 c, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                 float a = 1.0f) {
    if (!f || !g_brush || s.empty()) return;
    f->SetTextAlignment(align);
    g_brush->SetColor(Col(c, a));
    g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

/* 单行文字，超出部分省略号（歌单卡片里用） */
static void TextEllipsis(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r,
                         UINT32 c, float a = 1.0f) {
    if (!f || !g_brush || !g_dw || s.empty()) return;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f,
                                      r.right - r.left, r.bottom - r.top, &layout)) || !layout)
        return;
    D2D1_POINT_2F origin = D2D1::Point2F(r.left, r.top);
    g_brush->SetColor(Col(c, a));

    // 先量一遍：放得下就直接画，放不下才做省略号
    DWRITE_TEXT_METRICS tm{};
    bool fits = SUCCEEDED(layout->GetMetrics(&tm)) && tm.widthIncludingTrailingWhitespace <= (r.right - r.left) + 0.5f;
    if (!fits) {
        DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* ell = nullptr;
        if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(layout, &ell)) && ell) {
            layout->SetTrimming(&tr, ell);
            ell->Release();
        }
    }
    g_rt->DrawTextLayout(origin, layout, g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

static std::wstring FormatTime(int sec) {
    if (sec < 0) sec = 0;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d:%02d", sec / 60, sec % 60);
    return buf;
}

/* ══════════════════════════════════════════
   网格布局（不规则拼块）
   ══════════════════════════════════════════ */
struct TileBox {
    float x, y, w, h;    // 相对内容原点（未减滚动）
    int cw, ch;          // 占几个格子
};

/* 三份布局：
 *   g_base   —— 不含悬停，稳定，用于鼠标命中
 *   g_target —— 含悬停（该块撑到 2×2），其余块被挤开
 *   g_draw   —— 动画中的当前值，绘制用
 */
static std::vector<TileBox> g_base, g_target, g_draw;
static float g_baseHeight = 0.0f;
static float g_gridHeight = 0.0f;
static int   g_layoutCount = -1;
static float g_layoutWidth = -1.0f;
static int   g_layoutHover = -2;
static bool  g_layoutDirty = false;

/* 悬停块：原地锚定放大（不重排，避免整块跳到别处），靠边时朝中间长 */
static float g_cellSize = 140.0f;
static int   g_colsNow = 6;
static int   g_hvIdx = -1;               // 正在动画的悬停块
static D2D1_RECT_F g_hvCur{};
static D2D1_RECT_F g_hvTarget{};
static bool  g_snapUI = false;           // 离屏截图时直接到终态

/* 纵向分条索引：每帧只处理视口附近的那几条，不再扫全部 2000 块 */
static const float BAND_H = 220.0f;
static std::vector<std::vector<int>> g_baseBands, g_tgtBands;

/* 本帧可见块（去重用 stamp） */
static std::vector<int> g_vis;
static std::vector<uint32_t> g_stamp;
static uint32_t g_stampVal = 0;

static const float GRID_TOP = UI::TITLE_H + 18.0f;

static inline uint32_t Hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/** 每块占几格：大部分 1×1，少量横向/纵向长的，极少数 2×2 */
static void PickSize(int i, int cols, int& cw, int& ch) {
    int r = (int)(Hash32((uint32_t)i * 2654435761u) % 100u);
    if (cols >= 2 && r < 3)       { cw = 2; ch = 2; }
    else if (cols >= 2 && r < 15) { cw = 2; ch = 1; }
    else if (r < 27)              { cw = 1; ch = 2; }
    else                          { cw = 1; ch = 1; }
}

/** 打包：hoverIdx 那块撑到 2×2，并锚定在 hoverCol 这一列（靠边缘时朝中间长）；
 *  其余块按最低空位顺延，自然被挤开。 */
static void PackLayout(float W, int count, int hoverIdx, int hoverCol,
                       std::vector<TileBox>& out, float& totalHeight) {
    int cols = (int)((W - UI::PAD * 2 + UI::GAP) / 150.0f);
    if (cols < 3) cols = 3;
    if (cols > 10) cols = 10;
    float cell = (W - UI::PAD * 2 - UI::GAP * (cols - 1)) / cols;
    g_cellSize = cell;
    g_colsNow = cols;

    out.clear();
    out.reserve((size_t)count);

    std::vector<int> heights((size_t)cols, 0);

    for (int i = 0; i < count; i++) {
        int cw = 1, ch = 1;
        bool anchored = (i == hoverIdx);

        if (anchored) {
            cw = 2; ch = 2;
            if (cw > cols) cw = cols;
        } else {
            PickSize(i, cols, cw, ch);
            if (cw > cols) cw = cols;
        }

        // 锚定列：右边缘往左长，其余往右长
        int anchorCol = -1;
        if (anchored) {
            anchorCol = (std::min)(hoverCol < 0 ? 0 : hoverCol, cols - cw);
            if (anchorCol < 0) anchorCol = 0;
        }

        int bestCol = 0, bestRow = 0x7fffffff;
        if (anchorCol >= 0) {
            int r = 0;
            for (int k = 0; k < cw; k++) r = (std::max)(r, heights[(size_t)(anchorCol + k)]);
            bestCol = anchorCol;
            bestRow = r;
        } else {
            for (int c = 0; c + cw <= cols; c++) {
                int r = 0;
                for (int k = 0; k < cw; k++) r = (std::max)(r, heights[(size_t)(c + k)]);
                if (r < bestRow) { bestRow = r; bestCol = c; }
            }
        }
        for (int k = 0; k < cw; k++) heights[(size_t)(bestCol + k)] = bestRow + ch;

        TileBox b;
        b.cw = cw; b.ch = ch;
        b.x = UI::PAD + bestCol * (cell + UI::GAP);
        b.y = bestRow * (cell + UI::GAP);
        b.w = cw * cell + (cw - 1) * UI::GAP;
        b.h = ch * cell + (ch - 1) * UI::GAP;
        out.push_back(b);
    }

    int rows = 0;
    for (int h : heights) rows = (std::max)(rows, h);
    totalHeight = rows * (cell + UI::GAP) - UI::GAP;
}

/** 把块按纵向分条，绘制/命中时只取相关条（复用容量，避免每次重排重分配） */
static void BuildBands(const std::vector<TileBox>& boxes, std::vector<std::vector<int>>& bands) {
    if (boxes.empty()) { bands.clear(); return; }

    float maxY = 0.0f;
    for (const auto& b : boxes) maxY = (std::max)(maxY, b.y + b.h);
    int nb = (int)(maxY / BAND_H) + 1;

    if ((int)bands.size() != nb) bands.resize((size_t)nb);
    for (auto& v : bands) v.clear();          // clear 保留 capacity

    for (int i = 0; i < (int)boxes.size(); i++) {
        const TileBox& b = boxes[(size_t)i];
        int b0 = (int)(b.y / BAND_H);
        int b1 = (int)((b.y + b.h - 0.01f) / BAND_H);
        if (b0 < 0) b0 = 0;
        if (b1 >= nb) b1 = nb - 1;
        for (int k = b0; k <= b1; k++) bands[(size_t)k].push_back(i);
    }
}

static void EnsureBase(float W, int count) {
    if (count == g_layoutCount && W == g_layoutWidth) return;
    PackLayout(W, count, -1, -1, g_base, g_baseHeight);
    BuildBands(g_base, g_baseBands);
    g_layoutCount = count;
    g_layoutWidth = W;
    g_layoutHover = -2;                 // 强制重算目标
}

static void EnsureTarget(float W, int count, int hoverIdx) {
    EnsureBase(W, count);
    if (hoverIdx == g_layoutHover) return;

    // 取这块在基础布局里的列，作为放大后的锚定列
    int hcol = -1;
    if (hoverIdx >= 0 && hoverIdx < (int)g_base.size()) {
        float pitch = g_cellSize + UI::GAP;
        if (pitch > 0.0f)
            hcol = (int)std::lround((g_base[(size_t)hoverIdx].x - UI::PAD) / pitch);
    }

    PackLayout(W, count, hoverIdx, hcol, g_target, g_gridHeight);
    BuildBands(g_target, g_tgtBands);
    g_layoutHover = hoverIdx;
    g_layoutDirty = true;
    if (g_draw.size() != g_target.size()) {
        g_draw = g_target;
        g_layoutDirty = false;
    }
}

/** 鼠标落在哪个块上（-1 表示没有）
 *  先判当前悬停块放大后的整块区域，否则放大出来的地方会被算到邻居头上 */
static int HitTile(POINT p) {
    App& a = g_app;
    if (a.songs.empty()) return -1;
    if (p.y < GRID_TOP - 6) return -1;
    EnsureBase(a.width, (int)a.songs.size());

    // 1) 已悬停的块：放大区域整体都算它的（用目标矩形，稳定不抖）
    int cur = a.hoverTile;
    if (cur >= 0 && cur < (int)g_target.size()) {
        const TileBox& h = g_target[(size_t)cur];
        D2D1_RECT_F hr = Rc(h.x, GRID_TOP + h.y - a.scroll, h.w, h.h);
        if (Inside(hr, p)) return cur;
    }

    // 2) 其余按基础布局命中（分条查，不扫全表）
    float cy = (float)p.y - GRID_TOP + a.scroll;
    int band = (int)(cy / BAND_H);
    if (band < 0 || band >= (int)g_baseBands.size()) return -1;

    for (int i : g_baseBands[(size_t)band]) {
        const TileBox& b = g_base[(size_t)i];
        D2D1_RECT_F r = Rc(b.x, GRID_TOP + b.y - a.scroll, b.w, b.h);
        if (Inside(r, p)) return i;
    }
    return -1;
}

/* ══════════════════════════════════════════
   封面
   ══════════════════════════════════════════ */
/* ───────── 后台解码线程 ─────────
   WIC 解码一张 400px 的 JPEG 要 2~4ms，放在 UI 线程上滚屏就是灾难。
   这里改成后台线程解码，主线程只负责把解好的结果包成 D2D 位图。
   ─────────────────────────────── */
static std::mutex  g_decMutex;
static std::deque<std::wstring> g_decQueue;
static std::vector<std::pair<std::wstring, IWICFormatConverter*>> g_decDone;
static std::vector<std::thread> g_decThreads;
static std::atomic<bool> g_decRun{ false };
static std::set<std::wstring> g_decRequested;      // 主线程独占

static IWICFormatConverter* DecodeScaled(IWICImagingFactory* wic,
                                         const std::wstring& path, UINT maxSize) {
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;

    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(dec->GetFrame(0, &frame))) { dec->Release(); return nullptr; }

    IWICBitmapSource* src = frame;
    IWICBitmapScaler* scaler = nullptr;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w && h && (w > maxSize || h > maxSize)) {
        float s = (float)maxSize / (float)((w > h) ? w : h);
        UINT nw = (UINT)(w * s), nh = (UINT)(h * s);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, nw, nh, WICBitmapInterpolationModeFant)))
            src = scaler;
    }

    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateFormatConverter(&conv))) {
        if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) {
            conv->Release();
            conv = nullptr;
        }
    }

    if (scaler) scaler->Release();
    frame->Release();
    dec->Release();
    return conv;
}

static void DecodeWorkerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    while (g_decRun.load()) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lk(g_decMutex);
            if (!g_decQueue.empty()) {
                path = std::move(g_decQueue.front());
                g_decQueue.pop_front();
            }
        }
        if (path.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        IWICFormatConverter* conv = wic ? DecodeScaled(wic, path, 320) : nullptr;
        {
            std::lock_guard<std::mutex> lk(g_decMutex);
            g_decDone.emplace_back(std::move(path), conv);
        }
        g_coverDirty = true;            // 通知界面：有新封面可以用了
    }

    if (wic) wic->Release();
    CoUninitialize();
}

static void QueueDecode(const std::wstring& path) {
    if (path.empty()) return;
    if (g_decRequested.count(path)) return;
    g_decRequested.insert(path);
    std::lock_guard<std::mutex> lk(g_decMutex);
    g_decQueue.push_back(path);
}

/* ── 封面按需下载：列表里没本地封面的歌，滚动到就下 ── */
static std::string  HttpGet(const std::wstring& url);
static std::wstring FindProjectFile(const std::wstring& name);
static std::wstring Widen(const std::string& s);

/* 队列里带的是 url/hash 的副本，工作线程完全不碰 g_app（避免和清缓存/重载抢数据） */
struct CovJob {
    int         idx;
    std::string url;
    std::string hash;
};

extern std::string g_token, g_userid;   // 定义在下面

/* 歌单列表的跨线程交接 */
static std::mutex          g_plMx;
static std::vector<Playlist> g_plPending;
static std::atomic<bool>   g_plReady{ false };

static std::mutex        g_covMx;
static std::vector<CovJob> g_covQueue;
static std::vector<int>  g_covDone;
static std::vector<char> g_covFlag;      // 0 未处理 / 1 排队中 / 2 处理过

static std::wstring AssetDir() {
    std::wstring p = FindProjectFile(L"kugou-likes.tsv");
    if (p.empty()) return L".";
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"." : p.substr(0, s);
}

static void CoverWorkerMain() {
    for (;;) {
        CovJob job{ -1, "", "" };
        {
            std::lock_guard<std::mutex> lk(g_covMx);
            if (!g_covQueue.empty()) { job = g_covQueue.back(); g_covQueue.pop_back(); }
        }
        if (job.idx < 0) { Sleep(50); continue; }

        if (!job.url.empty() && !job.hash.empty()) {
            std::string bytes = HttpGet(Widen(job.url));
            if (bytes.size() > 1000) {
                std::wstring dir = AssetDir() + L"\\covers";
                CreateDirectoryW(dir.c_str(), nullptr);
                std::wstring path = dir + L"\\" + std::wstring(job.hash.begin(), job.hash.end()) + L".jpg";
                FILE* f = nullptr;
                _wfopen_s(&f, path.c_str(), L"wb");
                if (f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_covMx);
            g_covDone.push_back(job.idx);
        }
        g_coverDirty = true;            // 下载完了，叫界面来收
    }
}

/* ══════════════════════════════════════════
   歌单列表（选择页用）
   ══════════════════════════════════════════ */
static std::wstring PlaylistsFile() { return AssetDir() + L"\\playlists.tsv"; }

/* ── 界面语言 ── */
static std::wstring LangFile() { return AssetDir() + L"\\lang.txt"; }
static void LoadLang() {
    FILE* f = nullptr;
    _wfopen_s(&f, LangFile().c_str(), L"rb");
    if (!f) return;
    char buf[16] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;
    g_lang = (buf[0] == 'e' || buf[0] == 'E') ? Lang::En : Lang::Zh;
    Logf("APP", "界面语言 = %s", g_lang == Lang::En ? "en" : "zh");
}
static void SaveLang() {
    FILE* f = nullptr;
    _wfopen_s(&f, LangFile().c_str(), L"wb");
    if (!f) return;
    fputs(g_lang == Lang::En ? "en" : "zh", f);
    fclose(f);
}

static void LoadPlaylists() {
    g_app.playlists.clear();
    FILE* f = nullptr;
    _wfopen_s(&f, PlaylistsFile().c_str(), L"rb");
    if (!f) return;
    char line[2048];
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        if (first && s.rfind("name\t", 0) == 0) { first = false; continue; }   // 表头
        first = false;
        // name \t gcid \t count \t coverUrl
        std::vector<std::string> col;
        size_t p = 0;
        while (col.size() < 4) {
            size_t q = s.find('\t', p);
            if (q == std::string::npos) { col.push_back(s.substr(p)); break; }
            col.push_back(s.substr(p, q - p));
            p = q + 1;
        }
        if (col.size() < 2 || col[1].empty()) continue;
        Playlist pl;
        int n = MultiByteToWideChar(CP_UTF8, 0, col[0].data(), (int)col[0].size(), nullptr, 0);
        pl.name.resize((size_t)n);
        MultiByteToWideChar(CP_UTF8, 0, col[0].data(), (int)col[0].size(), &pl.name[0], n);
        pl.gcid = col[1];
        pl.count = (col.size() > 2) ? atoi(col[2].c_str()) : 0;
        pl.coverUrl = (col.size() > 3) ? col[3] : "";
        g_app.playlists.push_back(std::move(pl));
    }
    fclose(f);
    Logf("APP", "歌单列表载入 %d 个", (int)g_app.playlists.size());
}

static void SavePlaylists() {
    FILE* f = nullptr;
    _wfopen_s(&f, PlaylistsFile().c_str(), L"wb");
    if (!f) return;
    fputs("name\tgcid\tcount\tcoverUrl\n", f);
    for (const Playlist& pl : g_app.playlists) {
        std::string name;
        int n = WideCharToMultiByte(CP_UTF8, 0, pl.name.c_str(), (int)pl.name.size(),
                                    nullptr, 0, nullptr, nullptr);
        name.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, pl.name.c_str(), (int)pl.name.size(),
                            &name[0], n, nullptr, nullptr);
        fprintf(f, "%s\t%s\t%d\t%s\n", name.c_str(), pl.gcid.c_str(), pl.count, pl.coverUrl.c_str());
    }
    fclose(f);
}

/* ── 歌单封面：路径按 gcid 派生，文件存在就直接解码 ── */
static std::wstring PlCoverPath(const Playlist& pl) {
    std::string h = pl.gcid;
    for (char& c : h) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    return AssetDir() + L"\\covers\\pl_" + std::wstring(h.begin(), h.end()) + L".jpg";
}

static std::set<std::string> g_plCovAsked;
static std::atomic<int>      g_plCovBusy{ 0 };

static void RequestPlaylistCover(int i) {
    if (i < 0 || i >= (int)g_app.playlists.size()) return;
    const Playlist& pl = g_app.playlists[i];
    if (pl.coverUrl.empty() || g_plCovAsked.count(pl.gcid)) return;

    std::wstring path = PlCoverPath(pl);
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_plCovAsked.insert(pl.gcid);
        QueueDecode(path);
        return;
    }
    if (g_plCovBusy.load() >= 4) return;      // 同时最多 4 个

    g_plCovAsked.insert(pl.gcid);
    g_plCovBusy.fetch_add(1);
    std::string url = pl.coverUrl;
    // 接口给的地址里有 {size} 占位符，换成实际尺寸
    size_t szp = url.find("{size}");
    if (szp != std::string::npos) url.replace(szp, 6, "400");
    std::thread([url, path]() {
        std::string bytes = HttpGet(Widen(url));
        if (bytes.size() > 500) {
            CreateDirectoryW((AssetDir() + L"\\covers").c_str(), nullptr);
            FILE* f = nullptr;
            _wfopen_s(&f, path.c_str(), L"wb");
            if (f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
            QueueDecode(path);
        }
        g_plCovBusy.fetch_sub(1);
        g_coverDirty = true;
    }).detach();
}
/* 后台线程：从接口拉全部歌单并落盘 */
static void FetchPlaylistsAsync() {
    if (g_token.empty()) return;
    std::vector<KgPlaylist> got;
    std::wstring err;
    int n = KgFetchPlaylists(g_token, g_userid, got, err);
    if (n <= 0) {
        g_bootRefreshPending = false;   // 失败也别把开屏卡住
        Logf("ERR", "拉歌单列表失败: %s", Narrow(err).c_str());
        return;
    }

    std::lock_guard<std::mutex> lk(g_plMx);
    g_plPending.clear();
    for (auto& kp : got) {
        Playlist pl;
        pl.name = kp.name;
        pl.gcid = kp.gcid;
        pl.count = kp.count;
        pl.coverUrl = kp.coverUrl;
        pl.isDefault = kp.isDefault;
        g_plPending.push_back(std::move(pl));
    }
    g_plReady = true;
    g_bootRefreshPending = false;       // 首屏只等这一件事
    Logf("APP", "歌单列表拉取完成 %d 个", n);
}
/** 主线程：把下好的封面接进缓存 */
/** 还有没有封面相关的活没干完（定时器据此保持重绘） */
static bool CoverWorkPending() {
    {
        std::lock_guard<std::mutex> lk(g_covMx);
        if (!g_covQueue.empty() || !g_covDone.empty()) return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_decMutex);
        if (!g_decDone.empty()) return true;
    }
    return false;
}

static void DrainCovers() {
    std::vector<int> done;
    { std::lock_guard<std::mutex> lk(g_covMx); done.swap(g_covDone); }
    if (done.empty()) return;

    for (int i : done) {
        if (i < 0 || i >= (int)g_app.songs.size()) continue;
        Song& s = g_app.songs[(size_t)i];
        std::wstring path = AssetDir() + L"\\covers\\" +
            std::wstring(s.hash.begin(), s.hash.end()) + L".jpg";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && s.cover.empty()) {
            s.cover = path;
            QueueDecode(path);
        }
        if (i < (int)g_covFlag.size()) g_covFlag[(size_t)i] = 2;
    }
    g_coverDirty = true;                // 有新封面进了解码队列
}

/** 视口里的歌如果没封面就排队下载（只在这里读 g_app，工作线程拿的是副本） */
static void RequestVisibleCovers() {
    std::lock_guard<std::mutex> lk(g_covMx);
    if (g_covFlag.size() != g_app.songs.size()) return;
    for (int i : g_vis) {
        if (i < 0 || i >= (int)g_app.songs.size()) continue;
        if (g_covFlag[(size_t)i]) continue;
        const Song& s = g_app.songs[(size_t)i];
        if (!s.cover.empty() || s.coverUrl.empty() || s.hash.empty()) continue;
        g_covFlag[(size_t)i] = 1;
        g_covQueue.push_back({ i, s.coverUrl, s.hash });
    }
}

static void StartDecoder() {
    if (g_decRun.load()) return;
    g_decRun.store(true);
    // 按核数开，最多 8 个（解码是纯 CPU，WIC 每线程一个工厂）
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    if (n > 8) n = 8;
    if (n < 2) n = 2;
    for (unsigned i = 0; i < n; i++) g_decThreads.emplace_back(DecodeWorkerMain);
    Logf("APP", "封面解码线程 %u 个", n);
}

static void StopDecoder() {
    g_decRun.store(false);
    for (auto& th : g_decThreads) if (th.joinable()) th.join();
    g_decThreads.clear();
    std::lock_guard<std::mutex> lk(g_decMutex);
    for (auto& p : g_decDone) if (p.second) p.second->Release();
    g_decDone.clear();
    g_decQueue.clear();
}

/** 主线程：把后台解好的封面搬进缓存 */
static void DrainDecoded() {
    std::vector<std::pair<std::wstring, IWICFormatConverter*>> tmp;
    {
        std::lock_guard<std::mutex> lk(g_decMutex);
        if (g_decDone.empty()) return;
        tmp.swap(g_decDone);
    }
    for (auto& p : tmp) { MarkCoverReady(p.first); g_coverSrcs.push_back({ std::move(p.first), p.second }); }
    g_mouseDirty = true;              // 有新封面可用，触发一次重绘
}

static IWICFormatConverter* CoverSource(const std::wstring& path);   // 前置声明

/** 首屏封面是否都解好了（用于开屏多等一会儿） */
static bool CoversReadyForFirstScreen() {
    // 启动只加载歌单封面，歌曲封面等真正进歌单时再按需解码
    return true;
}
/** 把磁盘上已存在的封面补回 s.cover 并排进解码队列
    之前只认 TSV 第 5 列，而导出的 tsv 这列是空的，导致已下载的封面被当成没有 */
static void AdoptLocalCovers() {
    int adopted = 0;
    for (int i = 0; i < (int)g_app.songs.size(); i++) {
        Song& s = g_app.songs[(size_t)i];
        if (!s.cover.empty() || s.hash.empty()) continue;
        std::wstring p = AssetDir() + L"\\covers\\" +
                         std::wstring(s.hash.begin(), s.hash.end()) + L".jpg";
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        s.cover = p;            // 只记路径；解码等真正要画这张封面时再做
        adopted++;
    }
    if (adopted) Logf("APP", "复用本地封面 %d 张", adopted);
}
/* 预建 D2D 位图：滚动时就不用现场创建+上传显存了 */
static IWICFormatConverter* CoverSource(const std::wstring& path);   // 定义在后面
static CoverBmp* CoverEntry(const std::wstring& path);
static size_t g_prewarmIdx = 0;
static void PrewarmCovers(int budget) {
    if (!g_rt || g_app.songs.empty()) return;
    int made = 0;
    while (g_prewarmIdx < g_app.songs.size() && made < budget) {
        const Song& s = g_app.songs[g_prewarmIdx++];
        if (s.cover.empty()) continue;
        if (!CoverSource(s.cover)) continue;      // 还没解好，下一轮再看
        if (CoverEntry(s.cover)) made++;
    }
}

/** 取封面（已解码才有） */
static IWICFormatConverter* CoverSource(const std::wstring& path) {
    if (path.empty()) return nullptr;
    for (auto& c : g_coverSrcs) if (c.path == path) return c.conv;
    return nullptr;
}

static ID2D1Bitmap* CoverBitmap(const std::wstring& path);
static CoverBmp* CoverEntry(const std::wstring& path) {
    IWICFormatConverter* src = CoverSource(path);
    if (!src) {
        QueueDecode(path);           // 还没解好：丢给后台，先画占位
        return nullptr;
    }
    for (auto& b : g_coverBmps)
        if (b.src == src && b.rt == g_rt) return &b;

    g_coverBmps.push_back({ src, g_rt, nullptr, nullptr });
    MarkCoverReady(path);
    CoverBmp& e = g_coverBmps.back();
    if (FAILED(g_rt->CreateBitmapFromWicBitmap(src, nullptr, &e.bmp))) {
        g_coverBmps.pop_back();
        return nullptr;
    }
    return &e;
}

/** 用位图画刷填充圆角矩形 —— 不用 Layer，比 PushLayer 快一个数量级 */

static void DrawBitmapRounded(CoverBmp* e, const D2D1_RECT_F& r, float rad, float alpha = 1.0f) {
    if (!e || !e->bmp) return;
    if (!e->brush) {
        D2D1_BITMAP_BRUSH_PROPERTIES props = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP);
        if (FAILED(g_rt->CreateBitmapBrush(e->bmp, &props, nullptr, &e->brush))) {
            // 画刷创建失败就直接铺矩形，至少不卡
            g_rt->DrawBitmap(e->bmp, &r, g_alpha * g_alphaMul * alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
    }
    D2D1_SIZE_F sz = e->bmp->GetSize();
    if (sz.width <= 0.0f || sz.height <= 0.0f) return;

    // 等比裁切（cover）：取较大缩放比铺满，再居中裁掉溢出，绝不拉伸变形
    float rw = r.right - r.left;
    float rh = r.bottom - r.top;
    float sc = (std::max)(rw / sz.width, rh / sz.height);
    float ox = r.left + (rw - sz.width * sc) * 0.5f;
    float oy = r.top + (rh - sz.height * sc) * 0.5f;

    e->brush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) *
                           D2D1::Matrix3x2F::Translation(ox, oy));
    e->brush->SetOpacity(g_alpha * g_alphaMul * alpha);   // 位图画刷不吃颜色 alpha，得单独设
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), e->brush);
}

static void DrawCoverArt(const D2D1_RECT_F& r, const Song& s, int idx, float rad) {
    CoverBmp* e = CoverEntry(s.cover);
    float fade = e ? CoverFade(s.cover) : 0.0f;
    if (e && fade >= 1.0f) {                 // 已经完全显示，直接铺
        DrawBitmapRounded(e, r, rad);
        return;
    }
    // 没有封面：渐变占位 + 首字（画刷已缓存，不再每帧新建）
    int ci = ((idx % 8) + 8) % 8;
    if (g_covBrush[ci]) {
        g_covBrush[ci]->SetStartPoint(D2D1::Point2F(r.left, r.top));
        g_covBrush[ci]->SetEndPoint(D2D1::Point2F(r.right, r.bottom));
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_covBrush[ci]);
    } else {
        FillRoundGrad(r, rad, MixColor(UI::ACCENT, UI::PANEL, 0.25f + 0.1f * (ci % 4)),
                      MixColor(UI::SHADOW, UI::SELECT, 0.55f + 0.08f * ((ci / 2) % 3)));
    }
    if (s.title.empty()) return;
    std::wstring ch(1, s.title[0]);
    float size = (r.bottom - r.top) * 0.32f;
    // 首字跟着淡出，避免和真图叠在一起
    Text(ch, Fmt(size, DWRITE_FONT_WEIGHT_SEMI_BOLD), r, 0xFFFFFF, DWRITE_TEXT_ALIGNMENT_CENTER,
         0.55f * (1.0f - fade));

    if (e) DrawBitmapRounded(e, r, rad, fade * fade);   // 正在淡入：压在占位上面
}

/* ══════════════════════════════════════════
   窗口按钮
   ══════════════════════════════════════════ */
static D2D1_RECT_F BtnRect(int i, float W) {
    float x = W - UI::BTN_W * (float)(UI::BTN_COUNT - i);
    float y = 11.0f;
    return Rc(x, y, UI::BTN_W, UI::BTN_H);
}

/* ── 设置面板 ── */
std::string  g_token, g_userid;      // 登录状态（LoadSession 填）
static bool         g_settingsOpen = false;

/* 面板出现/消失：0 收起，1 展开。绘制时用 PanelFx 包一层，淡入 + 轻微缩放 */
static float g_playIconMix = 0.0f;    // 0 = 播放三角，1 = 暂停双条（几何形变，可被打断）
static float g_textT = 0.0f;          // 当前阶段已经过的时间
static int   g_textPhase = 0;         // 0 静止 / 1 左滑淡出 / 2 右滑淡入
static int   g_textDir = 1;           // +1 下一首（往左走）/ -1 上一首（往右走）
static std::wstring g_outTitle, g_outArtist, g_outAlbum;   // 正在退场的旧文字
static std::wstring g_curTitle, g_curArtist, g_curAlbum;   // 上一帧的三行（用来抓快照）
static float g_setT  = 0.0f;
static float g_qualT = 0.0f;
static float g_volT  = 0.0f;
static float g_togT  = 0.0f;
static std::wstring g_qrPath;            // 本次登录用的二维码文件（每次换名，避免命中旧位图）
static bool         g_togOpen = false;      // 一起听面板
static bool         g_togInput = false;     // 输入框聚焦
static std::wstring g_togInputText;         // 输入中的连接码
static std::wstring g_togMyCode;            // 我的连接码
static bool         g_togBusy    = false;   // 正在开房/加入
static bool         g_togHosting = false;   // 已开房但还没人进来
static bool         g_togForcePush = false; // 下一次 PushLocalState 强制推（拖进度/加载变化）
static bool         g_togIsHost  = false;   // 我是房主（能控播放）
static int          g_togAddrIdx = -1;      // 选中第几个本机地址（-1 自动）
static std::wstring g_togPeer;              // 对方昵称
static bool         g_togLost = false;      // 断线提示
static std::wstring g_togMsg;               // 面板内提示
static ULONGLONG    g_togMsgAt = 0;
/* Toast 的淡入淡出：进 180ms、出 300ms，淡入时上移 6px
   返回透明度，dy 带回纵向偏移；返回 0 表示已经过期 */
static float g_toastOverride = -1.0f;   // 截图模式：固定 toast 透明度
static float ToastFade(ULONGLONG at, float life, float& dy) {
    dy = 0.0f;
    if (g_toastOverride >= 0.0f) { dy = 0.0f; return g_toastOverride; }
    if (!at) return 0.0f;
    float t = (float)(GetTickCount64() - at) / 1000.0f;
    if (t < 0.0f || t > life) return 0.0f;

    const float inT = 0.18f, outT = 0.30f;   // 注意：IN/OUT 是 SDK 宏，不能用作变量名
    float a = 1.0f;
    if (t < inT) {                      // 淡入 + 上移归位
        float k = t / inT;
        a = k;
        dy = (1.0f - k) * 6.0f;
    } else if (t > life - outT) {       // 淡出（用 ease-in：走得干脆）
        float k = (t - (life - outT)) / outT;
        a = 1.0f - k * k;
    }
    return a < 0.0f ? 0.0f : a;
}
static void TogToast(const std::wstring& s) { g_togMsg = s; g_togMsgAt = GetTickCount64(); }
static bool         g_loginChoose = false;   // 显示"选酷狗/网易云"的小面板
static bool         g_loginAsked  = false;   // 已经自动弹过一次，避免反复弹
static std::wstring g_settingsMsg;
static ULONGLONG    g_settingsMsgAt = 0;
static bool         g_loginRunning = false;
static std::string  g_loginUser0;
static ULONGLONG    g_waitTsv = 0;
static ULONGLONG    g_tsvBefore = 0;

/* ── 登录态：启动时问一次接口，token 过期能提前发现，不用等播放失败 ── */
static int          g_loginState = 0;    // 0 检查中 / 1 有效 / 2 失效或未登录
static std::wstring g_nickname;
static int          g_vipType = 0;


static void SettingsToast(const wchar_t* msg) {
    g_settingsMsg = msg;
    g_settingsMsgAt = GetTickCount64();
}

static ULONGLONG FileStamp(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

/** 递归删目录（删不掉就留着，下次启动再清） */
static void RemoveTree(const std::wstring& path) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring child = path + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(child);
            else DeleteFileW(child.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
}

/** 清掉本地缓存：封面图、音频、内存位图 */
static void ClearCache() {
    wchar_t full[MAX_PATH] = { 0 };
    std::wstring rel = AssetDir();
    if (!GetFullPathNameW(rel.c_str(), MAX_PATH, full, nullptr)) wcscpy(full, rel.c_str());
    std::wstring dir = full;

    // 1) 先放掉内存里的位图（否则 WIC 解码器还占着文件，删不掉）
    for (auto& b : g_coverBmps) {
        if (b.brush) b.brush->Release();
        if (b.bmp) b.bmp->Release();
    }
    g_coverBmps.clear();
    // 关键：WIC 格式转换器握着文件句柄，不释放的话 DeleteFile 会报 32（占用）
    for (auto& c : g_coverSrcs) if (c.conv) c.conv->Release();
    g_coverSrcs.clear();
    g_decRequested.clear();
    for (auto& s : g_app.songs) s.cover.clear();
    std::fill(g_covFlag.begin(), g_covFlag.end(), 0);

    // 2) 整目录改名再删——文件被占用时改名不受影响
    const wchar_t* subs[2] = { L"covers", L"audio" };
    for (const wchar_t* sub : subs) {
        std::wstring a = dir + L"\\" + sub;
        std::wstring b = dir + L"\\" + sub + L"_old";
        RemoveTree(b);                       // 清掉上次没删干净的
        if (MoveFileW(a.c_str(), b.c_str())) {
            CreateDirectoryW(a.c_str(), nullptr);
            RemoveTree(b);                   // 还删不掉就留到下次启动
        }
    }

    SettingsToast(T(L"缓存已清空", L"Cache cleared"));
}


/* ── 扫码登录 & 导出歌单（纯 C++，不再依赖 node） ── */
static std::mutex   g_lgMx;
static int          g_lgState = 0;      // 0 空闲 1 等扫码 2 已扫码 3 成功 4 失败
static std::string  g_lgToken, g_lgUserid;
static std::wstring g_lgMsg;
static int          g_expState = 0;     // 0 空闲 1 拉取中 2 完成 3 失败
static int          g_expCount = 0;
static std::wstring g_expMsg;

/** 写 kugou-session.json */
static std::wstring LocalDir();

/* ── 音源：0 酷狗 / 1 网易云 ── */
static int g_source = 0;
static std::wstring SourceFile() { return LocalDir() + L"\\source.txt"; }
static const wchar_t* SourceTag() { return g_source == 2 ? L"qq" : (g_source == 1 ? L"netease" : L"kugou"); }
static std::wstring LikesFile()   { return AssetDir() + L"\\" + SourceTag() + L"-likes.tsv"; }
static std::wstring SessionFile() { return AssetDir() + L"\\" + SourceTag() + L"-session.json"; }
static void SaveSource() {
    FILE* f = nullptr; _wfopen_s(&f, SourceFile().c_str(), L"wb");
    if (!f) return; char b[4]; sprintf(b, "%d", g_source);
    fwrite(b, 1, strlen(b), f); fclose(f);
}
static void LoadSource() {
    FILE* f = nullptr; _wfopen_s(&f, SourceFile().c_str(), L"rb");
    if (!f) return; char b[4] = {0}; fread(b, 1, 3, f); fclose(f);
    int v = atoi(b); if (v >= 0 && v <= 2) g_source = v;
}

static bool WriteSession(const std::string& uid, const std::string& tok) {
    std::wstring p = SessionFile();
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"wb");
    if (!f) return false;
    std::string s = (g_source != 0)
        ? ("{\n  \"cookie\": \"" + tok + "\"\n}\n")
        : ("{\n  \"token\": \"" + tok + "\",\n  \"userid\": " + uid + "\n}\n");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

/** 后台拉歌单写 tsv */
static void StartExport(const std::string& uid, const std::string& tok) {
    {
        std::lock_guard<std::mutex> lk(g_lgMx);
        g_expState = 1;
        g_expCount = 0;
        g_expMsg.clear();
    }
    std::wstring tsv = LikesFile();
    int src = g_source;
    std::thread([uid, tok, tsv, src]() {
        std::wstring err;
        int n = (src == 2) ? QqExportLikes(tok, tsv, err) : (src == 1 ? NgExportLikes(tok, tsv, err) : KgExportLikes(tok, uid, tsv, err));
        std::lock_guard<std::mutex> lk(g_lgMx);
        if (n > 0) { g_expState = 2; g_expCount = n; }
        else { g_expState = 3; g_expMsg = err; }
    }).detach();
}

/** 确保本机 sidecar 在跑（定义在后面） */
/* ── 内嵌 sidecar：首次运行解压到本地，之后直接复用 ── */
static std::wstring SidecarRoot() { return LocalDir() + L"\\sidecars"; }

/* sidecars 的版本号：内容一变就递增，触发重新解压 */
#define SIDECARS_VERSION L"2026-11-07-1"

static bool ExtractSidecars() {
    std::wstring root = SidecarRoot();
    CreateDirectoryW(root.c_str(), nullptr);
    std::wstring okFile = root + L"\\.ok";

    // 已有解压结果：版本对得上才复用，否则整个重来
    if (GetFileAttributesW(okFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        _wfopen_s(&f, okFile.c_str(), L"rb");
        std::string have;
        if (f) {
            char b[64] = { 0 };
            size_t n = fread(b, 1, 63, f);
            fclose(f);
            if (n) have.assign(b, n);
        }
        if (have == Narrow(SIDECARS_VERSION)) return true;
        Logf("APP", "sidecars 版本变更（%s），需重新解压", have.c_str());
    } else {
        Logf("APP", "首次解压 sidecars");
    }

    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SIDECARS), RT_RCDATA);
    if (!r) return false;
    DWORD sz = SizeofResource(nullptr, r);
    HGLOBAL g = LoadResource(nullptr, r);
    void* p = LockResource(g);
    if (!p || !sz) return false;

    std::wstring zp = root + L"\\sidecars.zip";
    FILE* f = nullptr;
    _wfopen_s(&f, zp.c_str(), L"wb");
    if (!f) return false;
    fwrite(p, 1, sz, f);
    fclose(f);

    // Windows 10+ 自带 bsdtar，可以直接解 zip
    std::wstring cmd = L"tar.exe -xf \"" + zp + L"\" -C \"" + root + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    bool done = false;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, root.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 180000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        done = (code == 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    DeleteFileW(zp.c_str());
    if (!done) return false;

    FILE* mk = nullptr;
    _wfopen_s(&mk, okFile.c_str(), L"wb");
    if (mk) { fputs(Narrow(SIDECARS_VERSION).c_str(), mk); fclose(mk); }
    Logf("APP", "sidecars 解压完成");
    return true;
}
static void EnsureSidecar();

/** 扫码登录：取二维码 → 轮询 → 成功把 token/userid 交给主线程应用 */
static void StartRelogin() {
    if (g_loginRunning) return;
    // 每次用新文件名：同路径会命中封面位图缓存，导致界面一直显示旧二维码
    g_qrPath = AssetDir() + L"\\qr-" + std::to_wstring((unsigned long long)GetTickCount64()) + L".png";
    std::wstring qr = g_qrPath;
    if (g_source == 2) {                       // QQ 音乐扫码
        DeleteFileW(qr.c_str());
        g_loginRunning = true;
        { std::lock_guard<std::mutex> lk(g_lgMx); g_lgState = 1; g_lgMsg.clear(); }
        SettingsToast(T(L"用 QQ 音乐 App 扫码", L"Scan with the QQ Music app"));
        std::thread([qr]() {
            EnsureSidecar();
            std::wstring err;
            if (!QqLoginStart(qr, err)) {
                std::lock_guard<std::mutex> lk(g_lgMx);
                g_lgState = 4; g_lgMsg = err; return;
            }
            for (int i = 0; i < 150; i++) {
                std::string ck; std::wstring e;
                int st = QqLoginPoll(ck, e);
                if (st == 1) { std::lock_guard<std::mutex> lk(g_lgMx); g_lgState = 2; }
                if (st == 2) {
                    std::lock_guard<std::mutex> lk(g_lgMx);
                    g_lgToken = ck; g_lgUserid = "1";
                    g_lgState = 3; return;
                }
                if (st == 3 || st == 4) {
                    std::lock_guard<std::mutex> lk(g_lgMx);
                    g_lgState = 4; g_lgMsg = e; return;
                }
                { std::lock_guard<std::mutex> lk(g_lgMx); if (g_lgState == 0) return; }
                Sleep(2000);
            }
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_lgState = 4; g_lgMsg = T(L"二维码超时", L"QR code expired");
        }).detach();
        return;
    }
    if (g_source == 1) {                       // 网易云扫码
        DeleteFileW(qr.c_str());
        g_loginRunning = true;
        { std::lock_guard<std::mutex> lk(g_lgMx); g_lgState = 1; g_lgMsg.clear(); }
        SettingsToast(T(L"用网易云音乐 App 扫码", L"Scan with the NetEase Music app"));
        std::thread([qr]() {
            EnsureSidecar();          // 切平台后本地服务可能没起
            std::wstring err;
            if (!NgLoginStart(qr, err)) {
                std::lock_guard<std::mutex> lk(g_lgMx);
                g_lgState = 4; g_lgMsg = err; return;
            }
            for (int i = 0; i < 150; i++) {
                std::string ck; std::wstring e;
                int st = NgLoginPoll(ck, e);
                if (st == 2) {
                    std::lock_guard<std::mutex> lk(g_lgMx);
                    g_lgToken = ck; g_lgUserid = "1";   // 网易云用 cookie，userid 占位
                    g_lgState = 3; return;
                }
                if (st == 3 || st == 4) {
                    std::lock_guard<std::mutex> lk(g_lgMx);
                    g_lgState = 4; g_lgMsg = e; return;
                }
                { std::lock_guard<std::mutex> lk(g_lgMx); if (g_lgState == 0) return; }
                Sleep(2000);
            }
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_lgState = 4; g_lgMsg = T(L"二维码超时", L"QR code expired");
        }).detach();
        return;
    }
    DeleteFileW(qr.c_str());
    g_loginRunning = true;
    {
        std::lock_guard<std::mutex> lk(g_lgMx);
        g_lgState = 1;
        g_lgMsg.clear();
    }
    SettingsToast(T(L"用酷狗 App 扫描二维码", L"Scan with the Kugou app"));

    std::thread([qr]() {
        EnsureSidecar();
        std::wstring err;
        if (!KgLoginStart(qr, err)) {
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_lgState = 4;
            g_lgMsg = err;
            return;
        }
        for (int i = 0; i < 150; i++) {
            std::string tok, uid;
            std::wstring e;
            int st = KgLoginPoll(tok, uid, e);
            if (st == 2) {
                std::lock_guard<std::mutex> lk(g_lgMx);
                g_lgToken = tok;
                g_lgUserid = uid;
                g_lgState = 3;
                return;
            }
            if (st == 3 || st == 4) {
                std::lock_guard<std::mutex> lk(g_lgMx);
                g_lgState = 4;
                g_lgMsg = e;
                return;
            }
            {   // 用户中途点了「关闭」就别继续轮询了
                std::lock_guard<std::mutex> lk(g_lgMx);
                if (g_lgState == 0) return;
            }
            Sleep(2000);
        }
        std::lock_guard<std::mutex> lk(g_lgMx);
        g_lgState = 4;
        g_lgMsg = T(L"二维码超时", L"QR code expired");
    }).detach();
}

static void StartRelogin_old_unused();

/* 设置卡片的位置：绘制和"模态点击区"共用 */
static D2D1_RECT_F SettingsCardRect(float W, float H) {
    const float cw = 560.0f, ch = 352.0f;
    return Rc((W - cw) * 0.5f, (H - ch) * 0.5f, cw, ch);
}

static void DrawSettings(float W, float H) {

    const D2D1_RECT_F card = SettingsCardRect(W, H);
    const float cw = card.right - card.left;
    const float ch = card.bottom - card.top;
    FillRound(Rc(card.left + 4, card.top + 12, cw, ch), 18.0f, UI::SHADOW, 0.16f);
    FillRound(card, 18.0f, UI::PANEL);


    Text(T(L"设置", L"Settings"), Fmt(17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
         Rc(card.left + 24, card.top + 18, 200, 28), UI::TEXT);


    // 当前账号状态：启动时查过接口才有确定结论
    {
        std::wstring who;
        UINT32 col = UI::TEXT2;
        if (g_loginState == 1) {
            who = std::wstring(T(L"已登录：", L"Signed in: ")) + g_nickname;
            if (g_vipType > 0) who += L" · VIP" + std::to_wstring(g_vipType);
            col = UI::ACCENT_TEXT;
        } else if (g_loginState == 2) {
            who = g_userid.empty() ? T(L"未登录", L"Not signed in") : T(L"登录已过期，请重新登录", L"Session expired, sign in again");
            col = 0xB04A63;
        } else if (g_loginState == 3) {
            who = T(L"接口服务未启动", L"Service not running");
            col = 0x8A6A2F;
        } else {
            who = T(L"正在检查登录状态…", L"Checking sign-in status…");
        }
        Text(who, Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(card.left + 24, card.top + 44, 400, 18), col);
    }

    D2D1_RECT_F x = Rc(card.right - 48, card.top + 16, 30, 30);
    if (Inside(x, g_app.mouse)) FillRound(x, 8.0f, UI::HOVER);
    Line(x.left + 10, x.top + 10, x.right - 10, x.bottom - 10, UI::TEXT2, 1.6f);
    Line(x.right - 10, x.top + 10, x.left + 10, x.bottom - 10, UI::TEXT2, 1.6f);
    Hit hx; hx.r = x; hx.kind = HK_SET_CLOSE;
    g_app.hits.push_back(hx);

    const wchar_t* names[3] = { T(L"清空缓存", L"Clear cache"), T(L"重新登录", L"Sign in again"), T(L"切换主题", L"Change theme") };
    const int kinds[3] = { HK_SET_CLEAR, HK_SET_LOGIN, HK_SET_THEME };
    const int btnCount = (ThemeCount() > 1) ? 3 : 2;     // 只有一套配色时不显示切换
    for (int i = 0; i < btnCount && !g_loginChoose; i++) {   // 选平台时把按钮收起来
        D2D1_RECT_F b = Rc(card.left + 24, card.top + 74 + i * 58, 250, 44);
        bool hot = Inside(b, g_app.mouse);
        FillRound(b, 10.0f, hot ? UI::SELECT : UI::BG);
        if (hot) StrokeRound(b, 10.0f, UI::BTN_CLOSE, 1.0f);
        Text(names[i], Fmt(13.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(b.left + 18, b.top, 220, 44), UI::TEXT);
        Hit h; h.r = b; h.kind = kinds[i];
        g_app.hits.push_back(h);
    }

    // ── 语言 ──
    {
        float ly = card.top + 74.0f + (float)btnCount * 58.0f;
        Text(T(L"界面语言", L"Language"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(card.left + 24, ly, 120, 30), UI::TEXT2);

        const wchar_t* langs[2] = { L"中文", L"English" };
        const int kinds[2] = { HK_SET_LANG_ZH, HK_SET_LANG_EN };
        for (int i = 0; i < 2; i++) {
            D2D1_RECT_F b = Rc(card.left + 132 + i * 104, ly - 5, 96, 34);
            bool on = (g_lang == Lang::Zh) == (i == 0);
            bool hot = Inside(b, g_app.mouse);
            FillRound(b, 9.0f, on ? UI::SELECT : (hot ? UI::HOVER : UI::BG));
            if (on) StrokeRound(b, 9.0f, UI::BTN_CLOSE, 1.0f);
            Text(langs[i], Fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL), b,
                 on ? UI::TEXT : UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
            Hit h; h.r = b; h.kind = kinds[i];
            g_app.hits.push_back(h);
        }
    }

    if (!g_settingsMsg.empty()) {
        float dy = 0.0f;
        float ta = ToastFade(g_settingsMsgAt, 5.0f, dy);
        if (ta > 0.01f) {
            Text(g_settingsMsg, Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(card.left + 24, card.bottom - 42 + dy, 260, 20), UI::TEXT2,
                 DWRITE_TEXT_ALIGNMENT_LEADING, ta);
        }
    }

    // 先选平台，再出二维码
    if (g_loginChoose && !g_loginRunning) {
        Text(T(L"选择要登录的平台", L"Choose a platform"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(card.right - 300, card.top + 74, 270, 20), UI::TEXT2);
        const wchar_t* pf[3] = { T(L"酷狗音乐", L"Kugou Music"), T(L"网易云音乐", L"NetEase Music"), L"QQ Music" };
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F b = Rc(card.right - 300, card.top + 96 + i * 46, 270, 38);
            bool hot = Inside(b, g_app.mouse);
            FillRound(b, 10.0f, hot ? UI::SELECT : UI::BG);
            Text(pf[i], Fmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(b.left + 16, b.top, 220, 40), UI::TEXT);
            Hit h; h.r = b; h.kind = HK_LOGIN_PICK; h.index = i;
            g_app.hits.push_back(h);
        }
    }

    // 登录中：右边显示二维码
    std::wstring qr = g_qrPath;
    if (g_loginRunning && GetFileAttributesW(qr.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CoverBmp* e = CoverEntry(qr)) {
            D2D1_RECT_F q = Rc(card.right - 24 - 150, card.top + 74, 150, 150);
            FillRound(Rc(q.left - 6, q.top - 6, 162, 162), 10.0f, 0xFFFFFF);
            DrawBitmapRounded(e, q, 8.0f);

            // 轮询状态：让人看得出程序在等
            int st = 0;
            { std::lock_guard<std::mutex> lk(g_lgMx); st = g_lgState; }
            const wchar_t* s = (st == 2) ? T(L"已扫码，请手机上确认", L"Scanned, confirm on phone")
                                         : T(L"等待扫码…", L"Waiting for scan…");
            Text(s, Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(q.left, q.bottom + 8, 150, 18),
                 st == 2 ? UI::SHADOW : UI::TEXT3, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    } else if (g_loginRunning) {
        Text(T(L"正在生成二维码…", L"Generating QR code…"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(card.right - 214, card.top + 136, 190, 24), UI::TEXT3,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void DrawWindowButtons(float W) {
    for (int i = 0; i < UI::BTN_COUNT; i++) {
        D2D1_RECT_F b = BtnRect(i, W);
        bool hot = Inside(b, g_app.mouse);

        D2D1_RECT_F hov = Rc(b.left + 4, b.top + 2, (b.right - b.left) - 8, (b.bottom - b.top) - 4);
        if (hot) FillRound(hov, UI::R_BTN, i == WB_CLOSE ? UI::BTN_CLOSE : UI::BTN_HOVER);

        UINT32 fg = hot ? (i == WB_CLOSE ? 0xFFFFFF : UI::TEXT) : UI::TEXT2;
        float cx = (b.left + b.right) * 0.5f;
        float cy = (b.top + b.bottom) * 0.5f;

        if (i == WB_MIN)      HLine(cx - 6, cx + 6, cy, fg);
        else if (i == WB_MAX) StrokeRect(Rc(cx - 5.5f, cy - 5.5f, 11, 11), fg);
        else { Line(cx - 5, cy - 5, cx + 5, cy + 5, fg); Line(cx + 5, cy - 5, cx - 5, cy + 5, fg); }

        Hit h; h.r = b; h.kind = HK_MIN + i; h.index = -1;
        g_app.hits.push_back(h);
    }

    // 设置按钮：三个窗口按钮左边
    D2D1_RECT_F g = Rc(BtnRect(0, W).left - 40.0f, BtnRect(0, W).top + 2.0f, 32.0f,
                       (BtnRect(0, W).bottom - BtnRect(0, W).top) - 4.0f);
    bool hot = Inside(g, g_app.mouse);
    if (hot || g_settingsOpen) FillRound(g, UI::R_BTN, UI::HOVER);

    {
        float cx = (g.left + g.right) * 0.5f;
        float cy = (g.top + g.bottom) * 0.5f;
        UINT32 fg = (hot || g_settingsOpen) ? UI::TEXT : UI::TEXT2;
        // 三根滑杆，当设置图标
        for (int k = 0; k < 3; k++) {
            float y = cy - 6.0f + k * 6.0f;
            Line(cx - 8.0f, y, cx + 8.0f, y, fg, 1.4f);
            float knob = cx + (k == 1 ? 5.0f : (k == 0 ? -4.0f : 2.0f));
            FillRect(Rc(knob - 2.0f, y - 2.0f, 4.0f, 4.0f), fg);
        }
    }

    Hit hg; hg.r = g; hg.kind = HK_SETTINGS;
    g_app.hits.push_back(hg);
}

/** 登录提示条的位置：绘制和 WM_NCHITTEST 共用 */
static D2D1_RECT_F LoginPillRect(float W) {
    float rightEdge = W - (UI::BTN_W * UI::BTN_COUNT + 76.0f);
    float tw = 168.0f;                   // 按最长的"接口服务未响应 · 点此查看"留
    return Rc(rightEdge - tw - 12.0f, UI::TITLE_H * 0.5f - 11.0f, tw, 22.0f);
}

static void DrawTitleBar(float W) {


    // 顶栏底色：和页面同色，不透明。进场/退场时这层跟着裁剪一起「扫」进来
    FillRect(Rc(0, 0, W, UI::TITLE_H), UI::BG, 1.0f);

    Text(L"Floral", Fmt(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
         Rc(UI::PAD, 0, 240, UI::TITLE_H), UI::TEXT);
    {   // 点左上角回歌单选择页
        Hit hb; hb.r = Rc(UI::PAD - 6, 0, 90, UI::TITLE_H); hb.kind = HK_PL_PICK;
        g_app.hits.push_back(hb);
    }

    float rightEdge = W - (UI::BTN_W * UI::BTN_COUNT + 76.0f);

    // 回歌单选择页：2×2 小方块，放在唱片按钮左边
    {
        float bx2 = BtnRect(0, W).left - 40.0f - 44.0f - 44.0f;
        D2D1_RECT_F pb2 = Rc(bx2, 13.0f, 32.0f, 26.0f);
        bool hv2 = Inside(pb2, g_app.mouse);
        bool onPicker = (g_app.page == Page::Playlists);
        if (hv2 && !onPicker) FillRound(pb2, 8.0f, UI::HOVER);
        float gx = (pb2.left + pb2.right) * 0.5f, gy = (pb2.top + pb2.bottom) * 0.5f;
        UINT32 ic2 = onPicker ? UI::TEXT3 : UI::TEXT2;
        const float sq = 5.0f, gap2 = 1.5f;
        for (int r2 = 0; r2 < 2; r2++) {
            for (int c2 = 0; c2 < 2; c2++) {
                float sx = gx - sq - gap2 * 0.5f + (float)c2 * (sq + gap2);
                float sy = gy - sq - gap2 * 0.5f + (float)r2 * (sq + gap2);
                FillRound(Rc(sx, sy, sq, sq), 1.5f, ic2);
            }
        }
        if (!onPicker) {
            Hit hp3; hp3.r = pb2; hp3.kind = HK_PICKER;
            g_app.hits.push_back(hp3);
        }
        if (!onPicker) rightEdge = pb2.left - 12.0f;   // 让歌曲数别压到按钮上
    }

    // 进入播放页：小唱片图标，放在设置左边
    {
        float bx = BtnRect(0, W).left - 40.0f - 44.0f;
        D2D1_RECT_F pb = Rc(bx, 13.0f, 32.0f, 26.0f);
        bool hv = Inside(pb, g_app.mouse);
        bool onPlayer = (g_app.page == Page::Player);
        if (hv || onPlayer) FillRound(pb, 8.0f, onPlayer ? UI::SELECT : UI::HOVER);
        float cx = (pb.left + pb.right) * 0.5f, cy = (pb.top + pb.bottom) * 0.5f;
        UINT32 ic = onPlayer ? UI::ACCENT_TEXT : UI::TEXT2;
        StrokeRound(Rc(cx - 7.5f, cy - 7.5f, 15, 15), 7.5f, ic, 1.6f);
        FillRound(Rc(cx - 2.2f, cy - 2.2f, 4.4f, 4.4f), 2.2f, ic);
        Hit hp2; hp2.r = pb; hp2.kind = HK_PLAYER;
        g_app.hits.push_back(hp2);
        rightEdge = (std::min)(rightEdge, pb.left - 12.0f);
    }

    // 登录失效 / 接口没起来时给个能点的提示条，别让人等到播放失败才发现
    if (g_loginState == 2 || g_loginState == 3) {
        const wchar_t* tip;
        UINT32 fg;
        if (g_loginState == 3) {
            tip = T(L"接口服务未启动 · 点此查看", L"Service not running · click to check");
            fg = 0x8A6A2F;
        } else if (g_userid.empty() && g_token.empty()) {
            tip = T(L"未登录 · 点此登录", L"Not signed in · click to sign in");
            fg = 0xB04A63;
        } else {
            tip = T(L"登录已过期 · 点此重登", L"Session expired · click to sign in");
            fg = 0xB04A63;
        }
        D2D1_RECT_F pill = LoginPillRect(W);
        bool ph = Inside(pill, g_app.mouse);
        FillRound(pill, 11.0f, ph ? 0xF6CBD8 : (g_loginState == 3 ? 0xF7EBD2 : 0xFBE0E8));
        Text(tip, Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL), pill, fg,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit h; h.r = pill; h.kind = HK_LOGIN_TIP;
        g_app.hits.push_back(h);
        rightEdge = pill.left - 8.0f;
    }

    if (g_app.page == Page::Grid) {
        wchar_t cnt[64];
        swprintf(cnt, 64, T(L"共 %d 首", L"%d songs"), (int)g_app.songs.size());
        Text(cnt, Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(rightEdge - 200, 0, 200, UI::TITLE_H), UI::TEXT2,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    DrawWindowButtons(W);
}

/* ══════════════════════════════════════════
   网格页
   ══════════════════════════════════════════ */
static void DrawTile(const D2D1_RECT_F& t, const Song& s, int idx, float anim) {
    const float w = t.right - t.left;
    const float h = t.bottom - t.top;

    // 阴影
    if (anim > 0.02f) {
        D2D1_RECT_F sh = Rc(t.left + 4, t.top + 14, w - 8, h - 10);
        FillRound(sh, UI::R_TILE, UI::SHADOW, 0.18f * anim);
    }

    DrawCoverArt(t, s, idx, UI::R_TILE);

    if (anim > 0.01f) {
        // 底部压暗：圆角渐变直接填充，顶部 alpha 为 0，看不出圆角
        float sh = h * 0.46f;
        FillRoundGradV(Rc(t.left, t.bottom - sh, w, sh), UI::R_TILE,
                       0x000000, 0.0f, 0x000000, 0.66f * anim);

        Text(s.title, Fmt(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             Rc(t.left + 16, t.bottom - 58, w - 64, 22), 0xFFFFFF,
             DWRITE_TEXT_ALIGNMENT_LEADING, anim);
        Text(s.artist, Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(t.left + 16, t.bottom - 34, w - 64, 18), 0xFFFFFF,
             DWRITE_TEXT_ALIGNMENT_LEADING, anim * 0.8f);

        // 播放圆标（三角）
        float r = 17.0f;
        D2D1_RECT_F chip = Rc(t.right - 16 - r * 2, t.bottom - 16 - r * 2, r * 2, r * 2);
        FillRound(chip, r, 0xFFFFFF, 0.92f * anim);
        float px = chip.left + r, py = chip.top + r;
        Line(px - 3.5f, py - 7, px - 3.5f, py + 7, UI::TEXT, 1.8f, anim);
        Line(px - 3.5f, py - 7, px + 7, py, UI::TEXT, 1.8f, anim);
        Line(px + 7, py, px - 3.5f, py + 7, UI::TEXT, 1.8f, anim);
    }
}

static void DrawGrid() {
    App& a = g_app;
    const float W = a.width, H = a.height;

    int n = (int)a.songs.size();
    if (n == 0) {
        Text(g_source ? T(L"网易云歌单拉取中", L"Fetching playlist…") : T(L"还没有歌曲", L"No songs yet"),
             Fmt(14.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(0, H * 0.45f, W, 24), UI::TEXT3, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    if (a.page == Page::Grid && a.pageAnim < 0.02f) {
        // 滚动中、以及滚动后鼠标没再动过，都不放大
        bool gliding = std::fabs(a.scrollTarget - a.scroll) > 0.5f;
        a.hoverTile = (gliding || !g_hoverArmed || g_settingsOpen) ? -1 : HitTile(a.mouse);
    }
    int hov = (a.page == Page::Grid && a.pageAnim < 0.02f) ? a.hoverTile : -1;
    EnsureTarget(W, n, hov);         // 锚定列 + 局部重排：既挤开又不瞬移

    float viewH = (std::max)(0.0f, H - GRID_TOP - UI::PAD);
    a.maxScroll = (std::max)(0.0f, g_gridHeight - viewH);
    if (a.scrollTarget > a.maxScroll) a.scrollTarget = a.maxScroll;
    if (a.scrollTarget < 0) a.scrollTarget = 0;
    if (a.scroll > a.maxScroll) a.scroll = a.maxScroll;
    if (a.scroll < 0) a.scroll = 0;

    // ── 滚动条（右侧，可拖） ──
    if (a.maxScroll > 1.0f) {
        const float sbW = 6.0f;
        const float top = GRID_TOP;
        const float bottom = H - UI::PAD;
        const float trackH = bottom - top;
        if (trackH > 40.0f) {
            const float trackX = W - sbW - 6.0f;
            float thumbH = (std::max)(36.0f, trackH * viewH / (viewH + a.maxScroll));
            if (thumbH > trackH) thumbH = trackH;
            float t = (a.maxScroll > 0.0f) ? (a.scroll / a.maxScroll) : 0.0f;
            if (t < 0) t = 0; if (t > 1) t = 1;
            float thumbY = top + (trackH - thumbH) * t;

            FillRound(Rc(trackX, top, sbW, trackH), sbW * 0.5f, UI::TEXT3, 0.16f);
            D2D1_RECT_F thumb = Rc(trackX, thumbY, sbW, thumbH);
            bool hot = Inside(Rc(trackX - 8, top, sbW + 16, trackH), a.mouse) || g_scrollDrag;
            FillRound(thumb, sbW * 0.5f, hot ? UI::ACCENT : UI::TEXT3, hot ? 1.0f : 0.55f);

            Hit hs; hs.r = Rc(trackX - 8, top, sbW + 16, trackH); hs.kind = HK_SCROLL;
            if (g_alpha > 0.5f) a.hits.push_back(hs);
        }
    }

    // 只收集视口附近（上下各多留一条）的块
    {
        float vy0 = a.scroll - 150.0f;
        float vy1 = a.scroll + H + 150.0f;
        int b0 = (int)((vy0 - GRID_TOP) / BAND_H);
        int b1 = (int)((vy1 - GRID_TOP) / BAND_H);
        if (b0 < 0) b0 = 0;
        if (b1 >= (int)g_tgtBands.size()) b1 = (int)g_tgtBands.size() - 1;

        g_vis.clear();
        ++g_stampVal;
        if (g_stamp.size() != g_base.size()) g_stamp.assign(g_base.size(), 0);
        for (int k = b0; k <= b1; k++) {
            for (int idx : g_tgtBands[(size_t)k]) {
                if (idx < 0 || idx >= (int)g_stamp.size()) continue;
                if (g_stamp[(size_t)idx] == g_stampVal) continue;
                g_stamp[(size_t)idx] = g_stampVal;
                g_vis.push_back(idx);
            }
        }
    }

    // 命中区用"实际画出来的"布局（含悬停放大与挤开后的位置），点哪算哪
    // 进场飞入没落地前先不给点击，免得点空
    if (g_alpha > 0.5f && g_gridEnter > 0.85f) {
        for (int i : g_vis) {
            const TileBox& b = g_target[(size_t)i];
            Hit h;
            h.r = Rc(b.x, GRID_TOP + b.y - a.scroll, b.w, b.h);
            h.kind = HK_TILE;
            h.index = i;
            a.hits.push_back(h);
        }
    }

    // 视口里缺封面的歌，按需下载
    RequestVisibleCovers();

    g_rt->PushAxisAlignedClip(D2D1::RectF(0, GRID_TOP - 10, W, H), D2D1_ANTIALIAS_MODE_ALIASED);

    // 运动模糊：剩余距离和瞬时速度取较大者，保证滑行期间一定有拖影
    float glide = std::fabs(a.scrollTarget - a.scroll);
    float bDist = (std::min)(1.0f, glide / 80.0f);
    float bVel = (std::min)(1.0f, g_scrollVel / 1400.0f);
    float blur = (std::max)(bDist, bVel);
    float blurDir = (a.scrollTarget >= a.scroll) ? 1.0f : -1.0f;

    const bool blurOn = blur > 0.04f;
    const float step = 3.0f + 9.0f * blur;          // 最快时拖影约 60px
    const float ghostA[5] = { 0.30f, 0.24f, 0.18f, 0.12f, 0.07f };

    // 进场进度：0→1.45 就是"逐个从右侧飞入"的行程
    const bool entering = (g_gridEnter < 0.999f);
    const float entT = g_gridEnter * 1.45f;

    // 先画普通块，再画悬停块（保证它盖在邻居上面）
    int order = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i : g_vis) {
            bool isHover = (i == hov);
            if ((pass == 0) == isHover) continue;
            const TileBox& d = g_draw[(size_t)i];
            float ty = GRID_TOP + d.y - a.scroll;

            float enterA = 1.0f, enterDx = 0.0f;
            if (entering) {
                const int ord = (order < 18) ? order : 18;       // 错开最多 18 档，尾部不滞后
                order++;
                float p = (entT - (float)ord * 0.05f) / 0.42f;   // 错开 50ms，单块 420ms
                if (p <= 0.0f) continue;
                if (p > 1.0f) p = 1.0f;
                float e = 1.0f - powf(1.0f - p, 3.0f);
                enterA = e;
                enterDx = (1.0f - e) * (W + 60.0f - d.x);        // 从窗口右侧飞进来
            }

            if (blurOn && !isHover && !entering) {
                for (int k = 5; k >= 1; k--) {
                    g_alphaMul = ghostA[k - 1] * blur;
                    DrawTile(Rc(d.x, ty + blurDir * k * step, d.w, d.h), a.songs[i], i, 0.0f);
                }
                g_alphaMul = 1.0f;
            }

            float keep = g_alphaMul;
            g_alphaMul = keep * enterA;
            DrawTile(Rc(d.x + enterDx, ty, d.w, d.h), a.songs[i], i, isHover ? a.tileAnim[i] : 0.0f);
            g_alphaMul = keep;
        }
    }

    g_alphaMul = 1.0f;
    g_rt->PopAxisAlignedClip();
}

/* ══════════════════════════════════════════
   播放页：左侧转动的 CD + 上下专辑盒子，右侧歌曲信息
   ══════════════════════════════════════════ */
static int   g_switchFrom = -1, g_switchTo = -1, g_switchDir = 1;
static float g_switchT = 1.0f;          // 0..1，1 表示不在切歌
static float g_discAngle = 0.0f;        // 唱片转动角度（度）
static float g_lineT = 0.0f;            // 收起线的伸出进度 0..1
static int   g_casePrev0 = -1, g_caseNext0 = -1;   // 切歌瞬间，盒子里原来放的是哪两首

/* 退播放页的转场：整屏淡出 → 网格像开屏那样从右边飞入 */
static bool  g_exitOn = false;
static float g_exitT = 1.0f;

/* 进播放页的转场：0..1 走完全程 */
static bool  g_enterOn = false;
static float g_enterT = 1.0f;
static int   g_enterIdx = -1;
static D2D1_RECT_F g_enterFrom{};       // 点击时那格在屏幕上的位置

static D2D1_RECT_F LerpRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float t) {
    return D2D1::RectF(a.left + (b.left - a.left) * t,
                       a.top + (b.top - a.top) * t,
                       a.right + (b.right - a.right) * t,
                       a.bottom + (b.bottom - a.bottom) * t);
}

static void FillCircle(float cx, float cy, float r, UINT32 c, float a = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_brush);
}
static void StrokeCircle(float cx, float cy, float r, UINT32 c, float a = 1.0f, float w = 1.0f) {
    if (!g_brush) return;
    g_brush->SetColor(Col(c, a));
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_brush, w);
}

/** 画一张 CD：黑胶底 + 转动的封面 + 纹路 + 中心孔 */
static void DrawDisc(const D2D1_RECT_F& r, const Song& s, int idx, float angleDeg, float alpha) {
    if (alpha <= 0.01f) return;
    const float rad = (r.right - r.left) * 0.5f;
    if (rad < 4.0f) return;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float artR = rad * 0.66f;

    // 黑胶底 + 外圈
    FillCircle(cx, cy, rad, UI::DISC, alpha);

    // 转动的封面（椭圆填充 + 位图画刷，不用 Layer）
    // 淡入期间先铺占位渐变，再把真图压上去
    CoverBmp* ce = CoverEntry(s.cover);
    float coverFade = ce ? CoverFade(s.cover) : 0.0f;
    bool haveBrush = false;

    if (ce) {
        if (!ce->brush) {
            D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP);
            g_rt->CreateBitmapBrush(ce->bmp, &bp, nullptr, &ce->brush);
        }
        D2D1_SIZE_F sz = ce->bmp ? ce->bmp->GetSize() : D2D1::SizeF(0, 0);
        haveBrush = (ce->brush != nullptr) && sz.width > 0 && sz.height > 0;
    }

    if (!haveBrush || coverFade < 1.0f) {          // 占位（也作为淡入的底）
        int ci = ((idx % 8) + 8) % 8;
        if (g_covBrush[ci]) {
            g_covBrush[ci]->SetStartPoint(D2D1::Point2F(cx, cy - artR));
            g_covBrush[ci]->SetEndPoint(D2D1::Point2F(cx, cy + artR));
            g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), artR, artR), g_covBrush[ci]);
        }
    }
    if (haveBrush) {
        D2D1_SIZE_F sz = ce->bmp->GetSize();
        float sc = (2.0f * artR) / (std::min)(sz.width, sz.height);
        float ox = cx - sz.width * sc * 0.5f;
        float oy = cy - sz.height * sc * 0.5f;
        ce->brush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) *
                                D2D1::Matrix3x2F::Translation(ox, oy) *
                                D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(cx, cy)));
        ce->brush->SetOpacity(g_alpha * g_alphaMul * alpha * coverFade * coverFade);
        g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), artR, artR), ce->brush);
    }

    // 黑胶纹路
    for (int i = 1; i <= 3; i++) {
        float rr = artR + (rad - artR) * (i / 4.0f);
        StrokeCircle(cx, cy, rr, UI::DISC_GROOVE, 0.55f * alpha, 1.0f);
    }
    // 固定高光，增强"圆盘"感
    if (g_brush) {
        for (int k = 0; k < 2; k++) {
            float a0 = -1.9f + k * 0.26f;
            StrokeCircle(cx, cy, rad * (0.90f - k * 0.05f), UI::DISC_GROOVE, 0.35f * alpha, 1.4f);
        }
    }
    // 中心孔
    FillCircle(cx, cy, rad * 0.05f, UI::BG, alpha);
    StrokeCircle(cx, cy, rad * 0.05f, UI::DISC_GROOVE, 0.6f * alpha, 1.0f);
    // 外边缘
    StrokeCircle(cx, cy, rad - 0.5f, 0x000000, 0.18f * alpha, 1.0f);
}

/** 小专辑盒子里的封面 */
static void DrawBoxCover(float cx, float cy, float size, int idx, float alpha) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size() || alpha <= 0.01f) return;

    g_alphaMul = alpha;
    FillRound(Rc(cx - size * 0.5f - 3, cy - size * 0.5f - 3, size + 6, size + 6),
              10.0f, 0xFFFFFF, 0.55f);
    DrawCoverArt(Rc(cx - size * 0.5f, cy - size * 0.5f, size, size),
                 a.songs[idx], idx, 7.0f);
    g_alphaMul = 1.0f;
}

/** CD 专辑盒（照实物）：透明塑料外壳 → 深灰托盘 → 内页封面 */
static void DrawCase(const D2D1_RECT_F& r, int idx, float alpha) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size() || alpha <= 0.01f) return;

    g_alphaMul = alpha;

    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const float rad  = (std::max)(3.0f, w * 0.012f);     // 硬边盒子
    const float rim  = (std::max)(4.0f, w * 0.026f);     // 透明塑料边
    const float tray = (std::max)(5.0f, w * 0.030f);     // 深色托盘

    // 1) 透明塑料外壳
    FillRoundGrad(r, rad, 0xFDFBFC, 0xDED3D8);

    // 2) 深灰托盘
    D2D1_RECT_F tr = Rc(r.left + rim, r.top + rim, w - rim * 2.0f, h - rim * 2.0f);
    FillRoundGrad(tr, rad, 0x4C464A, 0x312B2F);

    // 3) 内页封面
    D2D1_RECT_F inner = Rc(tr.left + tray, tr.top + tray,
                           w - (rim + tray) * 2.0f, h - (rim + tray) * 2.0f);
    DrawCoverArt(inner, a.songs[idx], idx, rad);

    // 4) 内页上沿反光
    FillRound(Rc(inner.left, inner.top, inner.right - inner.left,
                 (std::max)(6.0f, h * 0.04f)), rad, 0xFFFFFF, 0.14f);

    // 5) 塑料边高光 + 外轮廓
    StrokeRound(Rc(r.left + 1, r.top + 1, w - 2, h - 2), rad, 0xFFFFFF, 0.55f, 1.0f);
    StrokeRound(r, rad, 0x000000, 0.18f, 1.0f);

    g_alphaMul = 1.0f;
}

/** 唱片的封套：和 CD 一样大，只露出顶部/底部一条边；可整体倾斜 */
static void DrawSleeve(const D2D1_RECT_F& full, const D2D1_RECT_F& strip,
                       int idx, float alpha, float angleDeg) {
    if (idx < 0 || idx >= (int)g_app.songs.size() || alpha <= 0.01f) return;

    D2D1::Matrix3x2F saved;
    g_rt->GetTransform(&saved);

    // 先压裁剪区（此时还是单位矩阵，保证是水平的），再旋转绘制
    g_rt->PushAxisAlignedClip(strip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float pcx = (full.left + full.right) * 0.5f;
    float pcy = (full.top + full.bottom) * 0.5f;
    g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(pcx, pcy)));

    DrawCase(full, idx, alpha);

    g_rt->SetTransform(saved);
    g_rt->PopAxisAlignedClip();
}

/* ── 音频 ── */
static bool g_audioOk = false;      // 当前这首有没有真的在放

/* ── 播放加载动画 ── */
static bool      g_audioLoading = false;
static bool      g_audioError = false;
static int       g_audioErrKind = 0;      // 1 解析失败 / 2 打不开（解不了）
static ULONGLONG g_loadingSince = 0;
static ULONGLONG g_loadingMin = 0;        // 至少显示到这一刻，太快会看不见
static int       g_audioRetry = 0;        // 打开成功却不出声时，允许自动重试一次
static float     g_spinAngle = 0.0f;      // 弧度


/* 播放地址解析（后台线程做 HTTP，主线程只负责开 MCI） */
static std::mutex    g_urlMx;
static std::wstring  g_urlReady;
static int           g_urlState = 0;   // 0 空闲 1 解析中 2 好了 3 失败

/* 开音频搬到后台线程：预缓冲要等网络，卡在 UI 线程会有明显顿感 */
static std::mutex       g_openMx;
static std::atomic<int> g_openGen{ 0 };     // 每次请求 +1，过时的线程结果直接丢
static std::atomic<int> g_openResult{ 0 };  // 0 无 / 1 成功 / -1 失败
static std::wstring     g_openUrl;          // 最近一次尝试的地址（UI 线程读）

/* 关音频也要下后台：停设备 + join 下载线程实测 16~31ms，卡在 UI 上就是掉一帧 */
static void BeginCloseAsync() {
    ++g_openGen;            // 让在途的 open 作废
    g_openResult = 0;
    std::thread([] {
        std::lock_guard<std::mutex> lk(g_openMx);
        AudioClose();
    }).detach();
}

static void BeginOpenAsync(const std::wstring& url) {
    if (url.empty()) return;
    g_openUrl = url;
    int gen = ++g_openGen;
    g_openResult = 0;
    std::thread([url, gen]() {
        std::lock_guard<std::mutex> lk(g_openMx);
        if (g_openGen.load() != gen) return;           // 已经被更新的请求取代
        bool ok = AudioOpen(url);                      // 这里会阻塞到预缓冲完成
        if (g_openGen.load() != gen) {                 // 期间又切歌了：丢掉
            if (ok) AudioClose();
            return;
        }
        g_openResult = ok ? 1 : -1;
    }).detach();
}
static int           g_urlIdx = -1;

static std::string   g_token2_unused;

/* 淡入淡出 */
static float g_vol = 1.0f;              // 淡入淡出用的系数
static float g_userVol = 0.8f;          // 用户音量 0..1
static bool  g_volOpen = false;         // 音量面板展开
static bool  g_volDrag = false;
static D2D1_RECT_F g_volTrack{};        // 弹出面板里滑杆的矩形（拖拽要用）
static bool  g_volMuted = false;
static float g_volBeforeMute = 0.8f;

/** 实际下发给 MCI 的音量：淡入淡出系数 × 用户音量 */
static void ApplyVolume() {
    if (g_audioOk) AudioSetVolume((int)(g_vol * g_userVol * 900.0f + 0.5f));
}

static std::wstring LocalDir();

/* ── 音质选择 ── */
static int  g_quality = 1;              // 0 128K / 1 320K / 2 无损
static bool g_qualOpen = false;
// MCI 只能解 mp3，FLAC 无损会 open 失败，所以只提供这两档
static const int      kQualCount = 3;
static const wchar_t* kQualName[3] = { T(L"标准", L"Standard"), T(L"高品", L"High"), T(L"无损", L"Lossless") };
static const char*    kQualApi[3]  = { "128", "320", "flac" };

/* 当前这首歌实际有哪些档位：bit0=标准 bit1=高品 bit2=无损
   未知(0xFF) 时三档全显示；解析完按拿到的档位收窄 */
static std::mutex    g_qmMx;
static int           g_qmIdx = -1;
static unsigned char g_qmMask = 0xFF;

static unsigned char QualMaskFor(int idx) {
    std::lock_guard<std::mutex> lk(g_qmMx);
    return (idx == g_qmIdx) ? g_qmMask : 0xFF;
}
static void SetQualMask(int idx, int gotLevel) {
    std::lock_guard<std::mutex> lk(g_qmMx);
    g_qmIdx = idx;
    g_qmMask = (gotLevel >= 2) ? 0x07 : ((gotLevel == 1) ? 0x03 : 0x01);
}

static std::wstring QualFile() { return LocalDir() + L"\\quality.txt"; }
static void SaveQuality() {
    FILE* f = nullptr;
    _wfopen_s(&f, QualFile().c_str(), L"wb");
    if (!f) return;
    char b[8]; sprintf(b, "%d", g_quality);
    fwrite(b, 1, strlen(b), f); fclose(f);
}
static void LoadQuality() {
    FILE* f = nullptr;
    _wfopen_s(&f, QualFile().c_str(), L"rb");
    if (!f) return;
    char b[8] = { 0 }; fread(b, 1, sizeof(b) - 1, f); fclose(f);
    int v = atoi(b);
    if (v >= 0 && v < kQualCount) g_quality = v;
}
static std::wstring VolumeFile() { return LocalDir() + L"\\volume.txt"; }

static void SaveVolume() {
    FILE* f = nullptr;
    _wfopen_s(&f, VolumeFile().c_str(), L"wb");
    if (!f) return;
    char b[32];
    sprintf(b, "%d", (int)(g_userVol * 100.0f + 0.5f));
    fwrite(b, 1, strlen(b), f);
    fclose(f);
}

static void LoadVolume() {
    FILE* f = nullptr;
    _wfopen_s(&f, VolumeFile().c_str(), L"rb");
    if (!f) return;
    char b[32] = { 0 };
    fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    int v = atoi(b);
    if (v >= 0 && v <= 100) g_userVol = v / 100.0f;
}

/** 音量按钮 + 弹出的滑杆（画在下一首右边） */
static void DrawVolume(float px, float by, float cy);
static int   g_fadeDir = 0;            // +1 淡入 -1 淡出
static int   g_pendingDir = 0;         // 淡出结束后要切的歌
static int   g_pendingPick = -1;       // 选歌界面点了别的歌：等旧歌淡出后再装新的
static int   g_pendingSeekMs = -1;     // 重装后要跳到的位置（切音质时用）

/* 进度条拖动 */
static bool      g_seekDrag = false;
static ULONGLONG g_lastSeek = 0;

static std::wstring FindProjectFile(const std::wstring& name) {
    if (GetFileAttributesW(name.c_str()) != INVALID_FILE_ATTRIBUTES) return name;
    wchar_t exe[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring dir(exe);
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) {
            std::wstring base = dir.substr(0, p);
            std::wstring c1 = base + L"\\" + name;
            if (GetFileAttributesW(c1.c_str()) != INVALID_FILE_ATTRIBUTES) return c1;
            std::wstring up = base.substr(0, base.find_last_of(L"\\/"));
            std::wstring c2 = up + L"\\" + name;
            if (GetFileAttributesW(c2.c_str()) != INVALID_FILE_ATTRIBUTES) return c2;
        }
    }
    return L"";
}

static std::string HttpGet(const std::wstring& url) {
    std::string out;
    HINTERNET hInet = InternetOpenW(L"KugouPlayer/1.0", INTERNET_OPEN_TYPE_DIRECT,
                                    nullptr, nullptr, 0);
    if (!hInet) return out;
    // 默认超时是分钟级：接口没起来时会把工作线程挂住，必须自己设
    DWORD ct = 5000, rt = 8000;
    InternetSetOptionW(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &ct, sizeof(ct));
    InternetSetOptionW(hInet, INTERNET_OPTION_SEND_TIMEOUT, &ct, sizeof(ct));
    InternetSetOptionW(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &rt, sizeof(rt));
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_NO_UI;
    HINTERNET hUrl = InternetOpenUrlW(hInet, url.c_str(), nullptr, 0, flags, 0);
    if (hUrl) {
        char buf[4096];
        DWORD got = 0;
        while (InternetReadFile(hUrl, buf, sizeof(buf), &got) && got > 0)
            out.append(buf, got);
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hInet);
    return out;
}

/* ── 内嵌资源：接口服务 exe + 歌单默认数据 ── */
static bool ReadRes(int id, std::string& out) {
    HRSRC h = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    DWORD sz = SizeofResource(nullptr, h);
    const void* p = LockResource(g);
    if (!p || !sz) return false;
    out.assign((const char*)p, (size_t)sz);
    return true;
}



static HANDLE g_apiProc = nullptr;
static HANDLE g_qqProc   = nullptr;      // 自己拉起来的接口服务，退出时要收掉

static std::wstring LocalDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n ? std::wstring(buf, n) : L".") + L"\\KugouPlayer";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

/** 确保 127.0.0.1:3000 有服务：先看有没有在跑，没有再解压内嵌的 exe 拉起来 */
/* 启动一个 Node 类型的 sidecar：已在跑就跳过，否则解压并拉起，最后等端口就绪 */
static bool StartNodeSidecar(const wchar_t* sub, const wchar_t* script,
                             const wchar_t* probe, HANDLE* procOut, const char* name) {
    if (!HttpGet(probe).empty()) return true;              // 已在跑

    if (!ExtractSidecars()) {
        Logf("ERR", "%s：sidecars 解压失败", name);
        return false;
    }

    std::wstring dir     = SidecarRoot() + L"\\" + sub;
    std::wstring nodeExe = SidecarRoot() + L"\\node.exe";
    std::wstring cmd     = L"\"" + nodeExe + L"\" " + script;

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, dir.c_str(), &si, &pi)) {
        Logf("ERR", "%s 启动失败 code=%lu", name, GetLastError());
        return false;
    }
    if (procOut) *procOut = pi.hProcess; else CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    for (int i = 0; i < 80; i++) {                        // 最多等 20 秒
        if (!HttpGet(probe).empty()) break;
        Sleep(250);
    }
    Logf("APP", "%s 已启动", name);
    return true;
}

static void EnsureSidecar() {
#ifndef FLORAL_NO_TOGETHER
    // 一起听助手：MQTT 凭据通过环境变量传进去
    SetEnvironmentVariableW(L"FLORAL_MQTT_HOST", L"k9bf6e3a.ala.cn-shenzhen.emqxsl.cn");
    SetEnvironmentVariableW(L"FLORAL_MQTT_PORT", L"8883");
    SetEnvironmentVariableW(L"FLORAL_MQTT_USER", L"Floral");
    SetEnvironmentVariableW(L"FLORAL_MQTT_PASS", L"qwertyuiop!@#$%^&*()");
    SetEnvironmentVariableW(L"FLORAL_LISTEN_PORT", L"4100");
    StartNodeSidecar(L"together", L"together-server.js",
                     L"http://127.0.0.1:4100/health", nullptr, "一起听助手");
#endif

    if (g_source == 2) {                                  // QQ
        StartNodeSidecar(L"qq",
                         L"node_modules/@yakult-green-tea/qq-music-api/dist/src/app.js",
                         L"http://127.0.0.1:3200/", &g_qqProc, "QQ 接口服务");
        return;
    }
    if (g_source == 1) {                                  // 网易云
        SetEnvironmentVariableW(L"REAL_IP", L"116.25.146.177");   // 不加这个全是 502
        StartNodeSidecar(L"netease", L"server.js",
                         L"http://127.0.0.1:4000/", &g_apiProc, "网易云接口服务");
        return;
    }

    // 酷狗：内嵌的 pkg 单文件 exe，不需要 node
    if (!HttpGet(L"http://127.0.0.1:3000/").empty()) return;

    std::string bytes;
    if (!ReadRes(IDR_KUGOU_API, bytes) || bytes.size() < 100000) return;

    std::wstring dir = LocalDir();
    std::wstring exe = dir + L"\\kugou-api.exe";

    WIN32_FILE_ATTRIBUTE_DATA fad;
    bool need = !GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fad) ||
                fad.nFileSizeLow != (DWORD)bytes.size();
    if (need) {
        FILE* f = nullptr;
        _wfopen_s(&f, exe.c_str(), L"wb");
        if (!f) return;
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        g_apiProc = pi.hProcess;
        CloseHandle(pi.hThread);
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
            jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
            AssignProcessToJobObject(job, pi.hProcess);
        }
    }
    for (int i = 0; i < 30; i++) {
        if (!HttpGet(L"http://127.0.0.1:3000/").empty()) break;
        Sleep(200);
    }
}

/** 取 JSON 里的字符串字段，容忍冒号/逗号后的空白（node 写的 json 带空格） */
static std::string JsonStr(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return "";
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    if (p >= s.size() || s[p] != '"') return "";
    ++p;
    size_t e = s.find('"', p);
    if (e == std::string::npos) return "";
    return s.substr(p, e - p);
}

static std::string JsonArrFirst(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\":[";
    size_t p = s.find(k);
    if (p == std::string::npos) return "";
    p = s.find('"', p + k.size());
    if (p == std::string::npos) return "";
    size_t e = s.find('"', p + 1);
    if (e == std::string::npos) return "";
    return s.substr(p + 1, e - p - 1);
}

static std::string UrlEnc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            char b[8];
            sprintf(b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/** 读 kugou-session.json 里的 token / userid */
static void LoadSession() {
    std::wstring p = FindProjectFile(L"kugou-session.json");
    if (p.empty()) return;
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"rb");
    if (!f) return;
    std::string s;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    if (g_source == 1 || g_source == 2) {     // 网易云 / QQ：整条 cookie 当 token
        g_token = JsonStr(s, "cookie");
        g_userid = g_token.empty() ? "" : "1";
        return;
    }
    g_token = JsonStr(s, "token");
    std::string uid = "\"userid\":";
    size_t q = s.find(uid);
    if (q != std::string::npos) {
        q += uid.size();
        while (q < s.size() && (s[q] == ' ' || s[q] == '"')) ++q;
        while (q < s.size() && s[q] >= '0' && s[q] <= '9') g_userid += s[q++];
    }
}

/** 取 JSON 里的整数字段，容忍空白 */
static int JsonInt(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return -1;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return -1;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    return atoi(s.c_str() + p);
}

/** 设备 id（酷狗风控要 dfid），带 1 小时缓存，多线程共用 */
static std::string GetDfid() {
    static std::mutex  mx;
    static std::string cached;
    static ULONGLONG   at = 0;
    std::lock_guard<std::mutex> lk(mx);
    ULONGLONG now = GetTickCount64();
    if (!cached.empty() && now - at <= 3600000ULL) return cached;
    cached.clear();
    for (int i = 0; i < 4 && cached.empty(); i++) {
        char mid[64];
        sprintf(mid, "%llu%d", (unsigned long long)now, i);
        std::string r = HttpGet(Widen(std::string("http://127.0.0.1:3000/register/dev?mid=") + mid));
        cached = JsonStr(r, "dfid");
    }
    at = now;
    return cached;
}


static ULONGLONG g_lastLoginCheck = 0;
static void CheckLoginAsync();
static void Trace(const char* fmt, ...);

/** 播放失败了：顺手复查一次登录态（15 秒内不重复查），失效就直接亮提示条 */
static void RecheckLoginOnFailure() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLoginCheck < 15000) return;
    g_lastLoginCheck = now;
    CheckLoginAsync();
}

static void CheckLoginAsync() {
    g_lastLoginCheck = GetTickCount64();
    if (g_userid.empty() || g_token.empty()) {
        g_loginState = 2;                // 连 session 文件都没有，不用发请求
        return;
    }
    std::string uid = g_userid, tok = g_token;
    int src = g_source;
    Trace("login check start");
    std::thread([uid, tok, src]() {
        if (src == 2) {                       // QQ：查 3200 的 /login/status
            std::string raw = HttpGet(Widen("http://127.0.0.1:3200/login/status?cookie=qqmusic_session%3D" +
                                            UrlEnc(tok)));
            if (raw.empty()) { g_loginState = 3; return; }
            if (raw.find("qqmusic_session") == std::string::npos &&
                raw.find("\"code\":200") == std::string::npos) { g_loginState = 2; return; }
            g_loginState = 1;
            return;
        }
        if (src == 1) {                       // 网易云：查 4000 的 /user/account
            std::string raw = HttpGet(Widen("http://127.0.0.1:4000/user/account?cookie=" +
                                            UrlEnc(NgTrimCookie(tok))));
            if (raw.empty()) { g_loginState = 3; return; }
            size_t p = raw.find("\"nickname\"");
            if (raw.find("\"account\":null") != std::string::npos || p == std::string::npos) {
                g_loginState = 2;
                return;
            }
            g_nickname = Widen(JsonStr(raw, "nickname"));
            g_loginState = 1;
            return;
        }
        std::string dfid = GetDfid();
        Trace("login dfid len=%d", (int)dfid.size());

        std::string cookie = "dfid=" + dfid + ";userid=" + uid + ";token=" + tok;
        std::string r = HttpGet(Widen("http://127.0.0.1:3000/user/detail?cookie=" +
                                      UrlEnc(cookie)));
        Trace("login resp len=%d", (int)r.size());
        if (r.empty()) {                 // 接口没响应，不是登录的问题
            g_loginState = 3;
            return;
        }
        if (JsonInt(r, "status") == 1) {
            g_nickname = Widen(JsonStr(r, "nickname"));
            g_vipType  = JsonInt(r, "vip_type");
            g_loginState = 1;
        } else {
            g_loginState = 2;
        }
    }).detach();
}

/** 解析一首歌的播放地址（会阻塞，只在后台线程里调） */
static std::wstring ResolvePlayUrl(int idx) {
    // 一进来就把要用的字段拷出来，之后不再碰 g_app（可能在别的线程被重建）
    std::string hash, cookie;
    long long mixId = 0, albumId = 0;
    {
        App& a = g_app;
        if (idx < 0 || idx >= (int)a.songs.size()) return L"";
        const Song& s = a.songs[(size_t)idx];
        hash = s.hash;
        mixId = s.mixId;
        albumId = s.albumId;
        cookie = "userid=" + g_userid + ";token=" + g_token;
    }
    if (hash.empty()) return L"";

    // dfid 有缓存就用缓存（GetDfid 内部加锁，多个解析线程可以同时进）
    Logf("SRC", "解析 qq id=%s 档位=%d", hash.c_str(), g_quality);
    if (g_source == 2) {                       // QQ 音乐
        std::wstring e;
        int got = -1;
        std::wstring u = QqResolveUrl(hash, g_token, g_quality, e, &got);
        if (u.empty()) {
            Logf("ERR", "QQ 解析失败: %s", Narrow(e).c_str());
            Trace("qq resolve fail");
        } else {
            Logf("SRC", "QQ 解析成功 档位=%d url=%s", got,
                 Narrow(u.substr(0, u.find(L'?') == std::wstring::npos ? 64 : u.find(L'?'))).c_str());
            SetQualMask(idx, got);
        }
        return u;
    }
    Logf("SRC", "解析 qq id=%s 档位=%d", hash.c_str(), g_quality);
    if (g_source == 2) {                       // QQ 音乐
        std::wstring e;
        int got = -1;
        std::wstring u = QqResolveUrl(hash, g_token, g_quality, e, &got);
        if (u.empty()) {
            Logf("ERR", "QQ 解析失败: %s", Narrow(e).c_str());
            Trace("qq resolve fail");
        } else {
            Logf("SRC", "QQ 解析成功 档位=%d url=%s", got,
                 Narrow(u.substr(0, u.find(L'?') == std::wstring::npos ? 64 : u.find(L'?'))).c_str());
            SetQualMask(idx, got);
        }
        return u;
    }
    if (g_source == 1) {                       // 网易云
        static const char* lv[3] = { "standard", "exhigh", "lossless" };
        std::wstring e;
        Logf("SRC", "解析 netease id=%s 档位=%d", hash.c_str(), g_quality);
        int got = -1;
        std::wstring u = NgResolveUrl(hash, lv[g_quality], e, &got);
        if (u.empty()) Logf("ERR", "网易云解析失败: %s", Narrow(e).c_str());
        else { Logf("SRC", "网易云解析成功 档位=%d", got); SetQualMask(idx, got); }
        return u;
    }

    std::string dfid = GetDfid();

    cookie = "dfid=" + dfid + ";" + cookie;
    char url[2048];
    sprintf(url,
            "http://127.0.0.1:3000/song/url?id=%s&album_audio_id=%lld&album_id=%lld&quality=%s&cookie=%s",
            hash.c_str(), mixId, albumId, kQualApi[g_quality], UrlEnc(cookie).c_str());

    // 不是每首歌都有无损/高品：先按请求档位试，拿不到就逐级往下退
    std::string lastResp;
    for (int q = g_quality; q >= 0; q--) {
        char u2[2048];
        sprintf(u2,
                "http://127.0.0.1:3000/song/url?id=%s&album_audio_id=%lld&album_id=%lld&quality=%s&cookie=%s",
                hash.c_str(), mixId, albumId, kQualApi[q], UrlEnc(cookie).c_str());
        for (int attempt = 0; attempt < 2; attempt++) {
            std::string r = HttpGet(Widen(u2));
            lastResp = r;
            std::string u = JsonArrFirst(r, "url");
            if (u.empty()) u = JsonArrFirst(r, "backupUrl");
            if (!u.empty()) {
                if (q != g_quality) Trace("kg quality fallback %d -> %d", g_quality, q);
                SetQualMask(idx, q);
                return Widen(u);
            }
            if (attempt < 1) Sleep(200);
        }
    }

    {   // 失败时把请求和返回写出来，方便排查
        FILE* lf = nullptr;
        _wfopen_s(&lf, (AssetDir() + L"\\url_debug.txt").c_str(), L"wb");
        if (lf) {
            std::string dump = "URL: " + std::string(url) + "\n\ndfid=" + dfid +
                               "\n\nRESP: " + lastResp.substr(0, 1200) + "\n";
            fwrite(dump.data(), 1, dump.size(), lf);
            fclose(lf);
        }
    }
    return L"";
}

/** 找这一首的本地音频：audio/<hash>.mp3（有就用本地，没有就流播） */
static std::wstring FindAudio(int idx) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size()) return L"";
    const std::string& h = a.songs[(size_t)idx].hash;
    if (h.empty()) return L"";
    return FindProjectFile(L"audio\\" + std::wstring(h.begin(), h.end()) + L".mp3");
}

/** 追踪日志：默认关闭，加 --trace 启动才写（打包版不产生垃圾文件） */
static bool g_traceOn = false;

static void Trace(const char* fmt, ...) {
    if (!g_traceOn) return;
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    FILE* f = nullptr;
    _wfopen_s(&f, (AssetDir() + L"\\trace.log").c_str(), L"ab");
    if (!f) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, f);
    fputc('\n', f);
    fclose(f);
}

static void StartFadeIn() {
    g_vol = 0.0f;
    AudioSetVolume(0);
    g_fadeDir = 1;
}

/* ── 地址预取：当前这首开始放，就顺手把下一首的地址解析好 ── */
static std::mutex g_preMx;
static std::map<int, std::wstring> g_urlCache;
static std::vector<int> g_preFetching;

static void PrefetchUrl(int idx) {
    if (idx < 0 || idx >= (int)g_app.songs.size()) return;
    {
        std::lock_guard<std::mutex> lk(g_preMx);
        if (g_urlCache.count(idx)) return;
        for (int x : g_preFetching) if (x == idx) return;
        g_preFetching.push_back(idx);
    }
    std::thread([idx]() {
        std::wstring u = ResolvePlayUrl(idx);
        std::lock_guard<std::mutex> lk(g_preMx);
        if (!u.empty()) g_urlCache[idx] = u;
        for (size_t i = 0; i < g_preFetching.size(); i++) {
            if (g_preFetching[i] == idx) { g_preFetching.erase(g_preFetching.begin() + i); break; }
        }
    }).detach();
}

/** 换曲目：先看本地文件，没有就后台解析地址流播 */
static void LoadAudioFor(int idx) {
    Trace("LoadAudioFor idx=%d", idx);
    BeginCloseAsync();
    g_audioOk = false;
    g_fadeDir = 0;
    g_pendingDir = 0;
    g_audioError = false;
    g_audioErrKind = 0;
    g_audioLoading = true;              // 先挂上"加载中"，出位置就撤
    g_loadingSince = GetTickCount64();

    std::wstring local = FindAudio(idx);
    if (!local.empty() && AudioOpen(local)) {
        AudioPlay();
        g_audioOk = true;
        StartFadeIn();
        g_audioLoading = false;        // 本地文件，不用转圈
        return;
    }

    // 预取过的地址直接用，省一次往返
    std::wstring cached;
    {
        std::lock_guard<std::mutex> lk(g_preMx);
        auto it = g_urlCache.find(idx);
        if (it != g_urlCache.end()) { cached = it->second; g_urlCache.erase(it); }
    }
    Logf("AUD", "尝试打开本地缓存 idx=%d", idx);
    if (!cached.empty()) {
        g_loadingMin = GetTickCount64() + 300;
        BeginOpenAsync(cached);                  // 网络流：后台开，别卡 UI
        PrefetchUrl((idx + 1) % (int)g_app.songs.size());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_urlMx);
        g_urlState = 1;
        g_urlReady.clear();
        g_urlIdx = idx;
    }
    g_loadingMin = GetTickCount64() + 600;   // 流播至少让圈转 0.6 秒
    std::thread([idx]() {
        Trace("resolve start idx=%d", idx);
        std::wstring u = ResolvePlayUrl(idx);
        Trace("resolve end   idx=%d ok=%d", idx, u.empty() ? 0 : 1);
        std::lock_guard<std::mutex> lk(g_urlMx);
        g_urlReady = u;
        g_urlState = u.empty() ? 3 : 2;
    }).detach();
}

static void SwitchTrack(int dir);

static void SwitchTrackNow(int dir) {
    App& a = g_app;
    int n = (int)a.songs.size();
    if (n <= 0) return;
    if (a.current < 0) a.current = 0;

    int from = a.current;
    int to = (from + dir % n + n) % n;

    // 记住切换前盒子里放的是哪两首，动画期间先显示它们
    g_casePrev0 = (from - 1 + n) % n;
    g_caseNext0 = (from + 1) % n;

    g_switchFrom = from;
    g_switchTo = to;
    g_switchDir = (dir >= 0) ? 1 : -1;
    g_switchT = 0.0f;
    g_textDir = (dir >= 0) ? 1 : -1;

    a.current = to;
    a.pos = 0.0;
    a.playing = true;
    g_audioRetry = 0;              // 新的一首，重试次数归零

    LoadAudioFor(to);
}

/** 切歌入口：音频正在放就先淡出，淡完再真切，避免硬切爆音 */
static void SwitchTrack(int dir) {
    if (g_audioOk && g_pendingDir == 0 && g_fadeDir >= 0) {
        g_pendingDir = dir;
        g_fadeDir = -1;
        return;
    }
    SwitchTrackNow(dir);
}

/** 方格上的歌曲信息（压暗底 + 曲名 + 歌手） */
static void DrawTileInfo(const D2D1_RECT_F& r, const Song& s, float a) {
    float w = r.right - r.left;
    float h = r.bottom - r.top;
    if (w < 90.0f || a <= 0.01f) return;

    float sc = w / 150.0f;
    if (sc < 1.0f) sc = 1.0f;
    if (sc > 1.9f) sc = 1.9f;

    float sh = h * 0.50f;
    FillRoundGradV(Rc(r.left, r.bottom - sh, w, sh), UI::R_TILE,
                   0x000000, 0.0f, 0x000000, 0.70f * a);

    Text(s.title, Fmt(13.5f * sc, DWRITE_FONT_WEIGHT_SEMI_BOLD),
         Rc(r.left + 16 * sc, r.bottom - 58 * sc, w - 40 * sc, 22 * sc), 0xFFFFFF,
         DWRITE_TEXT_ALIGNMENT_LEADING, a);
    Text(s.artist, Fmt(11.0f * sc, DWRITE_FONT_WEIGHT_NORMAL),
         Rc(r.left + 16 * sc, r.bottom - 34 * sc, w - 40 * sc, 18 * sc), 0xFFFFFF,
         DWRITE_TEXT_ALIGNMENT_LEADING, a * 0.85f);
}

/** 格子 → CD 的形变：discK=0 是方图，1 是圆盘；infoK 控制方框上的歌曲信息 */
static void DrawMorphDisc(const D2D1_RECT_F& r, const Song& s, int idx, float discK, float infoK) {
    if (discK < 0.0f) discK = 0.0f;
    if (discK > 1.0f) discK = 1.0f;
    if (discK <= 0.001f) {
        DrawCoverArt(r, s, idx, UI::R_TILE);
        if (infoK > 0.0f) DrawTileInfo(r, s, infoK);
        return;
    }

    // 全程连续：黑胶盘从小长到满，封面同时往中心缩。没有"到某个值才出现"的开关
    const float e = discK * discK * (3.0f - 2.0f * discK);      // smoothstep
    const float w = r.right - r.left;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float R  = w * 0.5f;

    // 先算封面，再让黑胶"从封面边缘往外长"——环的厚度从 0 连续增加，
    // 不会出现"某一帧突然多出一圈黑"的跳变
    const float artR  = R + (R * 0.66f - R) * e;                // R → 0.66R（和 DrawDisc 一致）
    const float discR = artR + (R - artR) * e;                  // 从 artR 长到 R
    FillCircle(cx, cy, discR, 0x2A2026, 1.0f);
    const float radius = UI::R_TILE + (artR - UI::R_TILE) * e;  // 圆角 → 正圆
    DrawCoverArt(Rc(cx - artR, cy - artR, artR * 2.0f, artR * 2.0f), s, idx, radius);

    // 还是方框形态时，歌曲信息跟着淡出
    if (infoK > 0.0f && e < 0.55f) {
        float ia = infoK * (1.0f - e / 0.55f);
        if (ia > 0.0f) DrawTileInfo(r, s, ia);
    }

    // 纹路和中心孔：跟着盘面一起淡出来
    if (e > 0.15f) {
        float k = (e - 0.15f) / 0.85f;
        StrokeCircle(cx, cy, discR * 0.93f, 0xFFFFFF, 0.05f * k, 1.0f);
        StrokeCircle(cx, cy, discR * 0.78f, 0xFFFFFF, 0.05f * k, 1.0f);
        FillCircle(cx, cy, R * 0.05f, UI::BG, 0.9f * k);
        StrokeCircle(cx, cy, R * 0.05f, 0xFFFFFF, 0.18f * k, 1.0f);
    }
}

static void DrawSpinner(float cx, float cy, float r, float ang, float alpha);

/** 转场用：直接扫布局取视口内的块，不依赖分条索引（分条表可能是空的/过期的） */
static const std::vector<TileBox>& TransBoxes() {
    return (g_draw.size() == g_base.size()) ? g_draw : g_base;
}

static void CollectVisible(float W, float H, std::vector<int>& out) {
    out.clear();
    const std::vector<TileBox>& boxes = TransBoxes();
    for (int i = 0; i < (int)boxes.size(); i++) {
        const TileBox& b = boxes[(size_t)i];
        float ty = GRID_TOP + b.y - g_app.scroll;
        if (ty > H + 160.0f) continue;
        if (ty + b.h < -160.0f) continue;
        out.push_back(i);
    }
}

/** 进播放页的转场（只画到主盘就位，之后交给 DrawPlayerPage） */
static void DrawEntering() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.songs.size();
    const bool hasSel = (g_enterIdx >= 0 && g_enterIdx < n);
    if (n == 0) return;                 // 一首歌都没有，没什么可散的

    const float lineY = UI::TITLE_H + 4.0f;
    const float dd = (std::min)(W * 0.42f, H * 0.62f);
    const float cx = dd * 0.10f;
    const float cy = (lineY + H) * 0.5f;
    D2D1_RECT_F discRect = Rc(cx - dd * 0.5f, cy - dd * 0.5f, dd, dd);

    const float t = g_enterT;
    auto ease = [](float x) { return x * x * (3.0f - 2.0f * x); };
    auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    // ── 1. 其他格子化作 CD：从原位直接飞向正上 / 正下 ──
    // 注意：转场期间网格层不画了，所以这里从第 0 帧就要把它们画在原位
    {
        CollectVisible(W, H, g_vis);
        const std::vector<TileBox>& boxes = TransBoxes();
        for (int i : g_vis) {
            if (i == g_enterIdx || i < 0 || i >= n) continue;
            if (i >= (int)boxes.size()) continue;
            const TileBox& bx = boxes[(size_t)i];
            float c0x = bx.x + bx.w * 0.5f;
            float c0y = bx.y + GRID_TOP - a.scroll + bx.h * 0.5f;
            if (c0y < -200.0f || c0y > H + 200.0f) continue;

            // 离中轴越远越晚出发
            float sx = std::fabs(c0x - W * 0.5f) / (W * 0.5f);
            float eo = clamp01((t - 0.10f - sx * 0.10f) / 0.50f);
            if (eo >= 1.0f) continue;                  // 已经飞没了

            float e = (eo <= 0.0f) ? 0.0f : ease(eo);
            float dirY = (c0y < cy) ? -1.0f : 1.0f;
            float tx = W * 0.5f;                                   // 正上 / 正下
            float ty = (dirY < 0) ? (-dd * 0.75f) : (H + dd * 0.75f);

            float nx = c0x + (tx - c0x) * e;
            float ny = c0y + (ty - c0y) * e;
            float sz = (std::min)(bx.w, bx.h) * (1.0f - 0.32f * e);
            DrawMorphDisc(Rc(nx - sz * 0.5f, ny - sz * 0.5f, sz, sz),
                          a.songs[i], i, clamp01(eo * 1.35f), 0.0f);
        }
    }

    // ── 2. 选中的那首：先挪到正中等着，等其他 CD 全飞完了再变盘就位 ──
    //    从顶部按钮进来又没有当前歌曲时没有这一格，整段跳过
    if (hasSel) {
        D2D1_RECT_F k1 = Rc(W * 0.5f - dd * 0.40f, (lineY + H) * 0.5f - dd * 0.40f,
                            dd * 0.80f, dd * 0.80f);

        D2D1_RECT_F r;
        float discK;
        if (t < 0.20f) {
            float p = ease(clamp01(t / 0.20f));
            r = LerpRect(g_enterFrom, k1, p);
            discK = 0.0f;
        } else if (t < 0.66f) {
            r = k1;                 // 停在正中，等别人走干净
            discK = 0.0f;
        } else {
            float p = clamp01((t - 0.66f) / 0.14f);
            float pe = ease(p);
            r = LerpRect(k1, discRect, pe);
            discK = pe;
        }
        DrawMorphDisc(r, a.songs[(size_t)g_enterIdx], g_enterIdx, discK, 1.0f);
    }

    // ── 3. 分隔线 ──
    {
        float e = g_lineT * g_lineT * (3.0f - 2.0f * g_lineT);
        float lw = W * e;
        if (lw > 0.5f) Line(W - lw, lineY, W, lineY, 0xC9AEB8, 1.0f);
    }
}


/** 环形加载圈：一段逐渐变亮的圆弧，转起来当 spinner */
/* 极简加载环：一根细轨道 + 一段圆弧扫过，圆头端帽，无发光 */
static void StrokeArc(float cx, float cy, float r, float startDeg, float sweepDeg,
                      UINT32 col, float alpha, float w) {
    if (!g_d2d || !g_brush || !g_rt || r < 1.0f) return;
    if (sweepDeg <= 0.5f) return;

    const float a0 = startDeg * 3.14159265f / 180.0f;
    const float a1 = (startDeg + sweepDeg) * 3.14159265f / 180.0f;

    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&geo)) || !geo) return;
    ID2D1GeometrySink* sk = nullptr;
    if (FAILED(geo->Open(&sk)) || !sk) { geo->Release(); return; }

    sk->BeginFigure(D2D1::Point2F(cx + cosf(a0) * r, cy + sinf(a0) * r),
                    D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(cx + cosf(a1) * r, cy + sinf(a1) * r),
        D2D1::SizeF(r, r), 0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        sweepDeg > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    sk->Release();

    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND);
    ID2D1StrokeStyle* ss = nullptr;
    g_d2d->CreateStrokeStyle(sp, nullptr, 0, &ss);

    g_brush->SetColor(Col(col));
    g_brush->SetOpacity(alpha * g_alphaMul);
    g_rt->DrawGeometry(geo, g_brush, w, ss);
    g_brush->SetOpacity(1.0f);          // 画笔是共享的，用完必须复位
    if (ss) ss->Release();
    geo->Release();
}

static void DrawSpinner(float cx, float cy, float r, float ang, float alpha) {
    const float w = 2.0f;
    // 轨道：整圈很淡
    StrokeCircle(cx, cy, r, UI::ACCENT, alpha * 0.13f, w);
    // 扫过的弧：100°，圆头
    StrokeArc(cx, cy, r, ang, 100.0f, UI::ACCENT, alpha * 0.95f, w);
}
/** 任意实心多边形 */
static void FillPoly(const D2D1_POINT_2F* p, int n, UINT32 col, float a) {
    if (!g_d2d || !g_brush || n < 3) return;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&geo)) || !geo) return;
    ID2D1GeometrySink* sk = nullptr;
    if (FAILED(geo->Open(&sk)) || !sk) { geo->Release(); return; }
    sk->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i < n; i++) sk->AddLine(p[i]);
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close();
    sk->Release();
    g_brush->SetColor(Col(col));
    g_brush->SetOpacity(a * g_alphaMul);
    g_rt->FillGeometry(geo, g_brush);
    g_brush->SetOpacity(1.0f);          // 同上
    geo->Release();
}

/** 喇叭图标：一条闭合路径画主体，声波用旋钮式弧线 */
static void DrawSpeakerIcon(float cx, float cy, float s, bool muted, float vol) {
    UINT32 col = muted ? UI::TEXT3 : UI::TEXT2;
    D2D1_POINT_2F pts[6] = {
        { cx - s,          cy - s * 0.28f },
        { cx - s * 0.34f,  cy - s * 0.28f },
        { cx + s * 0.30f,  cy - s * 1.00f },
        { cx + s * 0.30f,  cy + s * 1.00f },
        { cx - s * 0.34f,  cy + s * 0.28f },
        { cx - s,          cy + s * 0.28f },
    };
    FillPoly(pts, 6, col, 1.0f);

    if (muted || vol <= 0.02f) {                 // 静音：叉
        Line(cx + s * 0.55f, cy - s * 0.5f, cx + s * 1.5f, cy + s * 0.5f, UI::TEXT3, 1.7f);
        Line(cx + s * 1.5f, cy - s * 0.5f, cx + s * 0.55f, cy + s * 0.5f, UI::TEXT3, 1.7f);
        return;
    }
    // 一到三圈声波：半径递增，透明度递减
    int waves = (vol > 0.7f) ? 3 : (vol > 0.35f ? 2 : 1);
    float base = cx + s * 0.38f;
    for (int w = 0; w < waves; w++) {
        float r = s * (0.72f + 0.62f * (float)w);
        float a = 0.9f - 0.22f * (float)w;
        const int N = 12;
        D2D1_POINT_2F prev{};
        for (int i = 0; i <= N; i++) {
            float ang = (-0.85f + 1.70f * (float)i / (float)N);   // ±49°
            D2D1_POINT_2F pt = { base + cosf(ang) * r, cy + sinf(ang) * r };
            if (i > 0) Line(prev.x, prev.y, pt.x, pt.y, col, 1.7f, a);
            prev = pt;
        }
    }
}

/** 实心三角（旧接口保留） */
static void FillTri(float x1, float y1, float x2, float y2, float x3, float y3,
                    UINT32 col, float a) {
    if (!g_d2d || !g_brush) return;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&geo)) || !geo) return;
    ID2D1GeometrySink* sk = nullptr;
    if (FAILED(geo->Open(&sk)) || !sk) { geo->Release(); return; }
    sk->BeginFigure(D2D1::Point2F(x1, y1), D2D1_FIGURE_BEGIN_FILLED);
    sk->AddLine(D2D1::Point2F(x2, y2));
    sk->AddLine(D2D1::Point2F(x3, y3));
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close();
    sk->Release();
    g_brush->SetColor(Col(col));
    g_brush->SetOpacity(a * g_alphaMul);
    g_rt->FillGeometry(geo, g_brush);
    g_brush->SetOpacity(1.0f);          // 同上
    geo->Release();
}

/** 音量按钮 + 弹出滑杆 */
static void DrawVolume(float px, float by, float cy) {
    D2D1_RECT_F vb = Rc(px + 186, by, 40, 40);
    bool hv = Inside(vb, g_app.mouse);
    if (hv || g_volOpen) FillRound(vb, 20.0f, UI::HOVER);

    // 喇叭图标
    DrawSpeakerIcon(vb.left + 19.0f, by + 20.0f, 8.0f, g_volMuted, g_userVol);
    Hit hv2; hv2.r = vb; hv2.kind = HK_VOL;
    if (g_alpha > 0.5f) g_app.hits.push_back(hv2);


    // ── 音质按钮（音量右边）──
    D2D1_RECT_F qb = Rc(vb.right + 10.0f, by + 8.0f, 52, 24);
    bool qh = Inside(qb, g_app.mouse);
    if (qh || g_qualOpen) FillRound(qb, 12.0f, UI::HOVER);
    Text(kQualName[g_quality], Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL), qb,
         UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
    Hit hq; hq.r = qb; hq.kind = HK_QUAL;
    if (g_alpha > 0.5f) g_app.hits.push_back(hq);

    if (g_qualT > 0.01f) {
    PanelFx fxq(g_qualT, qb.left + 26.0f, by + 40.0f);

    const float qw = 96.0f, rh = 30.0f, qh2 = rh * (float)kQualCount + 10.0f;
    D2D1_RECT_F qp = Rc(qb.left - 6.0f, by + 40.0f, qw, qh2);
    if (qp.bottom > g_app.height - 8.0f) qp = Rc(qb.left - 6.0f, by - qh2 - 6.0f, qw, qh2);
    FillRound(Rc(qp.left + 3, qp.top + 8, qw, qh2), 12.0f, UI::SHADOW, 0.14f);
    FillRound(qp, 12.0f, UI::PANEL);
    const unsigned char mask = QualMaskFor(g_app.current);
    for (int i = 0; i < kQualCount; i++) {
        const bool avail = (mask & (1u << i)) != 0;
        D2D1_RECT_F row = Rc(qp.left + 6, qp.top + 5 + rh * (float)i, qw - 12, rh);
        bool rh_hot = avail && Inside(row, g_app.mouse);
        bool cur = (i == g_quality);
        if (cur || rh_hot) FillRound(row, 8.0f, cur ? UI::SELECT : UI::HOVER);
        UINT32 fg = !avail ? 0xC9B8BE : (cur ? UI::ACCENT_TEXT : UI::TEXT2);
        Text(kQualName[i], Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(row.left + 12, row.top, 60, rh), fg);
        if (!avail) {                                  // 这首歌没有这一档
            Text(T(L"不可用", L"Unavailable"), Fmt(10.0f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(row.right - 52, row.top, 44, rh), 0xC9B8BE,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            Hit hqi; hqi.r = row; hqi.kind = HK_QUAL_ITEM; hqi.index = i;
            if (g_alpha > 0.5f) g_app.hits.push_back(hqi);
        }
    }
    }   // ← 音质面板到此结束

#ifndef FLORAL_NO_TOGETHER
    // ── 一起听按钮（音质右边）──
    {
        D2D1_RECT_F tb2 = Rc(qb.right + 10.0f, by, 40.0f, 40.0f);
        bool th2 = Inside(tb2, g_app.mouse);
        bool on = TogetherActive();
        if (th2 || g_togOpen) FillRound(tb2, 20.0f, UI::HOVER);
        UINT32 icol = on ? UI::ACCENT : UI::TEXT2;
        const float ccx = tb2.left + 20.0f, ccy = by + 22.0f;
        // 耳麦：上半圆弧 + 两侧耳罩
        {
            const float rr = 8.5f;
            for (int k = 0; k <= 12; k++) {
                float a1 = 3.14159f + 3.14159f * (float)k / 12.0f;
                float a2 = 3.14159f + 3.14159f * (float)(k + 1) / 12.0f;
                Line(ccx + cosf(a1) * rr, ccy + sinf(a1) * rr,
                     ccx + cosf(a2) * rr, ccy + sinf(a2) * rr, icol, 2.0f);
            }
            FillRound(Rc(ccx - rr - 3.0f, ccy - 1.0f, 5.0f, 9.0f), 2.5f, icol);
            FillRound(Rc(ccx + rr - 2.0f, ccy - 1.0f, 5.0f, 9.0f), 2.5f, icol);
        }
        if (on) FillCircle(tb2.left + 31.0f, tb2.top + 9.0f, 3.0f, 0x6FBF73);
        Hit ht2; ht2.r = tb2; ht2.kind = HK_TOG;
        if (g_alpha > 0.5f) g_app.hits.push_back(ht2);

        if (g_togT > 0.01f) {
            PanelFx fxt(g_togT, tb2.left + 20.0f, by + 46.0f);
            const float pw2 = 430.0f, ph2 = on ? 96.0f : 176.0f;
            D2D1_RECT_F tp = Rc(tb2.left - pw2 + 40.0f, by + 46.0f, pw2, ph2);
            if (tp.left < 12.0f) tp = Rc(12.0f, by + 46.0f, pw2, ph2);
            FillRound(Rc(tp.left + 3, tp.top + 8, pw2, ph2), 12.0f, UI::SHADOW, 0.14f);
            FillRound(tp, 12.0f, UI::PANEL);
            {   // 卡片空白处吞掉点击，别一碰就关
                Hit hcard; hcard.r = tp; hcard.kind = HK_TOG_CARD;
                if (g_alpha > 0.5f) g_app.hits.push_back(hcard);
            }

            if (on) {
                std::wstring who = g_togPeer.empty()
                    ? (g_togHosting ? T(L"房间已开，等待对方加入…", L"Room open, waiting…") : T(L"已连接", L"Connected"))
                    : (std::wstring(T(L"正在与 ", L"Listening with ")) + g_togPeer);
                Text(who, Fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(tp.left + 16, tp.top + 16, pw2 - 120, 22), UI::TEXT);
                std::wstring code = Widen(TogetherMyCode());
                if (!code.empty())
                    Text(std::wstring(T(L"你的房间码：", L"Your room code: ")) + code, Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL),
                         Rc(tp.left + 16, tp.top + 44, pw2 - 32, 18), UI::TEXT3);
                D2D1_RECT_F cp = Rc(tp.right - 152, tp.top + 16, 60, 26);
                {
                    bool ch2 = Inside(cp, g_app.mouse);
                    FillRound(cp, 8.0f, ch2 ? UI::SELECT : UI::GHOST);
                    Text(T(L"复制", L"Copy"), Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL), cp,
                         ch2 ? UI::ACCENT_TEXT : UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
                    Hit hcp; hcp.r = cp; hcp.kind = HK_TOG_COPY;
                    if (g_alpha > 0.5f) g_app.hits.push_back(hcp);
                }
                D2D1_RECT_F lv = Rc(tp.right - 84, tp.top + 16, 68, 26);
                bool lh = Inside(lv, g_app.mouse);
                if (lh) FillRound(lv, 8.0f, UI::SELECT);
                Text(T(L"退出", L"Leave"), Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL), lv,
                     lh ? UI::ACCENT_TEXT : UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
                Hit hl; hl.r = lv; hl.kind = HK_TOG_LEAVE;
                if (g_alpha > 0.5f) g_app.hits.push_back(hl);
            } else {
                // 第一行：我的连接码
                Text(T(L"我的房间码", L"My room code"), Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(tp.left + 16, tp.top + 12, 100, 16), UI::TEXT3);

                std::wstring code = g_togMyCode;
                Text(code.empty() ? T(L"未开房", L"No room") : code,
                     Fmt(9.5f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(tp.left + 16, tp.top + 30, pw2 - 130, 18),
                     code.empty() ? UI::TEXT3 : UI::TEXT);
                D2D1_RECT_F hb = Rc(tp.right - 96, tp.top + 12, 80, 26);
                bool hh = Inside(hb, g_app.mouse);
                if (hh) FillRound(hb, 8.0f, UI::SELECT);
                Text(code.empty() ? T(L"开房", L"Open room") : T(L"复制", L"Copy"),
                     Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL), hb,
                     hh ? UI::ACCENT_TEXT : UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
                Hit hb2; hb2.r = hb;
                hb2.kind = code.empty() ? HK_TOG_HOST : HK_TOG_COPY;
                if (g_alpha > 0.5f) g_app.hits.push_back(hb2);

                // 第二行：输入对方连接码
                Text(T(L"输入房间码，回车加入", L"Enter room code, press Enter"), Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(tp.left + 16, tp.top + 92, pw2 - 32, 16), UI::TEXT3);
                D2D1_RECT_F ib = Rc(tp.left + 16, tp.top + 112, pw2 - 116, 30);
                FillRound(ib, 8.0f, g_togInput ? UI::BG : UI::GHOST);
                if (g_togInput) FillRound(ib, 8.0f, 0xFFFFFF);
                std::wstring shown = g_togInputText;
                if (shown.empty() && !g_togInput) shown = L"";
                Text(shown, Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(ib.left + 10, ib.top, ib.right - ib.left - 20, 30),
                     UI::TEXT, DWRITE_TEXT_ALIGNMENT_LEADING);
                if (g_togInput) {
                    float cx2 = ib.left + 10 + (float)shown.size() * 6.2f;
                    Line(cx2, ib.top + 7, cx2, ib.bottom - 7, UI::ACCENT, 1.4f);
                }
                Hit hi; hi.r = ib; hi.kind = HK_TOG_INPUT;
                if (g_alpha > 0.5f) g_app.hits.push_back(hi);

                D2D1_RECT_F jb = Rc(tp.right - 88, tp.top + 112, 72, 30);
                const bool hasTxt = !g_togInputText.empty();
                bool jh = Inside(jb, g_app.mouse) && hasTxt;
                FillRound(jb, 8.0f, jh ? UI::SELECT : (hasTxt ? UI::BG : UI::GHOST));
                Text(T(L"加入", L"Join"), Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL), jb,
                     hasTxt ? (jh ? UI::ACCENT_TEXT : UI::TEXT2) : 0xB9A6AD,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
                if (!g_togInputText.empty()) {
                    Hit hj; hj.r = jb; hj.kind = HK_TOG_JOIN;
                    if (g_alpha > 0.5f) g_app.hits.push_back(hj);
                }
                if (!g_togMsg.empty()) {
                    float dy = 0.0f;
                    float ta = ToastFade(g_togMsgAt, 6.0f, dy);
                    if (ta > 0.01f) {
                        Text(g_togMsg, Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL),
                             Rc(tp.left + 16, tp.top + 148 + dy, pw2 - 32, 18), 0xB04A63,
                             DWRITE_TEXT_ALIGNMENT_LEADING, ta);
                    }
                }
            }
        }
    }
#endif  // FLORAL_NO_TOGETHER

    if (g_volT <= 0.01f) return;         // ↓ 音量面板，两者互不影响
    PanelFx fxv(g_volT, vb.left + 20.0f, by + 20.0f);

    // 弹出卡片
    const float pw = 190.0f, ph = 54.0f;
    // 开在按钮右边，别挡住进度条和时间；右边放不下就翻到左边
    D2D1_RECT_F pop = Rc(vb.right + 10, by - 6.0f, pw, ph);
    if (pop.right > g_app.width - 12.0f) pop = Rc(vb.left - 10 - pw, by - 6.0f, pw, ph);
    FillRound(Rc(pop.left + 3, pop.top + 8, pw, ph), 12.0f, UI::SHADOW, 0.14f);
    FillRound(pop, 12.0f, UI::PANEL);

    wchar_t lab[32];
    swprintf(lab, 32, T(L"音量 %d%%", L"Volume %d%%"), (int)(g_userVol * 100.0f + 0.5f));
    Text(lab, Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL),
         Rc(pop.left + 14, pop.top + 9, 120, 16), UI::TEXT2);

    float tx = pop.left + 14, tw = pw - 28, ty = pop.top + 36;
    FillRound(Rc(tx, ty - 2, tw, 4), 2.0f, 0xF0DDE4);
    float k = g_volMuted ? 0.0f : g_userVol;
    if (k > 0.0f) FillRound(Rc(tx, ty - 2, tw * k, 4), 2.0f, UI::SHADOW);
    FillCircle(tx + tw * k, ty, 6.0f, 0xFFFFFF);
    StrokeRound(Rc(tx + tw * k - 6, ty - 6, 12, 12), 6.0f, UI::SHADOW, 1.6f);

    g_volTrack = Rc(tx, ty - 1, tw, 3);
    Hit ht; ht.r = Rc(tx - 8, ty - 12, tw + 16, 24); ht.kind = HK_VOL_TRACK;
    if (g_alpha > 0.5f) g_app.hits.push_back(ht);
}

static void DrawPlayerPage();

/** 退出播放页：先整屏淡出，再让网格从右边飞入（和开屏同一套） */
static void DrawExiting() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.songs.size();
    const float t = g_exitT;

    // ── 阶段 A：播放页整屏淡到底色 ──
    if (t < 0.22f) {
        DrawPlayerPage();
        g_alpha = 1.0f;
        DrawTitleBar(W);                 // 顶栏也要一起淡掉，否则点击返回的瞬间会「啪」地消失
        a.hits.clear();
        FillRect(Rc(0, 0, W, H), UI::BG, t / 0.22f);
        return;
    }
    if (n == 0) return;

    // ── 阶段 B：网格从右边飞入 ──
    const float tb = (t - 0.22f) / 0.78f;
    EnsureBase(W, n);
    EnsureTarget(W, n, -1);

    CollectVisible(W, H, g_vis);
    if (g_draw.size() != g_base.size()) g_draw = g_target;

    int order = 0;
    for (int i : g_vis) {
        float delay = order * 0.035f;
        float p = (tb - delay) / 0.45f;
        order++;
        if (p <= 0.0f) continue;
        if (p > 1.0f) p = 1.0f;

        float e = 1.0f - powf(1.0f - p, 3.0f);
        const TileBox& d = g_draw[(size_t)i];
        float ty = GRID_TOP + d.y - a.scroll;
        float dx = (1.0f - e) * (W + 60.0f - d.x);

        g_alphaMul = e;
        DrawTile(Rc(d.x + dx, ty, d.w, d.h), a.songs[(size_t)i], i, 0.0f);
        g_alphaMul = 1.0f;
    }

    // 顶栏统一由 DrawUI 在最上层画（不透明），这里不再重复画一条半透明的
}

static void DrawPlayerPage() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.songs.size();
    if (a.current < 0 || a.current >= n) {
        // ── 还没选歌：空白唱片 + 提示 ──
        const float lineY = UI::TITLE_H + 4.0f;
        const float dd = (std::min)(W * 0.42f, H * 0.62f);
        const float cx = dd * 0.10f;
        const float cy = (lineY + H) * 0.5f;
        {
            // 空白唱片：黑胶盘面 + 几道纹路 + 中心贴纸（没有封面）
            float r = dd * 0.5f;
            // 空唱片：底色 / 纹路 / 中心贴纸全部跟主题
            FillCircle(cx, cy, r, UI::DISC);
            for (int i = 1; i <= 4; i++) {
                float rr = r * (0.50f + 0.11f * (float)i);
                StrokeRound(Rc(cx - rr, cy - rr, rr * 2.0f, rr * 2.0f), rr,
                            UI::DISC_GROOVE, 1.0f, 0.5f);
            }
            FillCircle(cx, cy, r * 0.33f, MixColor(UI::SELECT, UI::PANEL, 0.35f));
            StrokeRound(Rc(cx - r * 0.33f, cy - r * 0.33f, r * 0.66f, r * 0.66f),
                        r * 0.33f, UI::BTN_CLOSE, 1.2f);
            FillCircle(cx, cy, 4.5f, UI::PANEL);           // 轴孔
        }

        D2D1_RECT_F back = Rc(W * 0.46f - 6.0f, UI::TITLE_H + 18.0f, 96, 28);
        bool bh = Inside(back, a.mouse);
        if (bh) FillRound(back, 8.0f, UI::HOVER);
        Text(T(L"< 返回 Esc", L"< Back  Esc"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL), back, UI::TEXT2);
        Hit hb; hb.r = back; hb.kind = HK_BACK;
        a.hits.push_back(hb);

        float px = W * 0.46f;
        Text(T(L"暂无播放歌曲", L"Nothing playing"), Fmt(22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             Rc(px, cy - 26, W - px - 40, 34), UI::TEXT);
        Text(T(L"在左边的列表里点一首开始播放", L"Pick a song from the list"), Fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(px, cy + 12, W - px - 40, 22), UI::TEXT3);
        return;
    }
    const Song& s = a.songs[(size_t)a.current];

    // 返回（放到右侧信息栏上方，左上角留给专辑盒子）
    D2D1_RECT_F back = Rc(W * 0.46f - 6.0f, UI::TITLE_H + 18.0f, 96, 28);
    bool bh = Inside(back, a.mouse);
    if (bh) FillRound(back, UI::R_BTN, UI::HOVER);
    Line(back.left + 10, back.top + 14, back.left + 18, back.top + 7, UI::TEXT2, 1.6f);
    Line(back.left + 10, back.top + 14, back.left + 18, back.top + 21, UI::TEXT2, 1.6f);
    Text(T(L"返回  Esc", L"Back  Esc"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
         Rc(back.left + 26, back.top, 70, 28), UI::TEXT2);
    Hit hb; hb.r = back; hb.kind = HK_BACK;
    if (g_alpha > 0.5f) a.hits.push_back(hb);

    // ── 左：主 CD 居中；上一首 / 下一首缩成小 CD 藏在左上、左下角，只露一角 ──
    const float lineY = UI::TITLE_H + 4.0f;
    const float dd = (std::min)(W * 0.42f, H * 0.62f);
    const float cx = dd * 0.10f;                            // 圆心略偏出左边缘
    const float cy = (lineY + H) * 0.5f;

    // 转场余下的部分：角落弹出、右侧浮现
    float cornerK;
    {
        float k = (g_enterT - 0.80f) / 0.20f;
        k = (k < 0.0f) ? 0.0f : (k > 1.0f ? 1.0f : k);
        float ek = 1.0f - powf(1.0f - k, 3.0f);
        // 弹过头一点再收回：0 → 118% → 100%
        float pop = (ek <= 0.75f) ? (ek / 0.75f) * 1.18f
                                  : 1.18f - 0.18f * ((ek - 0.75f) / 0.25f);
        cornerK = (k <= 0.0f) ? 0.0f : pop;
    }
    float infoK = (g_enterT - 0.90f) / 0.10f;
    infoK = (infoK < 0.0f) ? 0.0f : (infoK > 1.0f ? 1.0f : infoK);

    int prevIdx = (a.current - 1 + n) % n;
    int nextIdx = (a.current + 1) % n;

    D2D1_RECT_F discRect = Rc(cx - dd * 0.5f, cy - dd * 0.5f, dd, dd);

    // 角落小 CD：圆心压进角落，露出比四分之一多一点
    const float sr = (std::max)(28.0f, dd * 0.115f);
    D2D1_RECT_F discPrev = Rc(sr * 0.25f - sr, lineY + sr * 0.25f - sr, sr * 2, sr * 2);
    D2D1_RECT_F discNext = Rc(sr * 0.25f - sr, H - sr * 0.25f - sr, sr * 2, sr * 2);

    const bool switching = (g_switchT < 1.0f) && (g_switchFrom >= 0) && (g_switchFrom < n);

    // 转场时角落从无到有地弹出
    if (cornerK < 1.0f) {
        auto shrink = [cornerK](const D2D1_RECT_F& r) {
            float mx = (r.left + r.right) * 0.5f;
            float my = (r.top + r.bottom) * 0.5f;
            float hw = (r.right - r.left) * 0.5f * cornerK;
            float hh = (r.bottom - r.top) * 0.5f * cornerK;
            return Rc(mx - hw, my - hh, hw * 2, hh * 2);
        };
        discPrev = shrink(discPrev);
        discNext = shrink(discNext);
    }

    // 角落两张小的（裁到分隔线以下，不压标题）
    // 上角：切歌期间保持"原来那张"，等旧盘落上去之后再换——正好被盖住，不会闪
    int cornerPrev = prevIdx;
    if (switching && g_casePrev0 >= 0 && g_casePrev0 < n) cornerPrev = g_casePrev0;

    // 飞行的起止点：完全在画面外，所以是"从看不见的地方转进来 / 转出去"
    D2D1_RECT_F offPrev = Rc(-sr * 2.2f, lineY - sr * 2.2f, sr * 2, sr * 2);
    D2D1_RECT_F offNext = Rc(-sr * 2.2f, H + sr * 1.2f,    sr * 2, sr * 2);

    g_rt->PushAxisAlignedClip(Rc(0, lineY, W, H - lineY), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (switching) {
        float t = g_switchT;
        float e = t * t * (3.0f - 2.0f * t);              // smoothstep

        const bool fwd = (g_switchDir > 0);

        // 正位这张去向 / 新的一张来处
        D2D1_RECT_F toCorner   = fwd ? discPrev : discNext;
        D2D1_RECT_F fromCorner = fwd ? discNext : discPrev;
        // 被挤走的那张 / 补进来的那张
        D2D1_RECT_F pushOut = fwd ? discPrev : discNext;
        D2D1_RECT_F slideIn = fwd ? discNext : discPrev;
        D2D1_RECT_F outOff  = fwd ? offPrev  : offNext;
        D2D1_RECT_F inOff   = fwd ? offNext  : offPrev;

        int pushedIdx = fwd ? g_casePrev0 : g_caseNext0;
        int slideIdx  = fwd ? nextIdx : prevIdx;
        if (pushedIdx < 0 || pushedIdx >= n) pushedIdx = slideIdx;

        // 角度都收敛到 g_discAngle，飞行途中各多转一整圈
        float angOut = g_discAngle + 360.0f * e;
        float angIn  = g_discAngle + 360.0f * (1.0f - e);

        // 1) 角落最老的那张：让位，转出画面
        DrawDisc(LerpRect(pushOut, outOff, e), a.songs[(size_t)pushedIdx], pushedIdx, angOut, 1.0f);
        // 2) 新的一张：从画面外补到角落
        DrawDisc(LerpRect(inOff, slideIn, e), a.songs[(size_t)slideIdx], slideIdx, angIn, 1.0f);
        // 3) 正位这张：缩进角落
        DrawDisc(LerpRect(discRect, toCorner, e), a.songs[(size_t)g_switchFrom],
                 g_switchFrom, angOut, 1.0f);
        // 4) 角落那张：放大转到正位（最上层）
        DrawDisc(LerpRect(fromCorner, discRect, e), s, a.current, angIn, 1.0f);
    } else {
        if (cornerK > 0.01f) {
            // 角落两张小唱片静止不转，点一下切到那首
            DrawDisc(discPrev, a.songs[(size_t)prevIdx], prevIdx, 0.0f, 1.0f);
            DrawDisc(discNext, a.songs[(size_t)nextIdx], nextIdx, 0.0f, 1.0f);
            if (g_alpha > 0.5f && !switching) {
                Hit hp; hp.r = discPrev; hp.kind = HK_PREV;
                a.hits.push_back(hp);
                Hit hn; hn.r = discNext; hn.kind = HK_NEXT;
                a.hits.push_back(hn);
            }
        }
        DrawDisc(discRect, s, a.current, g_discAngle, 1.0f);
    }
    g_rt->PopAxisAlignedClip();

    // 加载中不改盘面，只在播放按钮上转小圈
    const bool showSpin = g_audioLoading && (GetTickCount64() - g_loadingSince > 180);
    {
        float e = g_lineT * g_lineT * (3.0f - 2.0f * g_lineT);
        float lw = W * e;
        if (lw > 0.5f) Line(W - lw, lineY, W, lineY, 0xC9AEB8, 1.0f);
    }

    // ── 右：歌曲信息 ──
    const float px = (std::max)(W * 0.46f, cx + dd * 0.5f + 90.0f);
    const float pw = W - px - UI::PAD * 2;
    g_alphaMul = infoK;
    if (pw > 80.0f) {
        // 三行：下一首往左滑出、从右滑入；上一首反过来。逐行错开 30ms
        const float SLIDE = 26.0f;
        auto drawSet = [&](const std::wstring& tt, const std::wstring& ar, const std::wstring& al,
                           float phase, bool entering) {
            const float dur = entering ? 0.16f : 0.13f;
            auto lineT = [&](int i) {
                float v = (phase - i * 0.03f) / dur;
                return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v);
            };
            struct { const std::wstring* s; float y, size; DWRITE_FONT_WEIGHT w; UINT32 c; } rows[3] = {
                { &tt, cy - 104, 25.0f, DWRITE_FONT_WEIGHT_BOLD,   UI::TEXT  },
                { &ar, cy -  62, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, UI::TEXT2 },
                { &al, cy -  34, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, UI::TEXT3 },
            };
            for (int i = 0; i < 3; i++) {
                float t = lineT(i);
                // 位移和透明度都走曲线：进场 ease-out 减速落位，退场 ease-in 加速离开
                float e = entering ? (1.0f - powf(1.0f - t, 3.0f)) : (t * t);
                float a = entering ? e : (1.0f - e);
                if (a <= 0.01f) continue;
                float dx = entering ? (1.0f - e) * SLIDE * (float)g_textDir
                                    : -e * SLIDE * (float)g_textDir;
                dx = floorf(dx + 0.5f);          // 取整：亚像素会让字形每帧抖
                Text(*rows[i].s, Fmt(rows[i].size, rows[i].w),
                     Rc(px + dx, rows[i].y, pw, rows[i].size + 12.0f),
                     rows[i].c, DWRITE_TEXT_ALIGNMENT_LEADING, a);
            }
        };



        if (g_textPhase == 1) {
            drawSet(g_outTitle, g_outArtist, g_outAlbum, g_textT, false);   // 旧文字滑出
        } else if (g_textPhase == 2) {
            drawSet(s.title, s.artist, s.album, g_textT, true);             // 新文字滑入
        } else {
            drawSet(s.title, s.artist, s.album, 10.0f, true);               // 静止：直接满
        }

        if (g_audioError) {
            Text(g_audioErrKind == 2 ? T(L"无法播放 · 解码失败", L"Cannot play · decode failed") : T(L"无法播放 · 地址解析失败", L"Cannot play · URL failed"), Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(px, cy - 10, pw, 18), UI::ACCENT);
        }

        // 进度
        float barW = (std::min)(pw, 380.0f);
        float barY = cy + 10.0f;
        float ratio = s.durationSec > 0 ? (float)(a.pos / s.durationSec) : 0.0f;
        ratio = (std::max)(0.0f, (std::min)(1.0f, ratio));

        FillRound(Rc(px, barY, barW, 4), 2.0f, UI::SELECT);
        if (ratio > 0) FillRound(Rc(px, barY, barW * ratio, 4), 2.0f, UI::SHADOW);
        if (Inside(Rc(px, barY - 8, barW, 22), a.mouse)) {
            FillRound(Rc(px + barW * ratio - 5, barY - 3, 10, 10), 5.0f, 0xFFFFFF);
            StrokeRound(Rc(px + barW * ratio - 5, barY - 3, 10, 10), 5.0f, UI::SHADOW, 1.5f);
        }
        Hit hp; hp.r = Rc(px, barY - 8, barW, 22); hp.kind = HK_PROGRESS;
        if (g_alpha > 0.5f) a.hits.push_back(hp);

        Text(FormatTime((int)a.pos), Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(px, barY + 10, 60, 16), UI::TEXT3);
        Text(FormatTime(s.durationSec), Fmt(11.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(px + barW - 60, barY + 10, 60, 16), UI::TEXT3,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

        // 上一首 / 播放 / 下一首
        float by = cy + 58.0f;
        float r = 26.0f;

        D2D1_RECT_F pv = Rc(px + 6, by, 40, 40);
        bool phv = Inside(pv, a.mouse);
        if (phv) FillRound(pv, 20.0f, UI::HOVER);
        Line(pv.left + 14, by + 20, pv.left + 26, by + 12, UI::TEXT2, 2.0f);
        Line(pv.left + 14, by + 20, pv.left + 26, by + 28, UI::TEXT2, 2.0f);
        Hit hpr; hpr.r = pv; hpr.kind = HK_PREV;
        if (g_alpha > 0.5f) a.hits.push_back(hpr);

        D2D1_RECT_F btn = Rc(px + 62, by - 6, r * 2, r * 2);
        bool ph = Inside(btn, a.mouse);
        FillRound(btn, r, ph ? UI::ACCENT : UI::BTN_CLOSE);
        float bx = px + 62 + r, byy = by - 6 + r;
        if (showSpin) {
            DrawSpinner(bx, byy, r * 0.55f, g_spinAngle, 1.0f);
        } else {
            // 暂停（两条竖线）与播放（三角）之间做 120ms 交叉淡化
            // 播放 ⇄ 暂停：几何形变。两个四边形同时插值，
            // 三角形的右顶点拆开长成右边的竖条，主体收成左边的竖条。
            // 不做淡入淡出，所以任何时刻都只有一个实心图形，也不会空帧。
            {
                float e = g_playIconMix;
                e = e * e * (3.0f - 2.0f * e);            // smoothstep：起步和收尾都软
                auto L = [&](float a, float b) { return a + (b - a) * e; };

                // 左块：三角形（顶点重复）→ 左竖条
                D2D1_POINT_2F left[4] = {
                    D2D1::Point2F(bx + L(-6.0f, -6.0f), byy + L(-9.5f, -8.0f)),
                    D2D1::Point2F(bx + L( 8.0f, -2.6f), byy + L( 0.0f, -8.0f)),
                    D2D1::Point2F(bx + L( 8.0f, -2.6f), byy + L( 0.0f,  8.0f)),
                    D2D1::Point2F(bx + L(-6.0f, -6.0f), byy + L( 9.5f,  8.0f)),
                };
                // 右块：全部缩在三角形顶点（不可见）→ 右竖条
                D2D1_POINT_2F right[4] = {
                    D2D1::Point2F(bx + L(8.0f, 2.6f), byy + L(0.0f, -8.0f)),
                    D2D1::Point2F(bx + L(8.0f, 6.0f), byy + L(0.0f, -8.0f)),
                    D2D1::Point2F(bx + L(8.0f, 6.0f), byy + L(0.0f,  8.0f)),
                    D2D1::Point2F(bx + L(8.0f, 2.6f), byy + L(0.0f,  8.0f)),
                };
                FillPoly(left, 4, 0xFFFFFF, 1.0f);
                FillPoly(right, 4, 0xFFFFFF, 1.0f);
            }
        }
        Hit hpl; hpl.r = btn; hpl.kind = HK_PLAY;
        if (g_alpha > 0.5f) a.hits.push_back(hpl);

        D2D1_RECT_F nx = Rc(px + 128, by, 40, 40);
        bool nhv = Inside(nx, a.mouse);
        if (nhv) FillRound(nx, 20.0f, UI::HOVER);
        Line(nx.left + 26, by + 20, nx.left + 14, by + 12, UI::TEXT2, 2.0f);
        Line(nx.left + 26, by + 20, nx.left + 14, by + 28, UI::TEXT2, 2.0f);
        Hit hnx; hnx.r = nx; hnx.kind = HK_NEXT;
        if (g_alpha > 0.5f) a.hits.push_back(hnx);

        DrawVolume(px, by, cy);
    }
    g_alphaMul = 1.0f;      // 恢复，别影响标题栏
}

/* ── 选中歌单：后台拉曲目 → 主线程换歌单 ── */
static std::string PlSafe(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    return r;
}

static std::mutex      g_plSwapMx;
static std::atomic<bool> g_plBusy{ false };
static std::wstring    g_plSwapFile;       // 拉好了的 tsv 文件名
static int             g_plSwapIdx = -1;
static std::wstring    g_plSwapErr;

static void SelectPlaylistAsync(int idx) {
    if (idx < 0 || idx >= (int)g_app.playlists.size()) return;
    if (g_plBusy.exchange(true)) return;

    std::string gcid = g_app.playlists[(size_t)idx].gcid;
    const std::string safe = PlSafe(gcid);          // 注意：不能对两个临时对象取 begin/end
    std::wstring name = L"pl-" + std::wstring(safe.begin(), safe.end()) + L".tsv";
    std::wstring path = AssetDir() + L"\\" + name;

    Logf("APP", "切换歌单 idx=%d %s", idx, Narrow(g_app.playlists[(size_t)idx].name).c_str());
    std::thread([gcid, name, path, idx]() {
        std::wstring err;
        int n = KgExportPlaylist(g_token, g_userid, gcid, path, err);
        {
            std::lock_guard<std::mutex> lk(g_plSwapMx);
            if (n > 0) { g_plSwapFile = name; g_plSwapIdx = idx; g_plSwapErr.clear(); }
            else       { g_plSwapErr = err; g_plSwapIdx = -1; }
        }
        g_plBusy = false;
        g_coverDirty = true;
    }).detach();
}

/* 主线程：把拉好的歌单换进来 */
static void DrainPlaylistSwap() {
    std::wstring file;
    std::wstring err;
    int idx = -1;
    {
        std::lock_guard<std::mutex> lk(g_plSwapMx);
        if (!g_plSwapFile.empty()) { file = g_plSwapFile; idx = g_plSwapIdx; g_plSwapFile.clear(); }
        else if (!g_plSwapErr.empty()) { err = g_plSwapErr; g_plSwapErr.clear(); }
    }
    if (!file.empty()) {
        g_likesName = file;
        InitData();
        AdoptLocalCovers();
        g_covFlag.assign(g_app.songs.size(), 0);
        App& a = g_app;
        a.current = -1;
        a.playing = false;
        a.pos = 0.0;
        a.scroll = 0.0f;
        a.scrollTarget = 0.0f;
        a.pageAnim = 0.0f;
        a.page = Page::Grid;
        a.plActive = idx;
        g_gridEnter = 0.0f;          // 选歌页：曲目从右侧飞入
        g_coverDirty = true;
        Logf("APP", "歌单已切换：%d 首", (int)a.songs.size());
    } else if (!err.empty()) {
        Logf("ERR", "切换歌单失败: %s", Narrow(err).c_str());
    }
}
/* ══════════════════════════════════════════
   歌单选择页：圆形蜂窝排布，可拖动
   ══════════════════════════════════════════ */
static const float PL_D      = 96.0f;               // 圆直径
static const float PL_PILL_W = 300.0f;              // 展开的胶囊宽度
static const float PL_GAP    = 1.16f;               // 圆之间留够呼吸感
static const float PL_SQ3    = 1.7320508f;

/* 正六边形排布：以中心为第 0 环向外扩散，
   坐标为轴向坐标 (q, r)，相邻圆心距离 = 直径 */
struct PlCell { float x, y; };
static std::vector<PlCell> g_plCells;
static int                 g_plCellsFor = -1;

static void BuildPlCells(int n) {
    g_plCells.clear();
    g_plCellsFor = n;
    if (n <= 0) return;

    const float step = PL_D * PL_GAP;       // 相邻圆心距

    // 第 0 环：圆心
    g_plCells.push_back({ 0.0f, 0.0f });

    int left = n - 1;
    for (int ring = 1; left > 0; ring++) {
        int cap = 6 * ring;                 // 这一环能放几个
        int take = (left < cap) ? left : cap;
        if (take == cap) {
            // 整环：走六边形点阵，保证邻居等距
            // 环起点 (ring, 0)，依次沿 6 条边推进
            const int dirs[6][2] = { { -1, 1 }, { -1, 0 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 0, 1 } };
            int q = ring, r = 0;
            for (int d = 0; d < 6 && take > 0; d++) {
                for (int k = 0; k < ring && take > 0; k++) {
                    float x = step * ((float)q + (float)r * 0.5f);
                    float y = step * ((float)r * (PL_SQ3 * 0.5f));
                    g_plCells.push_back({ x, y });
                    q += dirs[d][0]; r += dirs[d][1];
                    take--;
                }
            }
        } else {
            // 不满一圈：先取这一环的全部格子，按角度排好，再等间隔挑 take 个
            // 这样仍落在六边形点阵上，间距均匀，整体也是对称的六边形
            std::vector<PlCell> ringCells;
            const int dirs[6][2] = { { -1, 1 }, { -1, 0 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 0, 1 } };
            int q = ring, r0 = 0;
            for (int d = 0; d < 6; d++) {
                for (int k = 0; k < ring; k++) {
                    float x = step * ((float)q + (float)r0 * 0.5f);
                    float y = step * ((float)r0 * (PL_SQ3 * 0.5f));
                    ringCells.push_back({ x, y });
                    q += dirs[d][0]; r0 += dirs[d][1];
                }
            }
            std::stable_sort(ringCells.begin(), ringCells.end(), [](const PlCell& a, const PlCell& b) {
                return atan2f(a.y, a.x) < atan2f(b.y, b.x);
            });
            for (int i = 0; i < take && !ringCells.empty(); i++) {
                size_t idx = (size_t)((long long)i * (long long)ringCells.size() / take);
                if (idx >= ringCells.size()) idx = ringCells.size() - 1;
                g_plCells.push_back(ringCells[idx]);
            }
        }
        left -= take;
    }
}

static void PlPos(int i, float& x, float& y) {
    if (g_plCellsFor != (int)g_app.playlists.size()) BuildPlCells((int)g_app.playlists.size());
    if (i < 0 || i >= (int)g_plCells.size()) { x = y = 0; return; }
    x = g_plCells[(size_t)i].x;
    y = g_plCells[(size_t)i].y;
}

static void PlContentSize(float& w, float& h) {
    if (g_plCellsFor != (int)g_app.playlists.size()) BuildPlCells((int)g_app.playlists.size());
    float minX = 0, maxX = 0, minY = 0, maxY = 0;
    bool first = true;
    for (const PlCell& c : g_plCells) {
        if (first) { minX = maxX = c.x; minY = maxY = c.y; first = false; }
        minX = (std::min)(minX, c.x); maxX = (std::max)(maxX, c.x);
        minY = (std::min)(minY, c.y); maxY = (std::max)(maxY, c.y);
    }
    w = (maxX - minX) + PL_D;
    h = (maxY - minY) + PL_D;
}

/* 平移范围：最外圈那些圆心最多拖到窗口边上（圆心连成的多边形就是边界） */
static void PlClampPan() {
    App& a = g_app;
    if (g_plCellsFor != (int)a.playlists.size()) BuildPlCells((int)a.playlists.size());
    if (g_plCells.empty()) { a.plX = a.plY = 0.0f; return; }

    float minX = g_plCells[0].x, maxX = g_plCells[0].x;
    float minY = g_plCells[0].y, maxY = g_plCells[0].y;
    for (const PlCell& c : g_plCells) {
        minX = (std::min)(minX, c.x); maxX = (std::max)(maxX, c.x);
        minY = (std::min)(minY, c.y); maxY = (std::max)(maxY, c.y);
    }

    // 判定点是窗口中心：拖动时它必须始终落在最外圈圆心围成的范围里
    // 屏幕上中心恒为 (0,0)（内容坐标），即要求 minX+plX <= 0 <= maxX+plX
    float minPanX = -maxX;
    float maxPanX = -minX;
    float minPanY = -maxY;
    float maxPanY = -minY;

    if (minPanX > maxPanX) { float m = (minPanX + maxPanX) * 0.5f; minPanX = maxPanX = m; }
    if (minPanY > maxPanY) { float m = (minPanY + maxPanY) * 0.5f; minPanY = maxPanY = m; }

    if (a.plX < minPanX) a.plX = minPanX;
    if (a.plX > maxPanX) a.plX = maxPanX;
    if (a.plY < minPanY) a.plY = minPanY;
    if (a.plY > maxPanY) a.plY = maxPanY;
}

/* 进入歌单页时把内容摆到正中 */
static void PlCenterContent() {
    g_app.plX = 0.0f;
    g_app.plY = 0.0f;
    g_app.plTargetX = 0.0f;
    g_app.plTargetY = 0.0f;
    g_app.plVelX = g_app.plVelY = 0.0f;
    g_app.plActive = -1;
    g_app.plSelT = 0.0f;
    g_app.plHover = -1;
    PlClampPan();
}

/* 选中后其它圆被推开的偏移 */
/* 第 i 个圆在屏幕上的圆心（不含推开） */
static void PlScreenCenter(int i, float& cx, float& cy) {
    App& a = g_app;
    float x = 0, y = 0;
    PlPos(i, x, y);
    cx = a.width * 0.5f + x + a.plX;
    cy = UI::TITLE_H + (a.height - UI::TITLE_H) * 0.5f + y + a.plY;
}

/* 展开后胶囊卡片的屏幕矩形（没选中时是空矩形） */
static D2D1_RECT_F PlCapsuleRect() {
    App& a = g_app;
    if (a.plActive < 0) return Rc(0, 0, 0, 0);
    float e = a.plSelT;
    e = e * e * (3.0f - 2.0f * e);
    if (e <= 0.001f) return Rc(0, 0, 0, 0);
    float cx = 0, cy = 0;
    PlScreenCenter(a.plActive, cx, cy);
    const float R = PL_D * 0.5f, pillH = PL_D + 20.0f;
    return Rc(cx - R - 10.0f, cy - pillH * 0.5f,
              PL_PILL_W * e + R * 2.0f + 20.0f, pillH);
}

/* 展开后卡片的目标矩形（窗口正中） */
static D2D1_RECT_F PlCardTargetRect() {
    const float W = g_app.width, H = g_app.height;
    const float cardW = (std::min)(620.0f, W - 80.0f);
    const float cardH = 150.0f;
    const float cx = W * 0.5f;
    const float cy = UI::TITLE_H + (H - UI::TITLE_H) * 0.5f;
    return Rc(cx - cardW * 0.5f, cy - cardH * 0.5f, cardW, cardH);
}

/* 当前帧所有圆的最终屏幕圆心（含让位与去重叠），绘制和点击共用 */
static std::vector<D2D1_POINT_2F> g_plPos;
static int g_plPosFor = -1;

/* Container Transform：其它圆不参与布局，选中时整体淡出即可 */
static void PlBuildPositions() {
    App& a = g_app;
    const int n = (int)a.playlists.size();
    g_plPos.assign((size_t)n, D2D1::Point2F(0, 0));
    g_plPosFor = n;
    for (int i = 0; i < n; i++) {
        float cx = 0, cy = 0;
        PlScreenCenter(i, cx, cy);
        g_plPos[(size_t)i] = D2D1::Point2F(cx, cy);
    }
}
/* 第 i 个圆的最终屏幕圆心 */
static void PlScreenPos(int i, float& cx, float& cy) {
    App& a = g_app;
    if (g_plPosFor != (int)a.playlists.size()) PlBuildPositions();
    if (i >= 0 && i < (int)g_plPos.size()) {
        cx = g_plPos[(size_t)i].x;
        cy = g_plPos[(size_t)i].y;
    } else {
        PlScreenCenter(i, cx, cy);
    }
}

static void DrawPlaylistPicker() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.playlists.size();

    if (n == 0) {
        Text(T(L"还没有歌单数据", L"No playlists yet"), Fmt(14.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(0, H * 0.45f, W, 24), UI::TEXT3, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    // 正在拉歌单：底部给个提示，别让人以为点坏了
    if (g_plBusy.load()) {
        FillRect(Rc(0, UI::TITLE_H, W, H - UI::TITLE_H), UI::BG, 0.55f);
        DrawSpinner(W * 0.5f, H * 0.5f - 10.0f, 20.0f, (float)(GetTickCount64() % 4000) * 0.26f, 0.95f);
        int sel = a.plActive;
        if (sel >= 0 && sel < n) {
            Text(a.playlists[(size_t)sel].name, Fmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(0, H * 0.5f + 26, W, 22), UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    const float R = PL_D * 0.5f;

    PlBuildPositions();          // 本帧所有圆的最终位置（让位 + 去重叠）

    // ── Container Transform：选中的圆长成卡片，其它圆淡出 ──
    float selE = 0.0f;
    D2D1_RECT_F cardR{};
    float cardRadius = 0.0f;
    float avatarCx = 0.0f, avatarCy = 0.0f;
    if (a.plActive >= 0 && a.plActive < n) {
        selE = a.plSelT;
        selE = selE * selE * (3.0f - 2.0f * selE);

        float ccx = 0, ccy = 0;
        PlScreenPos(a.plActive, ccx, ccy);
        const Playlist& spl = a.playlists[(size_t)a.plActive];

        // 目标卡片：窗口正中，宽 620 高 150
        D2D1_RECT_F from = Rc(ccx - R, ccy - R, R * 2, R * 2);
        D2D1_RECT_F to   = PlCardTargetRect();
        const float cardCY = (to.top + to.bottom) * 0.5f;

        auto L = [&](float p, float q) { return p + (q - p) * selE; };
        // 先插值四条边，再换算成 Rc 需要的宽高
        float cardL = L(from.left, to.left);
        float cardT = L(from.top, to.top);
        float cardRt = L(from.right, to.right);
        float cardB = L(from.bottom, to.bottom);
        cardR = Rc(cardL, cardT, cardRt - cardL, cardB - cardT);
        cardRadius = L(R, 26.0f);

        // 背景压暗，衬托卡片
        if (selE > 0.02f) FillRect(Rc(0, UI::TITLE_H, W, H - UI::TITLE_H), 0x000000, 0.28f * selE);

        FillRound(cardR, cardRadius, UI::CARD, 0.99f);
        StrokeRound(cardR, cardRadius, UI::BORDER, 0.80f, 1.0f);

        // 封面：从圆心滑到卡片左侧的头像位
        const float avR = R * 0.92f;
        float avToX = to.left + 30.0f + avR;
        float avToY = cardCY;
        avatarCx = L(ccx, avToX);
        avatarCy = L(ccy, avToY);

        // 文字：标题 0.15 / 曲目数 0.42 / 提示 0.60 开始出现
        auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        float tx = avToX + avR + 22.0f;
        float tw = to.right - tx - 30.0f;
        if (tw > 40.0f) {
            float ta = clamp01((selE - 0.15f) / 0.55f);
            if (ta > 0.01f) {
                TextEllipsis(spl.name, Fmt(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                             Rc(tx, cardCY - 44.0f - (1.0f - ta) * 6.0f, tw, 28.0f), UI::TEXT, ta);
            }
            float tb = clamp01((selE - 0.42f) / 0.40f);
            if (tb > 0.01f) {
                wchar_t sub[128];
                swprintf(sub, 128, T(L"%d 首", L"%d songs"), spl.count);
                Text(sub, Fmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(tx, cardCY - 8.0f - (1.0f - tb) * 6.0f, tw, 20.0f), UI::TEXT2,
                     DWRITE_TEXT_ALIGNMENT_LEADING, tb);
            }
            float tc = clamp01((selE - 0.60f) / 0.32f);
            if (tc > 0.01f) {
                Text(T(L"再点一次进入", L"Click again to open"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
                     Rc(tx, cardCY + 20.0f - (1.0f - tc) * 6.0f, tw, 18.0f), UI::TEXT3,
                     DWRITE_TEXT_ALIGNMENT_LEADING, tc * 0.9f);
            }
        }
    }

    // 封面画成圆形（选中项的头像也用它）
    auto drawCoverCircle = [&](int idx, float cx, float cy, float rad, float alpha) {
        const Playlist& pl = a.playlists[(size_t)idx];
        RequestPlaylistCover(idx);
        bool painted = false;
        if (CoverBmp* e = CoverEntry(PlCoverPath(pl))) {
            if (!e->brush) {
                D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
                    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP);
                g_rt->CreateBitmapBrush(e->bmp, &bp, nullptr, &e->brush);
            }
            if (e->brush && e->bmp) {
                D2D1_SIZE_F sz = e->bmp->GetSize();
                if (sz.width > 0 && sz.height > 0) {
                    float sc = (2.0f * rad) / (std::min)(sz.width, sz.height);
                    float ox = cx - sz.width * sc * 0.5f;
                    float oy = cy - sz.height * sc * 0.5f;
                    e->brush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) *
                                           D2D1::Matrix3x2F::Translation(ox, oy));
                    e->brush->SetOpacity(g_alpha * g_alphaMul * alpha);
                    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), e->brush);
                    painted = true;
                }
            }
        }
        if (!painted) {
            int ci = ((idx % 8) + 8) % 8;
            if (g_covBrush[ci]) {
                g_covBrush[ci]->SetStartPoint(D2D1::Point2F(cx - rad, cy - rad));
                g_covBrush[ci]->SetEndPoint(D2D1::Point2F(cx + rad, cy + rad));
                g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), g_covBrush[ci]);
            } else {
                FillCircle(cx, cy, rad, UI::SELECT, 0.8f * alpha);
            }
            if (!pl.name.empty()) {
                std::wstring ch(1, pl.name[0]);
                Text(ch, Fmt(26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                     Rc(cx - rad, cy - rad, rad * 2, rad * 2), 0xFFFFFF,
                     DWRITE_TEXT_ALIGNMENT_CENTER, 0.65f * alpha);
            }
        }
    };

    // 未选中的圆：随卡片展开淡出（Container Transform 的"其余内容淡出"）
    float othersA = 1.0f - selE * 0.92f;
    for (int i = 0; i < n; i++) {
        if (i == a.plActive) continue;                 // 选中的改由卡片头像表示
        float cx = 0, cy = 0;
        PlScreenPos(i, cx, cy);
        if (cx < -R * 1.4f || cx > W + R * 1.4f) continue;
        if (cy < UI::TITLE_H - R * 1.4f || cy > H + R * 1.4f) continue;
        if (othersA <= 0.02f) continue;

        float keep = g_alphaMul;
        g_alphaMul = keep * othersA;
        drawCoverCircle(i, cx, cy, R, 1.0f);
        bool hot = (a.plHover == i);
        StrokeCircle(cx, cy, R - 0.75f, hot ? UI::ACCENT : UI::BORDER, hot ? 0.95f : 0.55f, 1.6f);
        g_alphaMul = keep;

        Hit h; h.r = Rc(cx - R, cy - R, R * 2, R * 2); h.kind = HK_PL; h.index = i;
        if (g_alpha > 0.5f && a.plActive < 0) a.hits.push_back(h);   // 选中后圆已淡出，不再响应
    }

    // 选中项的头像：跟着卡片一起从圆心滑到卡片左侧
    if (a.plActive >= 0 && a.plActive < n && selE > 0.01f) {
        drawCoverCircle(a.plActive, avatarCx, avatarCy, R * 0.92f, 1.0f);
        StrokeCircle(avatarCx, avatarCy, R * 0.92f - 0.75f, UI::ACCENT, 0.9f * selE, 1.6f);
    }
}
/* ══════════════════════════════════════════
   总绘制
   ══════════════════════════════════════════ */
/* ══════════════════════════════════════════
   开屏动画：空白 → 曲目从右侧飞入依次排好 → 标题栏浮现
   ══════════════════════════════════════════ */
/** 第一阶段：logo 与文字浮现、停留、渐隐 */
static void DrawSplash(float t, float W, float H) {
    float a;
    if (t < SPLASH_IN)                 a = t / SPLASH_IN;
    else if (t < SPLASH_HOLD_END)      a = 1.0f;
    else                               a = 1.0f - (t - SPLASH_HOLD_END) / (SPLASH_OUT - SPLASH_HOLD_END);
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;

    // 浮现时从下方微微升起
    float in = (t < SPLASH_IN) ? (t / SPLASH_IN) : 1.0f;
    float e = 1.0f - powf(1.0f - in, 3.0f);
    float dy = (1.0f - e) * 18.0f;

    const float cx = W * 0.5f;

    g_alphaMul = a;

    // 图标 + 标题作为一组，整体居中
    const float logoD = 92.0f, gap = 18.0f, titleH = 30.0f;
    const float totalH = logoD + gap + titleH;
    const float top = (H - totalH) * 0.5f + dy;

    D2D1_RECT_F logo = Rc(cx - logoD * 0.5f, top, logoD, logoD);
    FillRoundGrad(logo, 24.0f, UI::BTN_CLOSE, UI::BTN_CLOSE);
    Text(L"F", Fmt(30.0f, DWRITE_FONT_WEIGHT_BOLD), logo, 0xFFFFFF,
         DWRITE_TEXT_ALIGNMENT_CENTER);

    Text(L"Floral", Fmt(20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
         Rc(0, top + logoD + gap, W, titleH), UI::TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);

    g_alphaMul = 1.0f;
}

static void DrawIntro() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    int n = (int)a.songs.size();
    if (n == 0) return;

    // 加载阶段：只有一个居中的加载环，不出现 logo 和文字
    if (!g_loadDone) {
        const float cx = W * 0.5f;
        const float cy = H * 0.5f;
        // 开头 200ms 淡入，避免从纯色背景里"啪"地蹦出来
        float fade = g_loadT / 0.20f;
        if (fade > 1.0f) fade = 1.0f;
        DrawSpinner(cx, cy, 20.0f, g_loadT * 260.0f, 0.95f * fade);
        DrawTitleBar(W);          // 顶栏从第一帧就完整可用
        return;
    }

    // 第一阶段：logo + 文字浮现 → 渐隐
    if (g_introT < INTRO_TILE_START) {
        DrawSplash(g_introT, W, H);
        // 加载环在开头 200ms 内淡出，和 logo 的淡入交接
        if (g_introT < 0.20f) {
            float f = 1.0f - g_introT / 0.20f;
            DrawSpinner(W * 0.5f, H * 0.5f, 20.0f, g_loadT * 260.0f, 0.95f * f);
        }
        DrawTitleBar(W);
        return;
    }

    // 开屏之后直接交给歌单选择页（它自己淡入），不再飞入歌曲卡片
    DrawTitleBar(W);
}

static void DrawUI(ID2D1RenderTarget* rt) {
    Bind(rt);
    {   // 主题变了：占位渐变要跟着重建
        static int lastGen = -1;
        if (lastGen != ThemeGeneration()) {
            lastGen = ThemeGeneration();
            BuildCoverBrushes(g_rt);
            Logf("APP", "主题换代 %d：占位渐变已重建", lastGen);
        }
    }
    DrainDecoded();               // 后台解好的封面搬进来
    DrainCovers();                // 按需下载的封面落地，排进解码
    App& a = g_app;
    const float W = a.width, H = a.height;

    a.hits.clear();
    g_alpha = 1.0f;
    FillRect(Rc(0, 0, W, H), UI::BG);

    // 设置面板打开时，先屏蔽背景的 hover：把鼠标挪到画面外，画完面板再还原
    const POINT realMouse = a.mouse;
    if (g_settingsOpen) a.mouse = POINT{ -100000, -100000 };

    if (!g_introDone) {         // 开屏阶段：只画飞入
        DrawIntro();
        return;
    }

    if (g_exitOn) {             // 退出播放页：淡出 + 网格飞入
        DrawExiting();
        return;
    }

    const D2D1_POINT_2F center = D2D1::Point2F(W * 0.5f, H * 0.5f);

    // 歌单选择页：整页独立绘制
    if (a.page == Page::Playlists) {
        g_alpha = g_plEnter;          // 开屏结束后的淡入
        DrawPlaylistPicker();
        g_alpha = 1.0f;
        DrawTitleBar(W);
        if (g_setT > 0.01f) {
            a.mouse = realMouse;
            // 面板是模态的：遮罩和卡片的点击区要在 PanelFx 之外注册，
            // 否则动画没结束时会被裁掉，点击就会穿透到后面的圆
            { Hit bg; bg.r = Rc(0, 0, W, H); bg.kind = HK_SET_BG; a.hits.push_back(bg); }
            { Hit hc; hc.r = SettingsCardRect(W, H); hc.kind = HK_SET_CARD; a.hits.push_back(hc); }
            FillRect(Rc(0, 0, W, H), 0x000000, 0.20f * g_setT);
            PanelFx fx(g_setT, W * 0.5f, H * 0.5f);
            DrawSettings(W, H);
        }
        return;
    }

    // 网格层
    if (a.pageAnim < 0.999f) {
        g_alpha = 1.0f - a.pageAnim;
        float sc = 1.0f - 0.05f * a.pageAnim;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, center));
        DrawGrid();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // 播放层
    if (a.pageAnim > 0.001f) {
        g_alpha = a.pageAnim;
        if (g_enterOn && g_enterT < 0.80f) {
            DrawEntering();            // 转场前半段：格子飞散 + 主盘就位
        } else {
            rt->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, 1.0f, center));
            DrawPlayerPage();
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    g_alpha = 1.0f;

    // 没有账号 / 登录过期：自动弹出平台选择（每次启动只弹一次）
    if (g_loginState == 2 && !g_loginAsked && !g_loginRunning && !g_exitOn && !g_enterOn) {
        g_loginAsked = true;
        g_settingsOpen = true;
        g_loginChoose = true;
    }

    // 顶栏：全程不透明、每帧都画。不做进场淡入淡出——
    // 之前那段"隐藏 1.4 秒"既让按钮点不动，又让整条栏看着半透明。
    DrawTitleBar(W);
    if (g_setT > 0.01f) {
        a.mouse = realMouse;         // 面板自己要用真实鼠标做 hover
        // 遮罩和卡片始终可点（模态），按钮才受动画裁剪
        { Hit bg; bg.r = Rc(0, 0, W, H); bg.kind = HK_SET_BG; a.hits.push_back(bg); }
        { Hit hc; hc.r = SettingsCardRect(W, H); hc.kind = HK_SET_CARD; a.hits.push_back(hc); }
        FillRect(Rc(0, 0, W, H), 0x000000, 0.20f * g_setT);   // 遮罩只淡入，不缩放
        PanelFx fx(g_setT, W * 0.5f, H * 0.5f);
        DrawSettings(W, H);
    }
}

/* ══════════════════════════════════════════
   动画
   ══════════════════════════════════════════ */
/** 真实经过的秒数（动画时间不能按固定 16ms 累加，否则帧率低时会被拉长） */
static float DeltaSeconds() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (last == 0) { last = now; return 0.016f; }
    float dt = (float)(now - last) / 1000.0f;
    last = now;
    if (dt > 0.10f) dt = 0.10f;      // 卡顿时别让动画跳太远
    return dt;
}

/** 把"每帧收敛 perFrame"换算成与帧率无关的系数，保证墙钟时长一致 */
static float Ease(float perFrame, float dt) {
    return 1.0f - powf(1.0f - perFrame, dt * 62.5f);
}

static void GoPlayer(int idx);  // 定义在后面
static void SwitchTrack(int delta);

/* ── 一起听：跨平台匹配（异步，别卡 UI） ── */
struct PendPlay {
    bool has = false;
    bool failed = false;
    std::string hash;
    long long mixId = 0, albumId = 0;
    std::wstring title, artist, err;
    int posMs = 0;
    bool playing = true;
};
static std::mutex g_ppMx;
static PendPlay     g_pp;
static std::string  g_matchKey;

static void TryMatchAsync(const TogetherEvent& ev) {
    std::string key = ev.song + "|" + ev.title;
    {
        std::lock_guard<std::mutex> lk(g_ppMx);
        if (key == g_matchKey) return;          // 同一首不重复匹配
        g_matchKey = key;
        g_pp = PendPlay{};
    }
    Logf("TOG", "跨平台匹配: %s - %s", ev.title.c_str(), ev.artist.c_str());
    std::string title = ev.title, artist = ev.artist;
    int pos = ev.posMs;
    bool play = ev.playing;
    std::thread([title, artist, pos, play]() {
        std::string h; std::wstring err;
        long long mix = 0, alb = 0;
        bool ok = MatchOnPlatform(g_source, title, artist, 0, h, mix, alb, err);
        std::lock_guard<std::mutex> lk(g_ppMx);
        g_pp.has = true;
        g_pp.failed = !ok;
        g_pp.hash = h;
        g_pp.mixId = mix;
        g_pp.albumId = alb;
        g_pp.err = err;
        g_pp.title = Widen(title);
        g_pp.artist = Widen(artist);
        g_pp.posMs = pos;
        g_pp.playing = play;
        Logf(ok ? "TOG" : "ERR", "匹配%s %s", ok ? "成功" : "失败", ok ? h.c_str() : Narrow(err).c_str());
    }).detach();
}
/* ── 一起听：接收与应用对方状态 ── */
static void ApplyRemoteState(const TogetherEvent& ev) {
    App& a = g_app;
    if (ev.song.empty()) { Logf("TOG", "收到状态但没有曲目 id，忽略"); return; }
    Logf("TOG", "收到状态 song=%.20s pos=%d play=%d", ev.song.c_str(), ev.posMs, ev.playing ? 1 : 0);

    int idx = -1;
    for (int i = 0; i < (int)a.songs.size(); i++) {
        if (a.songs[i].hash == ev.song) { idx = i; break; }   // hash 是 std::string
    }
    if (idx < 0) {
        Logf("TOG", "本地没有该曲目，开始跨平台匹配");
        TryMatchAsync(ev);
        return;
    }
    Logf("TOG", "本地命中 idx=%d 当前=%d", idx, a.current);

    // 期望位置：对端发来位置 + 已经过去的时间（UTC 毫秒，跨机器可比）
    long long now = (long long)TogetherNowMs();
    long long elapsed = (now > ev.stampMs) ? (now - ev.stampMs) : 0;
    if (elapsed > 60000) elapsed = 0;                 // 时钟差太大就只按发来的位置
    long long expectedLL = (long long)ev.posMs + (ev.playing ? elapsed : 0);
    int dur = (idx >= 0 && idx < (int)a.songs.size()) ? a.songs[(size_t)idx].durationSec : 0;
    if (dur > 0 && expectedLL > (long long)dur * 1000 - 1000) expectedLL = (long long)dur * 1000 - 1000;
    if (expectedLL < 0) expectedLL = 0;
    int expected = (int)expectedLL;

    if (idx != a.current) {
        Logf("TOG", "跟随切歌 %d -> %d", a.current, idx);
        GoPlayer(idx);
        g_pendingSeekMs = expected;                 // 打开后直接跳到对端位置
        if (!ev.playing) a.playing = false;
        return;
    }
    if (ev.playing != a.playing) {                  // 播/停跟对端
        a.playing = ev.playing;
        if (ev.playing) AudioPlay(); else AudioPause();
    }
    if (ev.playing) {
        int mine = (int)(a.pos * 1000.0);
        if (abs(mine - expected) > 800) {           // 事件点偏差超过 0.8 秒才纠
            AudioSeekMS(expected);
            a.pos = expected / 1000.0;
        }
    }
}

/* ── 一起听：把自己的状态推出去 ── */
static void PushLocalState(bool force) {
    if (!TogetherActive()) return;
    // 双方都推：房主推权威状态；跟随方推的只用来让房主知道"有人在听"
    App& a = g_app;
    if (a.current < 0 || a.current >= (int)a.songs.size()) return;

    static std::string  lastHash;
    static bool         lastPlaying = false;
    static ULONGLONG    lastPush = 0;
    static int          lastSeekMs = -1;

    const Song& s = a.songs[a.current];
    ULONGLONG now = GetTickCount64();
    int posMs = (int)(a.pos * 1000.0);

    // 只在关键动作上同步：切歌、播停、拖进度、加载状态变化
    bool changed = force || g_togForcePush ||
                   s.hash != lastHash || a.playing != lastPlaying;
    if (!changed) return;
    g_togForcePush = false;

    lastHash = s.hash;
    lastPlaying = a.playing;
    lastPush = now;
    lastSeekMs = posMs;
    TogetherPushState(s.hash,
                      g_source == 2 ? "qq" : (g_source == 1 ? "netease" : "kugou"),
                      posMs, a.playing,
                      std::string(s.title.begin(), s.title.end()),
                      std::string(s.artist.begin(), s.artist.end()));
}
static bool Animate() {
#ifndef FLORAL_NO_TOGETHER
    // ── 一起听：收事件、推状态 ──
    {
        TogetherEvent ev;
        while (TogetherPoll(ev)) {
            if (ev.kind == 1) {
                Logf("TOG", "检测到对端：%s", ev.nick.c_str());
                g_togHosting = false;
                g_togForcePush = true;          // 新加入：立刻把当前状态推过去
                TogetherSayHello();            // 回一个报到，让对方也看到我
                g_togPeer = Widen(ev.nick);
                g_togMyCode = Widen(TogetherMyCode());
                std::wstring hello = std::wstring(T(L"已和 ", L"Connected with ")) + g_togPeer + T(L" 连接成功，一起听", L"");
                TogToast(hello);
            } else if (ev.kind == 3) {
                g_togPeer.clear();
                TogToast(T(L"对方已断开", L"Peer disconnected"));
            } else if (ev.kind == 4) {
                TogToast(ev.err.empty() ? T(L"连接失败", L"Connection failed") : ev.err);
                TogetherStop();
            } else if (ev.kind == 5) {
                if (g_togIsHost) {
                    Logf("TOG", "对方播放失败，房主跳过");
                    TogToast(T(L"对方无该曲目，已跳到下一首", L"Peer lacks this track, skipped to next"));
                    SwitchTrack(1);
                }
            } else if (ev.kind == 2) {
                if (g_togIsHost) {
                    // 房主是权威，不跟随来访者的状态
                } else {
                    ApplyRemoteState(ev);
                }
            }
        }
        {   // 在场心跳：15 秒报一次昵称，保证双方都能看到对面
            static ULONGLONG lastHello = 0;
            ULONGLONG nowH = GetTickCount64();
            if (nowH - lastHello > 15000) {
                lastHello = nowH;
                TogetherSayHello();
            }
        }
        {   // 加载状态变了（卡缓冲 / 恢复）：同步一次
            static bool lastLoading = false;
            if (g_audioLoading != lastLoading) {
                lastLoading = g_audioLoading;
                g_togForcePush = true;
            }
        }
        PushLocalState(false);

        // 跨平台匹配结果
        {
            PendPlay pp;
            {
                std::lock_guard<std::mutex> lk(g_ppMx);
                if (!g_pp.has) { /* 没结果 */ }
                pp = g_pp;
                if (g_pp.has) g_pp.has = false;     // 只消费一次
            }
            if (pp.has) {
                if (pp.failed) {
                    TogToast(T(L"该曲目没有对应版本，已通知房主跳过", L"No matching version, asked the host to skip"));
                    if (g_audioOk) { AudioPause(); g_app.playing = false; }
                    if (!g_togIsHost) TogetherRequestSkip();
                } else {
                    Logf("TOG", "注入匹配结果 hash=%.20s 并播放", pp.hash.c_str());
                    // 注入到本地列表并播放
                    Song s;
                    s.title = pp.title;
                    s.artist = pp.artist;
                    s.hash = pp.hash;
                    s.mixId = pp.mixId;
                    s.albumId = pp.albumId;
                    g_app.songs.push_back(s);
                    g_covFlag.push_back(0);
                    g_layoutCount = -1;
                    int i2 = (int)g_app.songs.size() - 1;
                    GoPlayer(i2);
                    g_pendingSeekMs = pp.posMs;
                }
            }
        }
    }
#endif  // FLORAL_NO_TOGETHER
    App& a = g_app;
    bool need = false;
    const float dt = DeltaSeconds();

    // 登录 / 导出状态机（后台线程只写状态 UI 相关的都在这边做）
    {
        int  lgState = 0, expState = 0, expCount = 0;
        std::string tgTok, tgUid;
        std::wstring lgMsg, expMsg;
        {
            std::lock_guard<std::mutex> lk(g_lgMx);
            lgState = g_lgState; expState = g_expState; expCount = g_expCount;
            tgTok = g_lgToken; tgUid = g_lgUserid;
            lgMsg = g_lgMsg; expMsg = g_expMsg;
        }

        if (g_loginRunning && lgState == 3) {
            g_loginRunning = false;
            bool changed = (!g_userid.empty() && g_userid != tgUid);
            WriteSession(tgUid, tgTok);
            g_userid = tgUid;
            g_token = tgTok;
            g_nickname.clear();
            g_vipType = 0;
            g_loginState = 1;                     // 登录成功，有效
            g_loginAsked = false;
            ClearCache();                         // 跨平台时缓存不通用
            g_covFlag.assign(g_app.songs.size(), 0);
            g_layoutCount = -1;
            g_layoutWidth = -1;
            SettingsToast(changed ? T(L"已切换账号，正在拉取歌单…", L"Account switched, fetching playlist…") : T(L"登录成功，正在拉取歌单…", L"Signed in, fetching playlist…"));
            StartExport(tgUid, tgTok);
            CheckLoginAsync();                    // 拉取昵称
            need = true;
        } else if (g_loginRunning && lgState == 4) {
            g_loginRunning = false;
            SettingsToast(lgMsg.empty() ? T(L"登录失败", L"Sign-in failed") : lgMsg.c_str());
            need = true;
        }

        if (expState == 2) {
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_expState = 0;
            g_bootRefreshPending = false;
            InitData();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers();
            g_covFlag.assign(g_app.songs.size(), 0);
            g_layoutCount = -1;
            g_layoutWidth = -1;
            wchar_t buf[64];
            swprintf(buf, 64, T(L"歌单已更新（%d 首）", L"Playlist updated (%d songs)"), expCount);
            SettingsToast(buf);
            need = true;
        } else if (expState == 3) {
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_expState = 0;
            g_bootRefreshPending = false;
            Logf("APP", "歌单拉取失败: %s", Narrow(expMsg).c_str());
            SettingsToast(expMsg.empty() ? T(L"拉取歌单失败", L"Failed to fetch playlist") : expMsg.c_str());
            need = true;
        }
    }

    // 空闲时预热"上一首/下一首"的封面位图，切歌那一帧就不用现场建位图（会掉一帧）
    if (a.page == Page::Player && a.current >= 0 && g_textPhase == 0 &&
        !g_enterOn && g_switchT >= 1.0f) {
        static ULONGLONG lastNeighbor = 0;
        static int       turn = 0;
        ULONGLONG nowN = GetTickCount64();
        if (nowN - lastNeighbor > 150) {
            lastNeighbor = nowN;
            int cnt = (int)a.songs.size();
            if (cnt > 1) {
                int delta = (turn++ % 2) ? 1 : -1;
                int ni = (a.current + delta + cnt) % cnt;
                const Song& ns = a.songs[(size_t)ni];
                if (!ns.cover.empty() && !CoverEntry(ns.cover)) {
                    // 还没解好，下一轮再试
                }
            }
        }
    }

    // 播放/暂停图标：几何形变参数朝目标逼近（可被打断，不会跳）
    {
        float target = a.playing ? 1.0f : 0.0f;
        if (fabsf(g_playIconMix - target) > 0.0005f) {
            float step = dt / 0.16f;                 // 160ms 走完
            if (g_playIconMix < target) g_playIconMix = (std::min)(target, g_playIconMix + step);
            else                        g_playIconMix = (std::max)(target, g_playIconMix - step);
            need = true;
        } else {
            g_playIconMix = target;
        }
    }

    // 播放页文字：切歌先滑出（旧文字），再滑入（新文字）
    {
        static int  lastCur   = -1;
        static Page lastPage2 = Page::Grid;

        const bool changed = (a.current != lastCur) || (a.page != lastPage2);
        if (changed) {
            const bool songChanged = (a.current != lastCur);
            lastCur = a.current;
            lastPage2 = a.page;

            if (a.page != Page::Player || g_exitOn) {
                // 离开播放页：文字保持静态，绝不能在退场时又播一遍进场
                g_textPhase = 0;
                g_textT = 0.0f;
            } else {
                // 抓一份旧文字的副本，退场时还要画
                g_outTitle  = g_curTitle;
                g_outArtist = g_curArtist;
                g_outAlbum  = g_curAlbum;
                g_textPhase = (songChanged && !g_outTitle.empty()) ? 1 : 2;   // 有旧文字才退场
                g_textT = 0.0f;
            }
        } else if (g_textPhase == 1) {
            g_textT += dt;
            if (g_textT >= 0.20f) {                 // 退场结束，接滑入
                g_textPhase = 2;
                g_textT = 0.0f;
            }
        } else if (g_textPhase == 2) {
            g_textT += dt;
            if (g_textT >= 0.42f) { g_textPhase = 0; g_textT = 0.0f; }
        }
        if (g_textPhase != 0) need = true;


        // 记下当前三行，供下次切歌当快照
        if (a.current >= 0 && a.current < (int)a.songs.size()) {
            const Song& cs = a.songs[(size_t)a.current];
            g_curTitle = cs.title; g_curArtist = cs.artist; g_curAlbum = cs.album;
        } else {
            g_curTitle.clear(); g_curArtist.clear(); g_curAlbum.clear();
        }
    }

    // toast 存活期间要一直重绘，否则淡入淡出不会动
    {
        ULONGLONG nowT2 = GetTickCount64();
        if (g_settingsMsgAt && nowT2 - g_settingsMsgAt < 5500) need = true;
        if (g_togMsgAt      && nowT2 - g_togMsgAt      < 6500) need = true;
    }

    if (g_gridEnter < 1.0f) {
        g_gridEnter += dt / 1.10f;
        if (g_gridEnter > 1.0f) g_gridEnter = 1.0f;
        need = true;
    }
    if (g_plEnter < 1.0f) {
        g_plEnter += dt / 0.30f;
        if (g_plEnter > 1.0f) g_plEnter = 1.0f;
        need = true;
    }

    DrainPlaylistSwap();

    // ── 歌单选择页：合并后台数据、惯性、悬停 ──
    if (g_plReady.load()) {
        std::vector<Playlist> got;
        { std::lock_guard<std::mutex> lk(g_plMx); got.swap(g_plPending); }
        g_plReady = false;
        if (!got.empty()) {
            a.playlists = std::move(got);
            SavePlaylists();
            g_plCovAsked.clear();
            a.plHover = -1;
            need = true;
        }
    }
    if (a.page == Page::Playlists) {
        // 选中进度
        if (a.plActive >= 0) {
            if (a.plSelT < 1.0f) {
                a.plSelT += dt / 0.34f;
                if (a.plSelT > 1.0f) a.plSelT = 1.0f;
                need = true;
            }
        } else if (a.plSelT > 0.0f) {
            a.plSelT -= dt / 0.24f;
            if (a.plSelT < 0.0f) a.plSelT = 0.0f;
            need = true;
        }

        if (!a.plDragging) {
            // 惯性
            if (a.plActive < 0) {
                a.plX += a.plVelX;
                a.plY += a.plVelY;
                a.plVelX *= 0.90f;
                a.plVelY *= 0.90f;
                if (fabsf(a.plVelX) < 0.25f) a.plVelX = 0.0f;
                if (fabsf(a.plVelY) < 0.25f) a.plVelY = 0.0f;
                if (a.plVelX != 0.0f || a.plVelY != 0.0f) need = true;
            }
            // 缓动追目标
            float k = Ease(0.25f, dt);
            a.plX += (a.plTargetX - a.plX) * k;
            a.plY += (a.plTargetY - a.plY) * k;
            if (fabsf(a.plX - a.plTargetX) < 0.35f) a.plX = a.plTargetX;
            if (fabsf(a.plY - a.plTargetY) < 0.35f) a.plY = a.plTargetY;
            if (a.plX != a.plTargetX || a.plY != a.plTargetY) need = true;
        }
        if (a.plActive < 0) PlClampPan();          // 选中时不受边界限制，要能正中

        int hov = -1;
        if (!a.plDragging && !g_settingsOpen) {
            float best = PL_D * 0.5f;
            for (int i = 0; i < (int)a.playlists.size(); i++) {
                float cx = 0, cy = 0;
                PlScreenPos(i, cx, cy);
                float dx = (float)a.mouse.x - cx, dy = (float)a.mouse.y - cy;
                float d2 = dx * dx + dy * dy;
                if (d2 <= best * best) { best = sqrtf(d2); hov = i; }
            }
            if (hov != a.plHover) { a.plHover = hov; need = true; }
        }
    }

    // 面板进出场进度
    {
        float k = Ease(0.24f, dt);
        auto step = [k](float& v, bool open) {
            float target = open ? 1.0f : 0.0f;
            v += (target - v) * k;
            if (fabsf(v - target) < 0.002f) v = target;
        };
        step(g_setT,  g_settingsOpen);
        step(g_qualT, g_qualOpen);
        step(g_volT,  g_volOpen);
        step(g_togT,  g_togOpen);
        if (g_setT > 0.0f || g_qualT > 0.0f || g_volT > 0.0f || g_togT > 0.0f) need = true;
    }

    // 开屏动画计时（按真实时间）
    if (!g_loadDone) {
        g_loadT += dt;
        need = true;
        // 歌单已经有了（或拉取结束），首屏封面解好，就可以进场
        bool dataReady = !g_app.songs.empty() || g_expState == 3;
        bool coversReady = CoversReadyForFirstScreen();
        bool refreshDone = (g_expState != 1) && !g_bootRefreshPending.load();

        if ((dataReady && coversReady && refreshDone) || g_loadT > 12.0f) {
            g_loadDone = true;
            g_introT = 0.0f;
            Logf("APP", "启动加载完成 t=%.2fs（封面就绪=%d）", g_loadT, coversReady ? 1 : 0);
        }
    } else if (!g_introDone) {
        g_introT += dt;
        need = true;
        // 开屏期间不再预解歌曲封面：歌单页只看歌单封面，歌曲封面等进歌单再说
        // logo 渐隐完就结束开屏，直接进歌单选择页（不再等封面、不再飞卡片）
        if (g_introT > SPLASH_OUT + 0.05f) {
            g_introDone = true;
            if (g_app.page == Page::Grid) { g_app.page = Page::Playlists; PlCenterContent(); }
            g_plEnter = 0.0f;                      // 歌单页淡入
            Logf("APP", "开屏结束 t=%.2fs", g_introT);
        }
    }

    // 平滑滚动：实际位置缓动追目标位置（帧率无关）
    {
        float d = a.scrollTarget - a.scroll;
        if (std::fabs(d) > 0.4f) {
            a.scroll += d * Ease(0.22f, dt);
            need = true;
        } else if (a.scroll != a.scrollTarget) {
            a.scroll = a.scrollTarget;
            need = true;
        }
    }

    // 滚动瞬时速度 -> 运动模糊强度
    {
        float moved = a.scroll - g_prevScroll;
        g_prevScroll = a.scroll;
        float v = std::fabs(moved) / ((dt > 1e-4f) ? dt : 0.016f);
        g_scrollVel += (v - g_scrollVel) * 0.45f;
        if (g_scrollVel < 3.0f) g_scrollVel = 0.0f;
    }

    float target = (a.page == Page::Player) ? 1.0f : 0.0f;
    float d = target - a.pageAnim;
    if (std::fabs(d) > 0.002f) { a.pageAnim += d * Ease(0.22f, dt); need = true; }
    else a.pageAnim = target;

    for (size_t i = 0; i < a.tileAnim.size(); i++) {
        float t = (a.page == Page::Grid && (int)i == a.hoverTile) ? 1.0f : 0.0f;
        float dd = t - a.tileAnim[i];
        if (std::fabs(dd) > 0.004f) { a.tileAnim[i] += dd * Ease(0.30f, dt); need = true; }
        else a.tileAnim[i] = t;
    }

    // 切歌动画 + 唱片转动
    if (g_switchT < 1.0f) {
        g_switchT += dt / 0.55f;
        if (g_switchT > 1.0f) g_switchT = 1.0f;
        need = true;
    }
    if (a.page == Page::Player && a.playing && !g_enterOn) {
        g_discAngle += dt * 55.0f;      // 约 9 秒一圈
        if (g_discAngle > 360.0f) g_discAngle -= 360.0f;
        need = true;
    }

    // 退播放页的转场计时
    if (g_exitOn) {
        g_exitT += dt / 2.0f;
        if (g_exitT >= 1.0f) { g_exitT = 1.0f; g_exitOn = false; }
        need = true;
    }

    // 进播放页的转场计时
    if (g_enterOn) {
        g_enterT += dt / 2.2f;
        if (g_enterT >= 1.0f) { g_enterT = 1.0f; g_enterOn = false; }
        // 分隔线等格子全飞走之后再长出来
        float lk = (g_enterT - 0.70f) / 0.20f;
        g_lineT = (lk < 0.0f) ? 0.0f : (lk > 1.0f ? 1.0f : lk);
        need = true;
    }

    // 分隔线：进播放页时从右往左伸出（转场结束后自己收尾）
    if (a.page == Page::Player) {
        if (!g_enterOn && g_lineT < 1.0f) {
            g_lineT += dt / 0.45f;
            if (g_lineT > 1.0f) g_lineT = 1.0f;
            need = true;
        }
    } else {
        g_lineT = 0.0f;
    }

    // 后台解析好的地址：回主线程开 MCI（MCI 必须在创建它的线程用）
    {
        int st = 0, ui = -1;
        std::wstring u;
        {
            std::lock_guard<std::mutex> lk(g_urlMx);
            st = g_urlState; u = g_urlReady; ui = g_urlIdx;
        }
        if (st == 2 && ui == a.current && !g_audioOk && !g_audioError) {
            {
                // 先把状态清掉：保证一个地址只尝试打开一次，绝不会每帧重试
                std::lock_guard<std::mutex> lk(g_urlMx);
                g_urlState = 0;
                g_urlReady.clear();
            }
            Trace("open  idx=%d url_ok=1", ui);
            BeginOpenAsync(u);                   // 后台开，UI 继续跑动画
        }
        // 后台 open 的结果：回到 UI 线程做收尾（播放状态只在这里改）
        {
            int r = g_openResult.exchange(0);
            if (r > 0) {
                AudioPlay();
                g_audioOk = true;
                StartFadeIn();
                g_audioLoading = false;
                if (g_pendingSeekMs >= 0) {          // 切音质 / 一起听：跳回目标位置
                    AudioSeekMS(g_pendingSeekMs);
                    a.pos = g_pendingSeekMs / 1000.0;
                    g_pendingSeekMs = -1;
                }
                Trace("open  ok idx=%d", a.current);
                PrefetchUrl((a.current + 1) % (int)a.songs.size());
            } else if (r < 0) {
                g_audioError = true;
                g_audioErrKind = 2;
                g_audioLoading = false;
                RecheckLoginOnFailure();
                std::string u8, err8;
                {
                    std::wstring u2 = g_openUrl;
                    int n = WideCharToMultiByte(CP_UTF8, 0, u2.c_str(), (int)u2.size(),
                                                nullptr, 0, nullptr, nullptr);
                    u8.resize((size_t)n);
                    WideCharToMultiByte(CP_UTF8, 0, u2.c_str(), (int)u2.size(),
                                        &u8[0], n, nullptr, nullptr);
                    std::wstring e = AudioLastError();
                    n = WideCharToMultiByte(CP_UTF8, 0, e.c_str(), (int)e.size(),
                                            nullptr, 0, nullptr, nullptr);
                    err8.resize((size_t)n);
                    WideCharToMultiByte(CP_UTF8, 0, e.c_str(), (int)e.size(),
                                        &err8[0], n, nullptr, nullptr);
                }
                FILE* lf = nullptr;
                _wfopen_s(&lf, (AssetDir() + L"\\url_debug.txt").c_str(), L"wb");
                if (lf) {
                    std::string d = "OPEN FAIL\nURL: " + u8 + "\nERR: " + err8 + "\n";
                    fwrite(d.data(), 1, d.size(), lf);
                    fclose(lf);
                }
                Trace("openFAIL");
            }
        }
        if (st == 3 && ui == a.current && !g_audioOk) {   // 只认自己那首的失败
            g_audioError = true;
            g_audioErrKind = 1;
            g_audioLoading = false;
            RecheckLoginOnFailure();
            std::lock_guard<std::mutex> lk(g_urlMx);
            g_urlState = 0;
        }
    }

    // 看门狗：MCI 说 open 成功、但一直不出位置 = 流是死的
    if (g_audioLoading && g_audioOk && AudioPosMS() <= 120 &&
        GetTickCount64() - g_loadingSince > 4000) {
        Trace("wd1 fire idx=%d", a.current);
        AudioClose();
        g_audioOk = false;
        g_audioError = true;
        g_audioLoading = false;
        RecheckLoginOnFailure();
        need = true;
    }

    // 看门狗：解析一直不回来（sidecar 挂了）也不能无限等
    if (g_audioLoading && !g_audioOk &&
        GetTickCount64() - g_loadingSince > 12000) {
        {
            std::lock_guard<std::mutex> lk(g_urlMx);
            g_urlState = 0;
        }
        g_audioError = true;
        g_audioLoading = false;
        RecheckLoginOnFailure();
        need = true;
    }

    // 后台跳转中也显示加载圈（流媒体要等下载追上目标位置）
    const bool seeking = AudioIsSeeking();
    if (seeking) {
        g_audioLoading = true;
        g_loadingMin = 0;
    }

    // 加载圈：转到能读到位置为止（并且至少显示 g_loadingMin 那么久）
    if (g_audioLoading) {
        bool ready = g_audioOk && AudioPosMS() > 120 && !seeking;
        if (ready && GetTickCount64() > g_loadingMin) {
            g_audioLoading = false;
        } else {
            g_spinAngle += dt * 7.0f;       // 大约 1 秒一圈
            if (g_spinAngle > 6.2831853f) g_spinAngle -= 6.2831853f;
            need = true;
        }
    }

    // 淡入淡出
    if (g_fadeDir != 0) {
        g_vol += g_fadeDir * (dt / (g_fadeDir > 0 ? 0.45f : 0.25f));
        if (g_vol > 1.0f) g_vol = 1.0f;
        if (g_vol < 0.0f) g_vol = 0.0f;
        ApplyVolume();

        if (g_fadeDir > 0 && g_vol >= 1.0f) {
            g_fadeDir = 0;
        } else if (g_fadeDir < 0 && g_vol <= 0.0f) {
            g_fadeDir = 0;
            if (g_pendingPick >= 0) {        // 换的是指定的一首
                int i = g_pendingPick;
                g_pendingPick = -1;
                LoadAudioFor(i);             // 里面会 StartFadeIn
            } else {
                int d = g_pendingDir;
                g_pendingDir = 0;
                if (d != 0) SwitchTrackNow(d);   // 淡出干净了才真换歌
            }
        }
        need = true;
    }

    if (a.page == Page::Player && a.playing && a.current >= 0 &&
        a.current < (int)a.songs.size()) {
        if (g_audioOk) {
            if (!g_seekDrag) {                 // 拖动时别被引擎位置顶回去
                int ms = AudioPosMS();
                if (ms >= 0) a.pos = ms / 1000.0;
                else         ms = (int)(a.pos * 1000.0);   // 解码器读不到位置（EOF）时沿用上次值，
                                                           // 这样下面的"位置不再前进"兜底才判断得到
                int len = AudioLenMS();
                int metaMs = a.songs[a.current].durationSec * 1000;

                // 解码器不报告长度（流式 MP3 常见）时退到元数据时长，
                // 否则 len=-1 会让下面判断永远不成立 —— 这就是"有时不自动连播"
                int endMs = (len > 0) ? len : metaMs;
                {   // 每首歌记一次解码器长度，方便排查"不自动连播"
                    static int loggedIdx = -1;
                    if (a.current != loggedIdx && ms > 2000) {
                        loggedIdx = a.current;
                        Logf("AUD", "曲目长度 idx=%d 解码器=%d ms 元数据=%d ms",
                             a.current, len, metaMs);
                    }
                }

                bool atEnd = false;
                if (endMs > 0 && ms >= 0) {
                    int margin = (len > 0) ? 150 : 1200;   // 元数据精度低，多留一点
                    atEnd = (ms >= endMs - margin);
                }

                // 兜底：位置 1.5 秒没再前进，且已经到 97% 以上，也认为放完了
                {
                    static int       lastMs  = -1;
                    static ULONGLONG lastAt  = 0;
                    ULONGLONG nowA = GetTickCount64();
                    if (ms != lastMs) { lastMs = ms; lastAt = nowA; }
                    else if (!atEnd && endMs > 0 && ms >= endMs * 97 / 100 &&
                             lastAt && nowA - lastAt > 1500) {
                        atEnd = true;
                    }
                }

                if (atEnd) {
                    // 同一首只触发一次：切歌有淡出延迟，期间条件仍然成立
                    static int autoDone = -1;
                    if (autoDone == a.current) { need = true; }
                    else {
                    autoDone = a.current;
                    Logf("AUD", "自动连播：ms=%d len=%d 元数据=%d", ms, len, metaMs);
                    bool follower = false;
#ifndef FLORAL_NO_TOGETHER
                    follower = TogetherActive() && !g_togIsHost;
#endif
                    if (follower) {
                        a.playing = false;         // 等房主推下一首，别自己切
                        AudioPause();
                    } else {
                        SwitchTrack(1);
                    }
                    }
                }
            }
        } else if (g_urlState != 1 && !g_audioError) {   // 解析中/失败都别假装在放
            a.pos += dt;
            if (a.pos >= a.songs[a.current].durationSec) {
                // 一起听里只有房主能切歌；访客停下来等房主推下一首
                bool follower = false;
#ifndef FLORAL_NO_TOGETHER
                follower = TogetherActive() && !g_togIsHost;
#endif
                if (follower) {
                    a.pos = (float)a.songs[a.current].durationSec;   // 停在末尾，别越界
                    a.playing = false;
                    AudioPause();
                } else {
                    SwitchTrack(1);            // 放完自动下一首（走飞盘动画）
                }
            }
        }
        need = true;
    }

    // 网格挤开动画：只缓动视口附近的块，视口外的直接到位
    if (g_draw.size() == g_target.size()) {
        if (g_layoutDirty) {
            g_layoutDirty = false;
            ++g_stampVal;
            if (g_stamp.size() != g_base.size()) g_stamp.assign(g_base.size(), 0);
            for (int i : g_vis)
                if (i >= 0 && i < (int)g_stamp.size()) g_stamp[(size_t)i] = g_stampVal;
            for (int i = 0; i < (int)g_draw.size() && i < (int)g_stamp.size(); i++) {
                if (g_stamp[(size_t)i] != g_stampVal)
                    g_draw[(size_t)i] = g_target[(size_t)i];
            }
            need = true;
        }

        for (int i : g_vis) {
            if (i < 0 || i >= (int)g_draw.size()) continue;
            TileBox& d = g_draw[(size_t)i];
            const TileBox& t = g_target[(size_t)i];
            float dx = t.x - d.x, dy = t.y - d.y;
            float dw = t.w - d.w, dh = t.h - d.h;
            if (std::fabs(dx) + std::fabs(dy) + std::fabs(dw) + std::fabs(dh) > 0.35f) {
                float k = Ease(0.24f, dt);
                d.x += dx * k; d.y += dy * k;
                d.w += dw * k; d.h += dh * k;
                need = true;
            } else {
                d = t;
            }
        }
    }
    return need;
}

/* ══════════════════════════════════════════
   初始化
   ══════════════════════════════════════════ */
static bool GfxInit() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_dw)))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_wic)))) return false;
    return true;
}

static void GfxRelease() {
    for (auto& e : g_fmts) if (e.f) e.f->Release();
    g_fmts.clear();
    for (auto& b : g_coverBmps) if (b.bmp) b.bmp->Release();
    g_coverBmps.clear();
    for (auto& c : g_coverSrcs) if (c.conv) c.conv->Release();
    g_coverSrcs.clear();
    for (int i = 0; i < 8; i++) {
        if (g_covBrush[i]) { g_covBrush[i]->Release(); g_covBrush[i] = nullptr; }
    }
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    if (g_wic) { g_wic->Release(); g_wic = nullptr; }
    if (g_dw) { g_dw->Release(); g_dw = nullptr; }
    if (g_d2d) { g_d2d->Release(); g_d2d = nullptr; }
}

/* ══════════════════════════════════════════
   托盘图标
   ══════════════════════════════════════════ */
#define WM_TRAYICON (WM_APP + 1)

static NOTIFYICONDATAW g_nid{};
static bool   g_trayAdded = false;
static HICON  g_trayIcon  = nullptr;

/* 自己画一个 32×32 图标：圆角方块 + 白色 F（和启动页一致） */
static HICON MakeTrayIcon(UINT32 bg, UINT32 fg) {
    const int S = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = S;
    bi.bV5Height      = -S;
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;
    HBITMAP dib = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { DeleteDC(hdc); return nullptr; }

    const float br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    const float fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    BYTE* px = (BYTE*)bits;

    // 圆角方块的范围
    const float x0 = 2.0f, y0 = 2.0f, x1 = (float)S - 2.0f, y1 = (float)S - 2.0f, rad = 8.0f;
    const float fx0 = 10.0f, fy0 = 8.0f, fx1 = 22.0f, fy1 = 24.0f;   // F 的外框
    const float stemW = 4.0f, barH = 3.6f;

    auto inRoundRect = [&](float x, float y) {
        float cx = (x < x0 + rad) ? x0 + rad : ((x > x1 - rad) ? x1 - rad : x);
        float cy = (y < y0 + rad) ? y0 + rad : ((y > y1 - rad) ? y1 - rad : y);
        float dx = x - cx, dy = y - cy;
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        return (dx * dx + dy * dy) <= rad * rad || (cx > x0 + rad - 0.01f && cx < x1 - rad + 0.01f) ||
               (cy > y0 + rad - 0.01f && cy < y1 - rad + 0.01f);
    };
    auto inF = [&](float x, float y) {
        if (x >= fx0 && x < fx0 + stemW && y >= fy0 && y < fy1) return true;                  // 竖
        if (x >= fx0 && x < fx1 && y >= fy0 && y < fy0 + barH) return true;                   // 上横
        if (x >= fx0 && x < fx1 - 3.0f && y >= fy0 + 6.4f && y < fy0 + 6.4f + barH) return true; // 中横
        return false;
    };

    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            int coverBg = 0, coverFg = 0;
            const int SS = 4;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float px1 = (float)x + (sx + 0.5f) / SS;
                    float py1 = (float)y + (sy + 0.5f) / SS;
                    if (inRoundRect(px1, py1)) { coverBg++; if (inF(px1, py1)) coverFg++; }
                }
            }
            float cb = coverBg / (float)(SS * SS);
            float cf = coverFg / (float)(SS * SS);
            if (cb <= 0.0f) continue;

            float r = br, g = bgc, b = bb;
            if (cf > 0.0f) {                       // 文字盖在底色上
                float k = cf / cb;
                if (k > 1.0f) k = 1.0f;
                r = br + (fr - br) * k;
                g = bgc + (fgc - bgc) * k;
                b = bb + (fb - bb) * k;
            }
            BYTE* p = px + ((size_t)y * S + x) * 4;
            p[0] = (BYTE)(b + 0.5f);
            p[1] = (BYTE)(g + 0.5f);
            p[2] = (BYTE)(r + 0.5f);
            p[3] = (BYTE)(cb * 255.0f + 0.5f);
        }
    }

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = dib;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(dib);
    DeleteObject(mask);
    DeleteDC(hdc);
    return icon;
}

static void UpdateTrayIcon() {
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
    g_trayIcon = MakeTrayIcon(UI::BTN_CLOSE, 0xFFFFFF);
    if (g_trayAdded) {
        g_nid.hIcon = g_trayIcon;
        g_nid.uFlags = NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

static void AddTrayIcon(HWND hwnd) {
    if (g_trayAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    if (!g_trayIcon) g_trayIcon = MakeTrayIcon(UI::BTN_CLOSE, 0xFFFFFF);
    g_nid.hIcon = g_trayIcon;
    wcscpy_s(g_nid.szTip, L"Floral");
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayAdded = (ok != FALSE);
    Logf("APP", "托盘图标 = %d", ok ? 1 : 0);
}

static void RemoveTrayIcon() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

static void ShowMainWindow(HWND hwnd) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}
/* ══════════════════════════════════════════
   交互
   ══════════════════════════════════════════ */
static Hit* Pick(POINT p) {
    for (int i = (int)g_app.hits.size() - 1; i >= 0; i--) {
        if (Inside(g_app.hits[i].r, p)) return &g_app.hits[i];
    }
    return nullptr;
}

/** 顶部按钮：和点歌一样走转场动画，区别只是不重新开始播放 */
static void GoPlayerPage() {
    App& a = g_app;
    if (a.page == Page::Player) return;

    const int n = (int)a.songs.size();
    const bool hasSong = (a.current >= 0 && a.current < n);

    if (hasSong) {
        // 从"当前这首"的格子起飞，位置和鼠标点它时一致
        EnsureBase(a.width, n);
        const std::vector<TileBox>& src = (g_draw.size() == g_base.size()) ? g_draw : g_base;
        if (a.current < (int)src.size()) {
            const TileBox& b = src[(size_t)a.current];
            g_enterFrom = Rc(b.x, b.y + GRID_TOP - a.scroll, b.w, b.h);
        } else {
            g_enterFrom = Rc(a.width * 0.5f - 80, a.height * 0.5f - 80, 160, 160);
        }
        g_enterIdx = a.current;
    } else {
        g_enterIdx = -1;                // 还没选歌：只让其他格子散开，中间是空唱片
    }

    a.page = Page::Player;
    a.pageAnim = 1.0f;
    a.hoverTile = -1;
    g_switchT = 1.0f;                   // 不播切歌的甩盘动画
    g_switchFrom = -1;
    g_discAngle = 0.0f;                 // 进场从 0 度开始转，交接不会跳
    g_enterT = 0.0f;
    g_enterOn = true;
    g_exitOn = false;
    g_exitT = 0.0f;
    g_settingsOpen = false;
    g_alpha = 1.0f;
}

static void GoPlayer(int idx) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size()) return;

    // 记下这一格"现在画在屏幕上"的位置（含悬停放大），转场从它开始，不会先缩一下
    EnsureBase(a.width, (int)a.songs.size());
    const std::vector<TileBox>& src = (g_draw.size() == g_base.size()) ? g_draw : g_base;
    if (idx < (int)src.size()) {
        const TileBox& b = src[(size_t)idx];
        g_enterFrom = Rc(b.x, b.y + GRID_TOP - a.scroll, b.w, b.h);
    } else {
        g_enterFrom = Rc(a.width * 0.5f - 80, a.height * 0.5f - 80, 160, 160);
    }

    // 点的是正在放的那首：只是回到播放页，别重头开始
    const bool sameTrack = (idx == a.current) && (g_audioOk || g_audioLoading) && !g_audioError;

    g_textDir = (idx >= a.current) ? 1 : -1;   // 文字滑动的方向跟着点的那一格走
    a.current = idx;
    a.playing = true;
    a.page = Page::Player;
    a.pageAnim = 1.0f;         // 不走淡入淡出，交给转场
    a.hoverTile = -1;
    g_switchT = 1.0f;          // 进入播放页不播放切歌动画
    g_switchFrom = -1;

    g_enterIdx = idx;
    g_enterT = 0.0f;
    g_enterOn = true;
    g_audioRetry = 0;
    g_pendingSeekMs = -1;               // 换的是别的歌：从头开始

    if (sameTrack) {
        // 保持进度和角度；之前是暂停的就继续播
        if (g_audioOk) AudioPlay();
    } else {
        a.pos = 0.0;
        g_discAngle = 0.0f;
        if (g_audioOk && !g_audioLoading && g_fadeDir == 0) {
            // 上一首还在放：先淡出，音量归零后再接新的
            g_pendingPick = idx;
            g_fadeDir = -1;
        } else {
            LoadAudioFor(idx);
        }
    }
}

/* 回到歌单选择页 */
static void GoPicker() {
    if (g_app.page == Page::Playlists) return;
    g_app.page = Page::Playlists;
    g_app.pageAnim = 0.0f;
    g_app.plHover = -1;
    g_app.plActive = -1;
    g_app.plDragging = false;
    g_app.plVelX = g_app.plVelY = 0.0f;
    g_exitOn = false;
    g_enterOn = false;
    g_enterT = 1.0f;
    PlCenterContent();
    g_plEnter = 0.0f;
    g_mouseDirty = true;
}

static void GoGrid() {
    g_app.page = Page::Grid;
    g_app.pageAnim = 0.0f;
    g_gridEnter = 0.0f;          // 回选歌页：曲目从右侧飞入（和开屏原来那套一样）
    g_enterOn = false;
    g_enterT = 1.0f;
    g_exitT = 0.0f;
    g_exitOn = false;            // 不再用"网格从右飞入"那套
    g_settingsOpen = false;      // 退出播放页顺手把设置面板收掉
    g_seekDrag = false;
}

/** 播放页进度条的位置（绘制和命中都用它，避免两套坐标对不上） */
static void ProgressGeom(float& px, float& barY, float& barW) {
    App& a = g_app;
    const float lineY = UI::TITLE_H + 4.0f;
    const float dd = (std::min)(a.width * 0.42f, a.height * 0.62f);
    const float cx = dd * 0.10f;
    const float cy = (lineY + a.height) * 0.5f;
    px = (std::max)(a.width * 0.46f, cx + dd * 0.5f + 90.0f);
    const float pw = a.width - px - UI::PAD * 2;
    barW = (std::min)(pw, 380.0f);
    barY = cy + 10.0f;
}

/** 按横坐标定位到某首歌的某个时间点 */
static void SeekToX(float x) {
    App& a = g_app;
    if (a.current < 0 || a.current >= (int)a.songs.size()) return;
    float px, barY, barW;
    ProgressGeom(px, barY, barW);
    float r = (barW > 1.0f) ? (x - px) / barW : 0.0f;
    r = (std::max)(0.0f, (std::min)(1.0f, r));
    a.pos = a.songs[(size_t)a.current].durationSec * r;
    if (g_audioOk) AudioSeekMS((int)(a.pos * 1000.0));
}

static void OnClick(POINT p) {
    App& a = g_app;

    // 窗口按钮优先
    if (p.y < UI::TITLE_H) {
        for (int i = 0; i < UI::BTN_COUNT; i++) {
            if (Inside(BtnRect(i, a.width), p)) {
                HWND hwnd = GetActiveWindow();
                if (i == WB_MIN) ShowWindow(hwnd, SW_MINIMIZE);
                else if (i == WB_MAX) {
                    WINDOWPLACEMENT wpl = { sizeof(wpl) };
                    GetWindowPlacement(hwnd, &wpl);
                    ShowWindow(hwnd, wpl.showCmd == SW_MAXIMIZE ? SW_RESTORE : SW_MAXIMIZE);
                } else ShowWindow(hwnd, SW_HIDE);   // 关闭 = 收到托盘，不退出
                return;
            }
        }
        // 标题栏里还有一堆按钮，别整块吞掉
        if (Hit* hs = Pick(p)) {
            if (hs->kind == HK_SETTINGS)  g_settingsOpen = !g_settingsOpen;
            if (hs->kind == HK_PLAYER)    GoPlayerPage();
            if (hs->kind == HK_PICKER || hs->kind == HK_PL_PICK) GoPicker();
            if (hs->kind == HK_LOGIN_TIP) { g_settingsOpen = true; g_loginChoose = true; }
        }
        return;
    }

    Hit* h = Pick(p);
    if (g_volOpen && (!h || (h->kind != HK_VOL && h->kind != HK_VOL_TRACK))) g_volOpen = false;
    if (g_qualOpen && (!h || (h->kind != HK_QUAL && h->kind != HK_QUAL_ITEM))) g_qualOpen = false;
    if (g_togOpen && (!h || (h->kind < HK_TOG || h->kind > HK_TOG_CARD))) g_togOpen = false;
    if (!h) return;

    switch (h->kind) {
    case HK_TILE:  GoPlayer(h->index); break;
    case HK_BACK:  GoGrid(); break;
    case HK_PICKER:
    case HK_PL_PICK:
        GoPicker();
        break;
    case HK_SET_LANG_ZH:
        g_lang = Lang::Zh; SaveLang(); g_mouseDirty = true;
        break;
    case HK_SET_LANG_EN:
        g_lang = Lang::En; SaveLang(); g_mouseDirty = true;
        break;
    case HK_SETTINGS:  g_settingsOpen = !g_settingsOpen; break;
    case HK_VOL:
        if (g_volMuted) {                       // 先解除静音
            g_volMuted = false;
            g_userVol = g_volBeforeMute;
            ApplyVolume();
        }
        g_volOpen = !g_volOpen;
        if (g_volOpen) g_qualOpen = false;   // 两个面板别同时开
        break;
    case HK_SRC:
        if (h->index != g_source) {
            g_source = h->index;
            SaveSource();
            g_likesName = std::wstring(SourceTag()) + L"-likes.tsv";
    Logf("APP", "音源=%d 歌单文件=%s", g_source, Narrow(g_likesName).c_str());
            { std::lock_guard<std::mutex> lk(g_preMx); g_urlCache.clear(); }
            AudioClose();
            g_audioOk = false;
            g_audioError = false;
            g_token.clear();
            g_userid.clear();
            g_nickname.clear();
            g_loginState = 0;
            LoadSession();
            InitData();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers();
            g_covFlag.assign(g_app.songs.size(), 0);
            g_layoutCount = -1;
            g_layoutWidth = -1;
            SettingsToast(g_source ? T(L"已切到网易云", L"Switched to NetEase") : T(L"已切到酷狗", L"Switched to Kugou"));
            CheckLoginAsync();
        }
        break;
#ifndef FLORAL_NO_TOGETHER
    case HK_TOG:
        g_togOpen = !g_togOpen;
        if (!g_togOpen) g_togInput = false;
        break;
    case HK_TOG_INPUT:
        g_togInput = true;
        break;
        Logf("TOG", "点击创建房间");
    case HK_TOG_HOST: {
        Logf("TOG", "点击创建房间");
        if (g_togBusy) break;
        g_togBusy = true;
        g_togPeer.clear();
        TogToast(T(L"正在连接服务器…", L"Connecting…"));
        std::thread([]() {
            std::string code; std::wstring err;
            bool ok = TogetherHost(TogetherDefaultNick(true), code, err, -1);
            if (ok) {
                g_togMyCode = Widen(code);
                g_togHosting = true;
                g_togIsHost = true;
                Logf("TOG", "房间创建成功 code=%s", code.c_str());
                TogToast(T(L"房间已开，分享房间码给朋友吧", L"Room open, share the code"));
            } else {
                g_togHosting = false;
                Logf("ERR", "创建房间失败: %s", Narrow(err).c_str());
                TogToast(err);
            }
            g_togBusy = false;
        }).detach();
        break;
    }
    case HK_TOG_COPY: {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            size_t n2 = (g_togMyCode.size() + 1) * sizeof(wchar_t);
            HGLOBAL g2 = GlobalAlloc(GMEM_MOVEABLE, n2);
            if (g2) {
                void* p2 = GlobalLock(g2);
                memcpy(p2, g_togMyCode.c_str(), n2);
                GlobalUnlock(g2);
                SetClipboardData(CF_UNICODETEXT, g2);
            }
            CloseClipboard();
        }
        TogToast(T(L"房间码已复制", L"Room code copied"));
        break;
    }
    case HK_TOG_JOIN: {
        if (!g_togMyCode.empty() && g_togInputText == g_togMyCode) {
            TogToast(T(L"房间码，发给朋友以邀请加入", L"Send this code to invite"));
            break;
        }
        std::wstring err;
        std::string code(g_togInputText.begin(), g_togInputText.end());
        Logf("TOG", "尝试加入 code=%s", code.c_str());
        if (TogetherJoin(code, TogetherDefaultNick(false), err)) {
            g_togInput = false;
            TogToast(T(L"正在连接…", L"Connecting…"));
        } else {
            TogToast(err);
        }
        break;
    }
    case HK_TOG_CARD: break;
    case HK_TOG_LEAVE:
        TogetherStop();
        g_togPeer.clear();
        g_togMyCode.clear();
        g_togHosting = false;
        g_togOpen = false;
        TogToast(T(L"已退出一起听", L"Left the room"));
        break;
#endif  // FLORAL_NO_TOGETHER
    case HK_SCROLL: {
        g_scrollDrag = true;                       // 按下就跟着拖
        float trackH = g_app.height - UI::PAD - GRID_TOP;
        if (trackH > 40.0f) {
            float thumbH = (std::max)(36.0f, trackH * trackH / (trackH + g_app.maxScroll));
            float span = trackH - thumbH;
            if (span > 1.0f) {
                float t = (p.y - GRID_TOP - thumbH * 0.5f) / span;
                if (t < 0) t = 0; if (t > 1) t = 1;
                g_app.scrollTarget = t * g_app.maxScroll;
                g_hoverArmed = false;
                g_app.hoverTile = -1;
            }
        }
        break;
    }
    case HK_QUAL:
        g_qualOpen = !g_qualOpen;
        if (g_qualOpen) g_volOpen = false;
        break;
    case HK_QUAL_ITEM: {
        if (h->index >= 0 && h->index < kQualCount && h->index != g_quality) {
            g_quality = h->index;
            SaveQuality();
            { std::lock_guard<std::mutex> lk(g_preMx); g_urlCache.clear(); }   // 旧地址作废
            if (g_audioOk && !g_audioLoading && g_fadeDir == 0 && a.current >= 0) {
                g_pendingSeekMs = (int)(a.pos * 1000.0);   // 记住进度，重装后接着放
                g_pendingPick = a.current;      // 淡出后按新音质重装
                g_fadeDir = -1;
            }
        }
        g_qualOpen = false;
        break;
    }
    case HK_VOL_TRACK: {
        float tw = g_volTrack.right - g_volTrack.left;
        if (tw > 1.0f) {
            float k = ((float)p.x - g_volTrack.left) / tw;
            if (k < 0.0f) k = 0.0f;
            if (k > 1.0f) k = 1.0f;
            g_userVol = k;
            g_volMuted = (k <= 0.001f);
            ApplyVolume();
            SaveVolume();
        }
        g_volDrag = true;
        break;
    }
    case HK_LOGIN_TIP: g_settingsOpen = true; break;
    case HK_SET_BG:                          // 点遮罩：关面板
    case HK_SET_CLOSE:
        g_settingsOpen = false;
        g_loginChoose = false;
        if (g_loginRunning) {                  // 关面板就取消这次扫码
            g_loginRunning = false;
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_lgState = 0;
        }
        break;
    case HK_SET_CARD: break;                 // 卡片内部：吞掉，别穿透
    case HK_SET_CLEAR: ClearCache(); break;
    case HK_SET_THEME: {
        ThemeNext();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);   // 颜色变了要立刻重画
        UpdateTrayIcon();
        std::wstring msg = std::wstring(T(L"主题已切换：", L"Theme: ")) + ThemeName(ThemeCurrent());
        SettingsToast(msg.c_str());
        break;
    }
    case HK_SET_LOGIN: {
        // 重新登录：先清干净（缓存 + 两个平台的登录信息），再选平台
        ClearCache();
        DeleteFileW((AssetDir() + L"\\kugou-session.json").c_str());
        DeleteFileW((AssetDir() + L"\\netease-session.json").c_str());
        g_token.clear();
        g_userid.clear();
        g_nickname.clear();
        g_vipType = 0;
        g_loginState = 0;
        g_loginAsked = false;
        g_loginRunning = false;                 // 取消上一次可能还在跑的轮询
        { std::lock_guard<std::mutex> lk(g_lgMx); g_lgState = 0; g_lgMsg.clear(); }
        AudioClose();
        g_audioOk = false;
        g_audioError = false;
        SettingsToast(T(L"已清空缓存和登录信息", L"Cache and sign-in cleared"));
        g_loginChoose = !g_loginChoose;
        break;
    }
    case HK_LOGIN_PICK: {
        g_loginChoose = false;
        if (h->index != g_source) {              // 换平台：连带切数据源
            g_source = h->index;
            SaveSource();
            g_likesName = std::wstring(SourceTag()) + L"-likes.tsv";
    Logf("APP", "音源=%d 歌单文件=%s", g_source, Narrow(g_likesName).c_str());
            g_token.clear();
            g_userid.clear();
            g_nickname.clear();
            g_loginState = 0;
            LoadSession();
            InitData();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers();
            g_covFlag.assign(g_app.songs.size(), 0);
            g_layoutCount = -1;
            g_layoutWidth = -1;
        }
        StartRelogin();
        break;
    }
    case HK_PLAY:
        if (g_audioError) {               // 失败了再点一下就当重试
            LoadAudioFor(a.current);
            break;
        }
        a.playing = !a.playing;
        if (g_audioOk) { if (a.playing) AudioPlay(); else AudioPause(); }
        break;
    case HK_PREV:  SwitchTrack(-1); break;
    case HK_NEXT:  SwitchTrack(1); break;
    case HK_PROGRESS: {
        if (a.current < 0) break;
        g_seekDrag = true;          // 按住就开始拖
        g_lastSeek = GetTickCount64();
        SeekToX(p.x);
        break;
    }
    default: break;
    }
}

/* ══════════════════════════════════════════
   窗口
   ══════════════════════════════════════════ */
static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    ++g_paintCount;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT w = (UINT)(std::max)(1L, rc.right - rc.left);
    UINT h = (UINT)(std::max)(1L, rc.bottom - rc.top);
    g_app.width = (float)w;
    g_app.height = (float)h;

    if (!g_hwndRt) {
        g_d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(w, h)),
            &g_hwndRt);
    } else {
        D2D1_SIZE_U sz = g_hwndRt->GetPixelSize();
        if (sz.width != w || sz.height != h) g_hwndRt->Resize(D2D1::SizeU(w, h));
    }

    if (g_hwndRt) {
        g_alpha = 1.0f;
        g_hwndRt->BeginDraw();
        g_hwndRt->Clear(D2D1::ColorF(
            ((UI::BG >> 16) & 0xFF) / 255.0f,
            ((UI::BG >> 8) & 0xFF) / 255.0f,
            (UI::BG & 0xFF) / 255.0f, 1.0f));
        DrawUI(g_hwndRt);
        if (g_hwndRt->EndDraw() == D2DERR_RECREATE_TARGET) {
            g_hwndRt->Release();
            g_hwndRt = nullptr;
            g_rt = nullptr;
        } else {
            // 不在这里等垂直同步：DwmFlush 会把 UI 线程卡住，反而更顿
        }
    }
    EndPaint(hwnd, &ps);
}

static void SaveWindowRect(HWND hwnd);
static bool LoadWindowRect(int& x, int& y, int& w, int& h);
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 520;
        return 0;
    }

    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        RECT rc;
        GetClientRect(hwnd, &rc);

        if (!IsZoomed(hwnd)) {
            const int b = 6;
            bool L = p.x < b, Rr = p.x >= rc.right - b;
            bool T = p.y < b, B = p.y >= rc.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && Rr) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && Rr) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (Rr) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
        }

        if (p.y < UI::TITLE_H) {
            float btnLeft = rc.right - UI::BTN_W * UI::BTN_COUNT;
            if (p.x >= btnLeft) return HTCLIENT;          // 三个窗口按钮
            // 设置按钮也得是 HTCLIENT，否则会被当成标题栏拖拽，点不动
            float gearX = BtnRect(0, (float)rc.right).left - 40.0f;
            if (p.x >= gearX - 4.0f && p.x <= gearX + 36.0f) return HTCLIENT;
            float pbX = gearX - 44.0f;
            if (p.x >= pbX - 4.0f && p.x <= pbX + 36.0f) return HTCLIENT;
            // 回歌单页按钮（唱片按钮左边再 44px）：同样要是 HTCLIENT，
            // 否则会落进上面的 HTCAPTION 分支，被当成拖窗口
            float pickX = pbX - 44.0f;
            if (p.x >= pickX - 4.0f && p.x <= pickX + 36.0f) return HTCLIENT;
            if (g_loginState == 2 || g_loginState == 3) {
                D2D1_RECT_F pill = LoginPillRect((float)rc.right);
                if (p.x >= pill.left - 4.0f && p.x <= pill.right + 4.0f) return HTCLIENT;
            }
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_NCMOUSEMOVE:
        if (g_app.mouse.x != -1 || g_app.mouse.y != -1) {
            g_app.mouse = { -1, -1 };
            g_mouseDirty = true;
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (g_app.page == Page::Playlists && g_app.plDragging && !g_settingsOpen) {
            POINT mp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            float dx = (float)(mp.x - g_app.plDragStartMouse.x);
            float dy = (float)(mp.y - g_app.plDragStartMouse.y);
            if (fabsf(dx) > 5.0f || fabsf(dy) > 5.0f) g_app.plDragMoved = true;
            g_app.plX = g_app.plDragStartX + dx;
            g_app.plY = g_app.plDragStartY + dy;
            g_app.plTargetX = g_app.plX;
            g_app.plTargetY = g_app.plY;
            g_app.plVelX = g_app.plVelX * 0.6f + dx * 0.004f;
            g_app.plVelY = g_app.plVelY * 0.6f + dy * 0.004f;
            PlClampPan();
            g_mouseDirty = true;
            return 0;
        }
        if (g_scrollDrag && g_app.page == Page::Grid && !g_settingsOpen) {
            float trackH = g_app.height - UI::PAD - GRID_TOP;
            if (trackH > 40.0f) {
                float thumbH = (std::max)(36.0f, trackH * trackH / (trackH + g_app.maxScroll));
                float span = trackH - thumbH;
                if (span > 1.0f) {
                    POINT mp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                    float t = (mp.y - GRID_TOP - thumbH * 0.5f) / span;
                    if (t < 0) t = 0; if (t > 1) t = 1;
                    g_app.scrollTarget = t * g_app.maxScroll;
                    g_hoverArmed = false;
                    g_app.hoverTile = -1;
                    g_mouseDirty = true;
                }
            }
            return 0;
        }
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (g_volDrag) {                        // 拖动音量：直接跟着走
            float tx = g_volTrack.left, tw = g_volTrack.right - g_volTrack.left;
            if (tw > 1.0f) {
                float k = ((float)mx - tx) / tw;
                if (k < 0.0f) k = 0.0f;
                if (k > 1.0f) k = 1.0f;
                g_userVol = k;
                g_volMuted = (k <= 0.001f);
                ApplyVolume();
                SaveVolume();
            }
        }
        if (g_seekDrag) {                       // 拖动进度条：跟着鼠标走
            ULONGLONG now = GetTickCount64();
            if (now - g_lastSeek > 60) {        // 别把 MCI 打爆
                g_lastSeek = now;
                SeekToX((float)mx);
            }
        }
        if (g_app.mouse.x != mx || g_app.mouse.y != my) {
            g_app.mouse.x = mx;
            g_app.mouse.y = my;
            g_hoverArmed = true;      // 鼠标动了，恢复悬停放大
            g_mouseDirty = true;      // 不直接重绘，交给定时器节流
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.mouse = { -1, -1 };
        g_app.hoverTile = -1;
        g_mouseDirty = true;
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_app.page == Page::Playlists && p.y > (LONG)UI::TITLE_H && !g_settingsOpen) {
            // 只有顶栏以下、且设置面板没开时才拖动；
            // 面板打开时鼠标事件必须走 OnClick，否则整个面板都点不动
            g_app.plDragging = true;
            g_app.plDragMoved = false;
            g_app.plDragStartMouse = p;
            g_app.plDragStartX = g_app.plX;
            g_app.plDragStartY = g_app.plY;
            g_app.plVelX = g_app.plVelY = 0.0f;
            SetCapture(hwnd);
            return 0;
        }
        OnClick(p);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_app.page == Page::Playlists && g_app.plDragging && !g_settingsOpen) {
            g_app.plDragging = false;
            ReleaseCapture();
            if (g_app.plDragMoved) {
                g_app.plActive = -1;                // 拖动即取消选中
                g_app.plTargetX = g_app.plX;
                g_app.plTargetY = g_app.plY;
            } else {
                // 当成点击：找鼠标下的那个圆
                float best = PL_D * 0.5f;
                int hit = -1;
                for (int i = 0; i < (int)g_app.playlists.size(); i++) {
                    float cx = 0, cy = 0;
                    PlScreenPos(i, cx, cy);
                    float dx = (float)g_app.mouse.x - cx, dy = (float)g_app.mouse.y - cy;
                    float d2 = dx * dx + dy * dy;
                    if (d2 <= best * best) { best = sqrtf(d2); hit = i; }
                }
                // 展开状态：整张卡片都能点，点哪都进歌单；卡片外则收起
                if (g_app.plActive >= 0) {
                    POINT mp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                    D2D1_RECT_F card = PlCardTargetRect();
                    bool inCard = (mp.x >= card.left - 6 && mp.x <= card.right + 6 &&
                                   mp.y >= card.top - 6 && mp.y <= card.bottom + 6);
                    if (inCard && g_app.plSelT > 0.9f) {
                        SelectPlaylistAsync(g_app.plActive);
                    } else {
                        g_app.plActive = -1;
                        g_app.plTargetX = 0.0f;
                        g_app.plTargetY = 0.0f;
                    }
                    g_mouseDirty = true;
                    return 0;
                }

                if (hit >= 0) {
                    if (g_app.plActive == hit && g_app.plSelT > 0.9f) {
                        SelectPlaylistAsync(hit);   // 再点一次：进入歌单
                    } else {
                        // 第一次：选中 + 把这张挪到窗口正中
                        g_app.plActive = hit;
                        g_app.plSelT = 0.0f;
                        if (g_plCellsFor != (int)g_app.playlists.size())
                            BuildPlCells((int)g_app.playlists.size());
                        // 卡片以窗口中心为目标展开，视角保持不动
                        g_app.plTargetX = 0.0f;
                        g_app.plTargetY = 0.0f;
                        g_app.plVelX = g_app.plVelY = 0.0f;
                    }
                    g_mouseDirty = true;
                }
            }
            return 0;
        }
        if (g_seekDrag) g_togForcePush = true;      // 拖完进度条：同步一次位置
        g_seekDrag = false;
        g_volDrag = false;
        g_scrollDrag = false;
        return 0;

    case WM_CAPTURECHANGED:
        g_seekDrag = false;
        return 0;

    case WM_MOUSEWHEEL: {
        if (g_app.page != Page::Grid) return 0;
        float delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 90.0f;
        g_app.scrollTarget -= delta;
        if (g_app.scrollTarget < 0) g_app.scrollTarget = 0;
        if (g_app.scrollTarget > g_app.maxScroll) g_app.scrollTarget = g_app.maxScroll;
        g_hoverArmed = false;         // 滚动期间和结束后都不弹开
        g_app.hoverTile = -1;
        g_mouseDirty = true;          // 交给定时器推动动画
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_settingsOpen) {
                g_settingsOpen = false;
            } else if (g_togOpen) {
                g_togOpen = false;
            } else if (g_app.page == Page::Player) {
                GoGrid();
            } else if (g_app.page == Page::Grid) {
                GoPicker();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // 选歌界面：方向键 / PgUp PgDn 滚动
        if (g_app.page == Page::Grid && !g_settingsOpen && !g_togOpen) {
            const float viewH = (std::max)(0.0f, (float)g_app.height - GRID_TOP - UI::PAD);
            float step = 0.0f;
            if (wp == VK_UP)        step = -110.0f;                    // 约一行
            else if (wp == VK_DOWN) step = 110.0f;
            else if (wp == VK_PRIOR) step = -viewH + 40.0f;            // PgUp：一屏
            else if (wp == VK_NEXT)  step = viewH - 40.0f;             // PgDn
            if (step != 0.0f) {
                g_app.scrollTarget += step;
                if (g_app.scrollTarget < 0) g_app.scrollTarget = 0;
                if (g_app.scrollTarget > g_app.maxScroll) g_app.scrollTarget = g_app.maxScroll;
                g_hoverArmed = false;
                g_app.hoverTile = -1;
                g_mouseDirty = true;
                return 0;
            }
        }
        // 一起听输入框：Ctrl+V 粘贴（自绘输入框没有系统编辑器，得自己读剪贴板）
        if (g_togOpen && g_togInput && wp == 'V' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (OpenClipboard(hwnd)) {
                HANDLE hd = GetClipboardData(CF_UNICODETEXT);
                if (hd) {
                    const wchar_t* src = (const wchar_t*)GlobalLock(hd);
                    if (src) {
                        for (const wchar_t* q = src; *q; q++) {
                            if (*q >= 32 && *q < 127 && g_togInputText.size() < 96)
                                g_togInputText += *q;
                        }
                        GlobalUnlock(hd);
                    }
                }
                CloseClipboard();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Ctrl+A 全选清空，方便重输
        if (g_togOpen && g_togInput && wp == 'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            g_togInputText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;

    case WM_CHAR: {
        if (g_togOpen && g_togInput) {
            wchar_t c = (wchar_t)wp;
            if (c == 8) {                                  // 退格
                if (!g_togInputText.empty()) g_togInputText.pop_back();
            } else if (c == 13) {                          // 回车加入
                if (!g_togMyCode.empty() && g_togInputText == g_togMyCode) {
                    TogToast(T(L"这是你自己的房间码，发给朋友让他加入", L"That is your own room code"));
                    return 0;
                }
                std::string code(g_togInputText.begin(), g_togInputText.end());
                std::wstring err;
                if (TogetherJoin(code, TogetherDefaultNick(false), err)) {
                    g_togInput = false;
                    TogToast(T(L"正在连接…", L"Connecting…"));
                } else {
                    TogToast(err);
                }
            } else if (c >= 32 && c < 127) {
                g_togInputText += c;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_TIMER: {

        bool need = Animate();
        if (g_mouseDirty) { g_mouseDirty = false; need = true; }
        if (g_coverDirty.exchange(false)) need = true;   // 后台解出封面，重画一次
        if (!need && CoverWorkPending()) need = true;    // 还有封面在排队/下载/解码
        size_t bmpsBefore = g_coverBmps.size();
        if (g_app.page == Page::Grid) PrewarmCovers(2);   // 只在选歌页预热
        if (g_coverBmps.size() != bmpsBefore) need = true;
        if (!IsWindowVisible(hwnd)) need = false;      // 收到托盘里就不用重绘了
        if (need) InvalidateRect(hwnd, nullptr, FALSE);
        if (g_showFps) {
            static ULONGLONG last = 0;
            ULONGLONG now = GetTickCount64();
            if (last == 0) last = now;
            if (now - last >= 1000) {
                wchar_t buf[64];
                swprintf(buf, 64, L"Floral  fps=%d", g_paintCount);
                SetWindowTextW(hwnd, buf);
                g_paintCount = 0;
                last = now;
            }
        }
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            ShowMainWindow(hwnd);
        } else if (lp == WM_RBUTTONUP) {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, T(L"显示 Floral", L"Show Floral"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, T(L"退出", L"Quit"));
            SetForegroundWindow(hwnd);
            int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            PostMessage(hwnd, WM_NULL, 0, 0);      // 让菜单正常收起
            if (cmd == 1) ShowMainWindow(hwnd);
            else if (cmd == 2) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        SaveWindowRect(hwnd);
#ifndef FLORAL_NO_TOGETHER
        TogetherStop();
#endif
        AudioClose();                       // 停设备、join 下载线程
        if (g_apiProc) {                    // 自己拉起来的接口服务跟着退出
            TerminateProcess(g_apiProc, 0);
            CloseHandle(g_apiProc);
            g_apiProc = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ══════════════════════════════════════════
   离屏渲染（开发用）
   ══════════════════════════════════════════ */
static bool SavePng(IWICBitmapSource* src, const std::wstring& path) {
    IWICStream* stream = nullptr;
    if (FAILED(g_wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        stream->Release();
        return false;
    }
    IWICBitmapEncoder* enc = nullptr;
    g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    enc->CreateNewFrame(&frame, &props);
    if (props) props->Release();
    frame->Initialize(nullptr);
    frame->WriteSource(src, nullptr);
    frame->Commit();
    enc->Commit();

    frame->Release();
    enc->Release();
    stream->Release();
    return true;
}

static bool RenderToPng(const std::wstring& out) {
    IWICBitmap* bmp = nullptr;
    if (FAILED(g_wic->CreateBitmap((UINT)g_app.width, (UINT)g_app.height,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)))
        return false;

    ID2D1RenderTarget* rt = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bmp, props, &rt))) {
        bmp->Release();
        return false;
    }

    rt->BeginDraw();
    rt->Clear(Col(UI::BG, 1.0f));
    g_alpha = 1.0f;
    DrawUI(rt);
    bool ok = SUCCEEDED(rt->EndDraw()) && SavePng(bmp, out);

    rt->Release();
    bmp->Release();
    return ok;
}

/* ── 记住窗口位置 ── */
static std::wstring WindowFile() { return LocalDir() + L"\\window.txt"; }

static void SaveWindowRect(HWND hwnd) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return;
    FILE* f = nullptr;
    _wfopen_s(&f, WindowFile().c_str(), L"wb");
    if (!f) return;
    fprintf(f, "%ld %ld %ld %ld", r.left, r.top, r.right - r.left, r.bottom - r.top);
    fclose(f);
}

/* 读上次的位置；落在屏幕外就忽略（拔掉副屏也要能开） */
static bool LoadWindowRect(int& x, int& y, int& w, int& h) {
    FILE* f = nullptr;
    _wfopen_s(&f, WindowFile().c_str(), L"rb");
    if (!f) return false;
    long a = 0, b = 0, c = 0, d = 0;
    int n = fscanf(f, "%ld %ld %ld %ld", &a, &b, &c, &d);
    fclose(f);
    if (n != 4 || c < 640 || d < 420 || c > 6000 || d > 4000) return false;

    RECT probe{ (LONG)a, (LONG)b, (LONG)(a + c), (LONG)(b + d) };
    HMONITOR mon = MonitorFromRect(&probe, MONITOR_DEFAULTTONULL);
    if (!mon) return false;                 // 那块屏幕已经不在了
    x = (int)a; y = (int)b; w = (int)c; h = (int)d;
    return true;
}
/* ══════════════════════════════════════════
   入口
   ══════════════════════════════════════════ */
/** 崩溃时把异常码和地址写下来，便于定位 */
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n ? std::wstring(buf, n) : L".") + L"\\KugouPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = nullptr;
    _wfopen_s(&f, (dir + L"\\crash.txt").c_str(), L"wb");
    if (f) {
        const EXCEPTION_RECORD* r = ep->ExceptionRecord;
        // 反查异常地址属于哪个模块，以及模块内偏移
        wchar_t mod[MAX_PATH] = L"?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)r->ExceptionAddress, &hm) && hm) {
            GetModuleFileNameW(hm, mod, MAX_PATH);
        }
        char s[512];
        sprintf(s, "code=0x%08X addr=%p flags=%u\nmodule=%ls rva=0x%llX\n",
                (unsigned)r->ExceptionCode, (void*)r->ExceptionAddress, r->ExceptionFlags,
                mod, (unsigned long long)((uintptr_t)r->ExceptionAddress - (uintptr_t)hm));
        fwrite(s, 1, strlen(s), f);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* 未捕获异常：写清楚是什么，别默默 abort */
static void OnTerminate() {
    std::string msg = "unknown";
    if (auto e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) { msg = ex.what(); }
        catch (...) { msg = "non-std"; }
    }
    Logf("ERR", "未捕获异常，terminate: %s", msg.c_str());
    std::abort();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(OnTerminate);
    timeBeginPeriod(1);              // 提高定时器精度，减少帧间隔抖动
    LogInit();
    ThemeLoad();
    Logf("APP", "==== Floral 启动 ====");
    {
        OSVERSIONINFOW vi = { sizeof(vi) };
        GetVersionExW(&vi);
        Logf("APP", "Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
    Logf("APP", "exe 目录 = %s", Narrow(AssetDir()).c_str());
    Logf("APP", "日志文件 = %s", Narrow(LogPath()).c_str());
    LoadVolume();
    LoadQuality();
    LoadSource();                    // 必须先定音源，LoadSession 才知道读哪个文件
    g_likesName = std::wstring(SourceTag()) + L"-likes.tsv";
    Logf("APP", "音源=%d 歌单文件=%s", g_source, Narrow(g_likesName).c_str());
    LoadSession();
    LoadLang();
    InitData();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers();          // 只补路径，不解码：启动不再拉起歌曲封面
    // 首屏只等歌单列表；歌曲列表和封面都等用户点进歌单时再拉
    if (!g_token.empty()) g_bootRefreshPending = true;
    std::thread([]() {
        EnsureSidecar();
        CheckLoginAsync();
        if (!g_token.empty()) {
            Logf("APP", "启动刷新歌单列表…");
            FetchPlaylistsAsync();
        }
    }).detach();
    if (!GfxInit()) {
        MessageBoxW(nullptr, T(L"图形初始化失败", L"Graphics init failed"), T(L"错误", L"Error"), MB_ICONERROR);
        return 1;
    }

    // 启动后台解码线程，并把所有封面排进队列
    StartDecoder();

    // 缺封面的歌按需下载
    g_covFlag.assign(g_app.songs.size(), 0);
    std::thread(CoverWorkerMain).detach();

    // 命令行
    std::wstring outPath, mode, page, audioTest;
    float enterT = -1.0f;
    float panelT = -1.0f;
    float coverT = -1.0f;
    float textT = -1.0f;
    int   textPhase = -1;
    float toastT = -1.0f;
    float iconT  = -1.0f;
    int hoverIdx = -1;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--render") {
            mode = L"render";
            if (i + 1 < argc && argv[i + 1][0] != L'-') outPath = argv[++i];
        } else if (a == L"--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a == L"--hover" && i + 1 < argc) {
            hoverIdx = _wtoi(argv[++i]);
        } else if (a == L"--page" && i + 1 < argc) {
            page = argv[++i];
        } else if (a == L"--fps") {
            g_showFps = true;
        } else if (a == L"--settings") {
            g_settingsOpen = true; g_setT = 1.0f;   // 截图用：直接打开设置面板
        } else if (a == L"--loading") {
            g_audioLoading = true;        // 截图用：固定显示加载圈
            g_loadingSince = GetTickCount64() - 1000;
            g_spinAngle = 0.8f;
        } else if (a == L"--trace") {
            g_traceOn = true;
        } else if (a == L"--choose") {
            g_loginChoose = true; g_settingsOpen = true; g_setT = 1.0f;
        } else if (a == L"--icont" && i + 1 < argc) {
            iconT = (float)_wtof(argv[++i]);        // 截图用：播放图标交叉淡化进度
        } else if (a == L"--toastt" && i + 1 < argc) {
            toastT = (float)_wtof(argv[++i]);      // 截图用：toast 透明度
        } else if (a == L"--textphase" && i + 1 < argc) {
            textPhase = _wtoi(argv[++i]);          // 截图用：1 退场 / 2 进场
        } else if (a == L"--textt" && i + 1 < argc) {
            textT = (float)_wtof(argv[++i]);       // 截图用：文字进场进度
        } else if (a == L"--covert" && i + 1 < argc) {
            coverT = (float)_wtof(argv[++i]);      // 截图用：封面淡入进度
        } else if (a == L"--panelt" && i + 1 < argc) {
            panelT = (float)_wtof(argv[++i]);     // 截图用：面板定格在某个进度
        } else if (a == L"--volopen") {
            g_volOpen = true; g_volT = 1.0f;   // 截图用
        } else if (a == L"--qualopen") {
            g_qualOpen = true; g_qualT = 1.0f;
        } else if (a == L"--enter" && i + 1 < argc) {
            enterT = (float)_wtof(argv[++i]);   // 截图用：定格进场某一时刻
        } else if (a == L"--loginstate" && i + 1 < argc) {
            g_loginState = _wtoi(argv[++i]);   // 截图用：伪造登录态 1/2/3
        } else if (a == L"--audiotest" && i + 1 < argc) {
            audioTest = argv[++i];
        }
    }
    if (argv) LocalFree(argv);

    // 音频引擎自检：打开、播 1.2 秒、把状态写到 audiotest.txt
    if (!audioTest.empty()) {
        std::wstring log;
        bool ok = AudioOpen(audioTest);
        log += ok ? L"open OK\n" : (L"open FAIL: " + AudioLastError() + L"\n");
        if (ok) {
            AudioPlay();
            Sleep(1200);
            wchar_t b[128];
            swprintf(b, 128, L"pos=%d len=%d playing=%d\n",
                     AudioPosMS(), AudioLenMS(), (int)AudioIsPlaying());
            log += b;

            // 再验一下 seek 在流上能不能用
            AudioSeekMS(60000);
            Sleep(900);
            swprintf(b, 128, L"after seek(60s) pos=%d\n", AudioPosMS());
            log += b;

            AudioClose();
        }
        wchar_t exe[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p(exe);
        size_t sp = p.find_last_of(L"\\/");
        if (sp != std::wstring::npos) p = p.substr(0, sp) + L"\\audiotest.txt";
        FILE* f = nullptr;
        _wfopen_s(&f, p.c_str(), L"wb");
        if (f) { fwrite(log.c_str(), 2, log.size(), f); fclose(f); }
        CoUninitialize();
        return 0;
    }

    if (!mode.empty()) {
        if (outPath.empty()) outPath = L"shot.png";
        g_app.width = 960.0f;
        g_app.height = 640.0f;
        g_snapUI = true;              // 截图直接到终态
        g_introDone = true;           // 截图跳过开屏动画
        g_lineT = 1.0f;               // 分隔线直接伸到底

        // 截图要完整：等后台把封面都解出来
        {
            size_t want = g_decRequested.size();
            for (int i = 0; i < 2000 && g_coverSrcs.size() < want; i++) {
                DrainDecoded();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            DrainDecoded();
        }

        if (page == L"playlists") {            // 画面用：歌单选择页
            g_app.page = Page::Playlists;
            PlCenterContent();
        } else if (page == L"empty") {         // 画面用：播放页但没选歌
            g_app.current = -1;
            g_app.page = Page::Player;
            g_app.pageAnim = 1.0f;
        } else if (page == L"player" && !g_app.songs.empty()) {
            g_app.current = 0;
            g_app.playing = true;
            g_app.pos = 62.0;
            g_app.page = Page::Player;
            g_app.pageAnim = 1.0f;
            if (enterT >= 0.0f) {              // 定格进场动画的某一帧
                EnsureBase(g_app.width, (int)g_app.songs.size());
                if (!g_base.empty()) {
                    const TileBox& b = g_base[0];
                    g_enterFrom = Rc(b.x, b.y + GRID_TOP - g_app.scroll, b.w, b.h);
                }
                g_enterIdx = 0;
                g_enterOn = true;
                g_enterT = enterT;
                g_lineT = (enterT > 0.70f) ? (enterT - 0.70f) / 0.20f : 0.0f;
            }
        } else if (hoverIdx >= 0 && hoverIdx < (int)g_app.tileAnim.size()) {
            g_app.hoverTile = hoverIdx;
            g_app.tileAnim[hoverIdx] = 1.0f;
            // 直接算到终态，截图才能看到"挤开"后的样子
            EnsureBase(g_app.width, (int)g_app.songs.size());
            // 鼠标要落在「基础布局」的这块上，否则命中会跑到邻居
            const TileBox& bb = g_base[(size_t)hoverIdx];
            g_app.mouse.x = (LONG)(bb.x + bb.w * 0.5f);
            g_app.mouse.y = (LONG)(GRID_TOP + bb.y + bb.h * 0.5f);
            EnsureTarget(g_app.width, (int)g_app.songs.size(), hoverIdx);
            g_draw = g_target;
        }

        g_noCoverFade = true;   // 截图：封面直接满，别拍成占位色
    if (coverT >= 0.0f) { g_noCoverFade = false; g_coverFadeOverride = coverT; }
    if (iconT >= 0.0f) {          // 截图：把形变参数定在某个值（0=播放，1=暂停）
        g_app.playing = (iconT > 0.5f);
        g_playIconMix = iconT;
    }
    if (toastT >= 0.0f) {
        g_toastOverride = toastT;
        g_settingsMsg = L"歌单已更新";     // 截图时造一条提示
        g_settingsMsgAt = GetTickCount64();
        g_togMsg = std::wstring(T(L"已和 ", L"Connected with ")) + L"朋友的电脑";
        g_togMsgAt = GetTickCount64();
    }
    if (textT >= 0.0f) {
        g_textT = textT;
        if (textPhase >= 0) g_textPhase = textPhase;
        if (g_textPhase == 1 && g_outTitle.empty() && !g_app.songs.empty()) {
            // 截图时没有真实切歌，人工造一份旧文字
            g_outTitle = L"上一首歌的名字";
            g_outArtist = L"上一位歌手";
            g_outAlbum = L"上一张专辑";
        }
    }
    if (panelT >= 0.0f) {   // 只给当前打开的那个面板定格进度
        if (g_settingsOpen) g_setT = panelT;
        if (g_qualOpen)     g_qualT = panelT;
        if (g_volOpen)      g_volT = panelT;
        if (g_togOpen)      g_togT = panelT;
    }
    bool ok = RenderToPng(outPath);
        StopDecoder();
        GfxRelease();
        timeEndPeriod(1);
        CoUninitialize();
        return ok ? 0 : 2;
    }

    // 无边框窗口
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SongListWnd";
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    HWND hwnd = CreateWindowExW(0, L"SongListWnd", L"Floral", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 640, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    int cornerPref = 2;
    DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));

    g_hwnd = hwnd;
    AddTrayIcon(hwnd);
    {
        int wx, wy, ww, wh;
        if (LoadWindowRect(wx, wy, ww, wh)) {
            SetWindowPos(hwnd, nullptr, wx, wy, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
            Logf("APP", "恢复窗口位置 %d,%d %dx%d", wx, wy, ww, wh);
        }
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 8, nullptr);   // 8ms 保持垂直同步

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hwndRt) g_hwndRt->Release();
    StopDecoder();
    GfxRelease();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
