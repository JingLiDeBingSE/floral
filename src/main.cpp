#include "app.h"
#include "resource.h"
#include "kugou.h"
#include "together.h"
#include "log.h"
#include "lang.h"
#include "audio.h"
#include "tiles.h"

#include <d2d1_1.h>
#include <d2d1effectauthor.h>
#include <d3dcompiler.h>
#include <wininet.h>
#include <winsock2.h>
#include <thread>
#include <mutex>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <thread>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <exception>
#include <optional>

App g_app;
static HWND g_hwnd = nullptr;      // main window (drives explicit redraws)

/* ══════════════════════════════════════════
   Drawing infrastructure
   ══════════════════════════════════════════ */
static ID2D1Factory*          g_d2d    = nullptr;
static IDWriteFactory*        g_dw     = nullptr;
static IWICImagingFactory*    g_wic    = nullptr;
static ID2D1RenderTarget*     g_rt     = nullptr;
static ID2D1SolidColorBrush*  g_brush  = nullptr;
static ID2D1HwndRenderTarget* g_hwndRt = nullptr;
static ID2D1Effect*             g_blurFx  = nullptr;   // motion blur effect on the current render target
static ID2D1BitmapRenderTarget* g_layerRt = nullptr;   // offscreen layer for content in motion

/** Whole-layer opacity (for page transition fades) */
static float g_alpha = 1.0f;

/** Extra opacity on top of g_alpha, set around a group of draws such as a panel or a fading tile */
static float g_alphaMul = 1.0f;
static float g_plEnter  = 1.0f;   // playlist page fade-in 0→1
static float g_gridEnter = 1.0f;  // song grid entrance 0→1

/* Playlist page -> song page transition:
   0 idle, 1 wipe out bottom->top, 2 loading ring, 3 ring fade-out (then the fly-in starts) */
static int   g_plPhase  = 0;
static float g_plT      = 0.0f;
static float g_plSpin   = 0.0f;    // spinner angle accumulator
static bool  g_swapDone = false;   // playlist data arrived
static float g_plEnterX = 0.0f, g_plEnterY = 0.0f;   // where the card was clicked (origin of the dissolve)

/* Page curtain: covers the window with the background colour, swaps the page, then reveals.
   0 idle, 1 covering (old page out), 2 revealing (new page in) */
static int   g_curtainPhase  = 0;
static float g_curtainT      = 0.0f;
static int   g_curtainAction = 0;                    // 1 picker, 2 player
static bool  g_playerFromPicker = false;             // so leaving the player returns where we came from

/* Immediate page switches, defined further down; the curtain calls them */
static void GoGrid();
static void GoPickerNow();
static void GoPlayerPageNow(bool fromPicker);
static bool      g_scrollDrag = false;   // dragging the scrollbar

/** A new frame is needed. Input and background work set it, and Tick serves it at most once per display refresh */
static bool g_mouseDirty = false;

/* Common wrapper for panel enter/exit: fade + 0.96→1 scale around the anchor; while t is small, clicks are ignored */
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
        // Clip only the buttons inside the panel: they are still tiny right after it appears, so ignore them for now\n        if (t < 0.55f && g_app.hits.size() > hitMark) g_app.hits.resize(hitMark);
    }
};
static std::atomic<bool> g_coverDirty{ false };   // set by workers once a cover is decoded; Tick draws a frame for it

/** After scrolling, wait for the mouse to actually move before allowing hover zoom (so it does not pop open by itself) */
static bool g_hoverArmed = true;

/** Share of each frame's travel that the motion blur covers; 0.5 is the 180° shutter of film cameras */
static const float SHUTTER = 0.5f;

/** Motion blur vectors in DIPs for the current frame: song grid scrolling and playlist picker panning */
static D2D1_VECTOR_2F g_gridBlur = { 0.0f, 0.0f };
static D2D1_VECTOR_2F g_plBlur   = { 0.0f, 0.0f };
static float g_plLastX = 0.0f, g_plLastY = 0.0f;   // picker pan at the previous animation step

/* Debug: --fps shows the measured frame rate in the title bar */
static bool g_showFps = false;
static int  g_paintCount = 0;

/* Boot animation: logo+text fade in then out → tracks fly in from the right and settle → title bar appears */
static float g_introT = 0.0f;
static bool  g_introDone = false;
static std::atomic<bool> g_bootRefreshPending{ false };   // playlist still needs refreshing at startup
static bool  g_loadDone  = false;    // startup resources loaded (playlists + covers)
static float g_loadT     = 0.0f;

static const float SPLASH_IN       = 0.40f;   // logo fade-in
static const float SPLASH_HOLD_END = 0.75f;   // hold until this moment
static const float SPLASH_OUT      = 1.10f;   // fade-out ends (tracks start right after)
static const float INTRO_TILE_START = 1.10f;
static const float INTRO_STAGGER    = 0.05f;  // interval per tile
static const float INTRO_FLY        = 0.42f;  // flight duration per tile
static const float INTRO_BAR        = 0.85f;  // title bar starts appearing (relative to track start)

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

/* Millisecond clock for animation; GetTickCount64 advances in 15.6 ms steps, too coarse above 60 fps */
static ULONGLONG NowMs() {
    using namespace std::chrono;
    return (ULONGLONG)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* Cover palette: 8 Morandi gradient swatches (light → mid) */
struct CoverPair { UINT32 a, b; };
static const CoverPair COVERS[8] = {
    { 0xF9E1E8, 0xE2B3C2 },   // rose
    { 0xF3DCE6, 0xD2A8BE },   // mauve
    { 0xE6DDEB, 0xC3B2D2 },   // gray violet
    { 0xDCE6EF, 0xB2C4D8 },   // mist blue
    { 0xE3EBE0, 0xBACBB6 },   // sage
    { 0xEFE6D9, 0xD6C3A8 },   // sand
    { 0xF0DED5, 0xD3B3A2 },   // clay
    { 0xE9E3E9, 0xC0B0BE },   // blush
};

/* Cover cache keyed by file path: decoded pixels waiting for upload, and bitmaps on the current render target */
static std::unordered_map<std::wstring, IWICBitmapSource*> g_coverSrcs;

struct CoverBmp {
    ID2D1Bitmap* bmp = nullptr;
    ID2D1BitmapBrush* brush = nullptr;   // for rounded fills; avoids creating a Layer every frame
};
static std::unordered_map<std::wstring, CoverBmp> g_coverBmps;

/* Text formats keyed by weight and size; sizes are rounded to half a DIP, so animated sizes share a few formats */
static std::unordered_map<int, IDWriteTextFormat*> g_fmts;

static IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT weight) {
    const int halfDips = (int)lroundf(size * 2.0f);
    const int key = halfDips * 1000 + (int)weight;     // font weights are below 1000
    auto it = g_fmts.find(key);
    if (it != g_fmts.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    if (FAILED(g_dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, halfDips * 0.5f, L"zh-cn", &f)))
        return nullptr;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_fmts.emplace(key, f);
    return f;
}

/** Blend two colors by ratio */
static UINT32 MixColor(UINT32 a, UINT32 b, float t) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (UINT32)((r << 16) | (g << 8) | bl);
}

/** Build 8 placeholder gradients from the current theme (used when there is no cover) */
static ID2D1LinearGradientBrush* g_covBrush[8] = { nullptr };
static void BuildCoverBrushes(ID2D1RenderTarget* rt) {
    for (int i = 0; i < 8; i++) {
        if (g_covBrush[i]) { g_covBrush[i]->Release(); g_covBrush[i] = nullptr; }
    }
    if (!rt) return;

    for (int i = 0; i < 8; i++) {
        // light end oscillates between SELECT/ACCENT; dark end is pressed toward the base palette
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

/* Cover placeholder gradient brushes: 8, cached per render target to avoid rebuilding every frame */

/* Cover fade-in: records when each image first becomes drawable, fading 0→1 over 150ms */
static std::unordered_map<std::wstring, ULONGLONG> g_coverSeen;

static bool  g_noCoverFade = false;     // screenshot mode: no fade-in, straight to full
static float g_coverFadeOverride = -1.0f;  // screenshot mode: freeze fade progress
static bool  g_coverFading = false;     // a cover drawn in the last frame is still fading in

static void MarkCoverReady(const std::wstring& path) {
    if (path.empty()) return;
    g_coverSeen.emplace(path, NowMs());
}

static float CoverFade(const std::wstring& path) {
    if (g_coverFadeOverride >= 0.0f) return g_coverFadeOverride;
    if (g_noCoverFade || path.empty()) return 1.0f;
    auto it = g_coverSeen.find(path);
    if (it == g_coverSeen.end()) return 1.0f;        // not recorded: treat as ready long ago
    float t = (float)(NowMs() - it->second) / 150.0f;
    if (t >= 1.0f) return 1.0f;
    g_coverFading = true;
    return t;
}
static void Bind(ID2D1RenderTarget* rt) {
    if (g_rt == rt) return;
    g_rt = rt;
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    for (int i = 0; i < 8; i++) {
        if (g_covBrush[i]) { g_covBrush[i]->Release(); g_covBrush[i] = nullptr; }
    }
    // bitmaps/brushes are bound to the render target, so rebuild them when it changes
    for (auto& entry : g_coverBmps) {
        if (entry.second.brush) entry.second.brush->Release();
        if (entry.second.bmp) entry.second.bmp->Release();
    }
    g_coverBmps.clear();
    if (g_blurFx)  { g_blurFx->Release();  g_blurFx = nullptr; }
    if (g_layerRt) { g_layerRt->Release(); g_layerRt = nullptr; }
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

/* Single line of text, ellipsized when it overflows (used in playlist cards) */
static void TextEllipsis(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r,
                         UINT32 c, float a = 1.0f) {
    if (!f || !g_brush || !g_dw || s.empty()) return;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f,
                                      r.right - r.left, r.bottom - r.top, &layout)) || !layout)
        return;
    D2D1_POINT_2F origin = D2D1::Point2F(r.left, r.top);
    g_brush->SetColor(Col(c, a));

    // measure first: draw directly if it fits, only trim when it does not
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
   Motion blur
   ══════════════════════════════════════════ */
/* A custom Direct2D effect: every output pixel averages the input along a vector, which is what a
   camera records while content travels that vector with the shutter open (a box filter in time) */
static const GUID CLSID_MotionBlur      = { 0xad1e53dc, 0x36d7, 0x4ea4, { 0xa2, 0xd6, 0x55, 0x96, 0x10, 0xe7, 0xad, 0x8d } };
static const GUID GUID_MotionBlurShader = { 0x20924afc, 0x55b9, 0x4d80, { 0xa7, 0x6a, 0x14, 0xaf, 0x61, 0x3b, 0x33, 0xbf } };

static const char MOTION_BLUR_HLSL[] = R"(
Texture2D Input : register(t0);
SamplerState InputSampler : register(s0);
cbuffer Params : register(b0) { float2 stride; float taps; };

// TEXCOORD0: xy is the input position, zw the size of one output pixel in input coordinates
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv : TEXCOORD0) : SV_Target {
    float4 sum = 0;
    [loop] for (float i = 0.5; i < taps; i += 1.0)
        sum += Input.Sample(InputSampler, uv.xy + stride * i * uv.zw);
    return sum / taps;
}
)";

static const wchar_t MOTION_BLUR_XML[] = LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='Motion Blur'/>
  <Property name='Author' type='string' value='Floral'/>
  <Property name='Category' type='string' value='Blur'/>
  <Property name='Description' type='string' value='Averages the input along a vector'/>
  <Inputs><Input name='Source'/></Inputs>
  <Property name='Vector' type='vector2'>
    <Property name='DisplayName' type='string' value='Vector'/>
    <Property name='Default' type='vector2' value='(0.0, 0.0)'/>
  </Property>
</Effect>)";

static std::string g_blurShader;      // compiled pixel shader bytecode
static const float BLUR_MAX_TAPS = 64.0f;

class MotionBlurEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    static HRESULT CALLBACK Create(IUnknown** effect) {
        *effect = static_cast<ID2D1EffectImpl*>(new MotionBlurEffect());
        return S_OK;
    }

    /* Accessors for the Vector property (in pixels); D2D passes back the IUnknown returned by Create */
    static HRESULT CALLBACK SetVector(IUnknown* effect, const BYTE* data, UINT32 size) {
        if (size != sizeof(D2D1_VECTOR_2F)) return E_INVALIDARG;
        static_cast<MotionBlurEffect*>(static_cast<ID2D1EffectImpl*>(effect))->m_vector =
            *reinterpret_cast<const D2D1_VECTOR_2F*>(data);
        return S_OK;
    }
    static HRESULT CALLBACK GetVector(const IUnknown* effect, BYTE* data, UINT32 size, UINT32* actual) {
        if (actual) *actual = sizeof(D2D1_VECTOR_2F);
        if (!data || size < sizeof(D2D1_VECTOR_2F)) return E_NOT_SUFFICIENT_BUFFER;
        *reinterpret_cast<D2D1_VECTOR_2F*>(data) =
            static_cast<const MotionBlurEffect*>(static_cast<const ID2D1EffectImpl*>(effect))->m_vector;
        return S_OK;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1EffectImpl))
            *out = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform) || riid == __uuidof(ID2D1Transform) ||
                 riid == __uuidof(ID2D1TransformNode))
            *out = static_cast<ID2D1DrawTransform*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG n = --m_refs;
        if (n == 0) delete this;
        return n;
    }

    IFACEMETHODIMP Initialize(ID2D1EffectContext* context, ID2D1TransformGraph* graph) override {
        HRESULT hr = context->LoadPixelShader(GUID_MotionBlurShader,
                                              reinterpret_cast<const BYTE*>(g_blurShader.data()),
                                              (UINT32)g_blurShader.size());
        return SUCCEEDED(hr) ? graph->SetSingleTransformNode(this) : hr;
    }
    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) override {
        // About one tap per pixel of travel, each at the center of an equal step along the vector
        const float length = sqrtf(m_vector.x * m_vector.x + m_vector.y * m_vector.y);
        const float taps = (std::min)(BLUR_MAX_TAPS, (std::max)(1.0f, ceilf(length)));
        const float params[4] = { m_vector.x / taps, m_vector.y / taps, taps, 0.0f };
        return m_info->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(params), sizeof(params));
    }
    IFACEMETHODIMP SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }

    IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* info) override {
        m_info = info;
        m_info->AddRef();
        D2D1_INPUT_DESCRIPTION input = { D2D1_FILTER_MIN_MAG_MIP_LINEAR, 0 };
        HRESULT hr = info->SetInputDescription(0, input);
        return SUCCEEDED(hr) ? info->SetPixelShader(GUID_MotionBlurShader) : hr;
    }
    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* output, D2D1_RECT_L* inputs,
                                           UINT32 count) const override {
        if (count != 1) return E_INVALIDARG;
        inputs[0] = Reach(*output, m_vector.x, m_vector.y);      // pixel p reads from p to p + vector
        return S_OK;
    }
    IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L* inputs, const D2D1_RECT_L*, UINT32 count,
                                           D2D1_RECT_L* output, D2D1_RECT_L* opaque) override {
        if (count != 1) return E_INVALIDARG;
        *output = Reach(inputs[0], -m_vector.x, -m_vector.y);
        *opaque = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }
    IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L invalid, D2D1_RECT_L* output) const override {
        *output = Reach(invalid, -m_vector.x, -m_vector.y);
        return S_OK;
    }
    IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 1; }

private:
    ~MotionBlurEffect() { if (m_info) m_info->Release(); }

    /* r extended toward (dx, dy), plus one pixel for bilinear sampling */
    static D2D1_RECT_L Reach(D2D1_RECT_L r, float dx, float dy) {
        r.left   += (LONG)floorf((std::min)(dx, 0.0f)) - 1;
        r.top    += (LONG)floorf((std::min)(dy, 0.0f)) - 1;
        r.right  += (LONG)ceilf((std::max)(dx, 0.0f)) + 1;
        r.bottom += (LONG)ceilf((std::max)(dy, 0.0f)) + 1;
        return r;
    }

    ULONG          m_refs = 1;
    D2D1_VECTOR_2F m_vector = { 0.0f, 0.0f };
    ID2D1DrawInfo* m_info = nullptr;
};

/* Compiles the pixel shader with the system's d3dcompiler_47.dll and registers the effect.
   If either step fails, moving content is drawn without blur */
static void RegisterMotionBlur() {
    HMODULE dll = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    pD3DCompile compile = dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    ID3DBlob* code = nullptr;
    if (compile && SUCCEEDED(compile(MOTION_BLUR_HLSL, sizeof(MOTION_BLUR_HLSL) - 1, nullptr, nullptr, nullptr,
                                     "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr))) {
        g_blurShader.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        code->Release();
    }
    if (dll) FreeLibrary(dll);

    ID2D1Factory1* factory = nullptr;
    HRESULT hr = E_FAIL;
    if (!g_blurShader.empty() && SUCCEEDED(g_d2d->QueryInterface(IID_PPV_ARGS(&factory)))) {
        const D2D1_PROPERTY_BINDING bindings[] = {
            { L"Vector", MotionBlurEffect::SetVector, MotionBlurEffect::GetVector },
        };
        hr = factory->RegisterEffectFromString(CLSID_MotionBlur, MOTION_BLUR_XML, bindings, 1,
                                               MotionBlurEffect::Create);
        factory->Release();
    }
    if (FAILED(hr)) Logf("ERR", "运动模糊不可用（着色器编译或效果注册失败）");
}

static ID2D1RenderTarget* g_motionScreen = nullptr;   // target to return to while a motion layer is open
static D2D1_VECTOR_2F     g_motionPx = { 0.0f, 0.0f };

/* Content drawn between BeginMotionBlur and EndMotionBlur travels `blur` DIPs while the shutter is open.
   When that is long enough to see, it goes to an offscreen layer and reaches the screen through the effect */
static void BeginMotionBlur(D2D1_VECTOR_2F blur) {
    float dpiX = 96.0f, dpiY = 96.0f;
    g_rt->GetDpi(&dpiX, &dpiY);
    g_motionPx = D2D1::Vector2F(blur.x * dpiX / 96.0f, blur.y * dpiY / 96.0f);
    if (fabsf(g_motionPx.x) + fabsf(g_motionPx.y) < 0.5f) return;
    // Taps more than 2 px apart leave gaps that show as separate copies, so longer blur is shortened
    const float length = sqrtf(g_motionPx.x * g_motionPx.x + g_motionPx.y * g_motionPx.y);
    if (length > 2.0f * BLUR_MAX_TAPS) {
        g_motionPx.x *= 2.0f * BLUR_MAX_TAPS / length;
        g_motionPx.y *= 2.0f * BLUR_MAX_TAPS / length;
    }

    if (!g_blurFx) {
        ID2D1DeviceContext* dc = nullptr;
        if (SUCCEEDED(g_rt->QueryInterface(IID_PPV_ARGS(&dc)))) {
            dc->CreateEffect(CLSID_MotionBlur, &g_blurFx);
            dc->Release();
        }
    }
    // The layer has to match the screen target in pixel size and DPI, which both follow the window
    const D2D1_SIZE_U size = g_rt->GetPixelSize();
    if (g_layerRt) {
        const D2D1_SIZE_U have = g_layerRt->GetPixelSize();
        float layerDpiX = 0.0f, layerDpiY = 0.0f;
        g_layerRt->GetDpi(&layerDpiX, &layerDpiY);
        if (have.width != size.width || have.height != size.height || layerDpiX != dpiX || layerDpiY != dpiY) {
            g_layerRt->Release();
            g_layerRt = nullptr;
        }
    }
    if (!g_layerRt) g_rt->CreateCompatibleRenderTarget(&g_layerRt);
    if (!g_blurFx || !g_layerRt) return;

    g_motionScreen = g_rt;
    g_rt = g_layerRt;
    g_layerRt->BeginDraw();
    g_layerRt->Clear(D2D1::ColorF(0, 0, 0, 0));
}

static void EndMotionBlur() {
    if (!g_motionScreen) return;
    g_layerRt->EndDraw();
    g_rt = g_motionScreen;
    g_motionScreen = nullptr;

    ID2D1Bitmap* layer = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    if (SUCCEEDED(g_layerRt->GetBitmap(&layer)) && SUCCEEDED(g_rt->QueryInterface(IID_PPV_ARGS(&dc)))) {
        g_blurFx->SetInput(0, layer);
        g_blurFx->SetValue(0, g_motionPx);
        dc->DrawImage(g_blurFx);
    }
    if (dc) dc->Release();
    if (layer) layer->Release();
}

/* ══════════════════════════════════════════
   Grid layout (irregular tiles)
   ══════════════════════════════════════════ */
struct TileBox {
    float x, y, w, h;    // relative to the content origin (scroll not subtracted)
};

/* Three layouts:
 *   g_base   —— no hover
 *   g_target —— the hovered tile grown to 2×2, the tiles around it shrunk or moved to make room
 *   g_draw   —— current animated values, used for drawing
 */
static std::vector<TileBox> g_base, g_target, g_draw;
static std::vector<TileBox> g_drawVel;   // velocity of each g_draw value, DIPs per second
static float g_gridHeight = 0.0f;
static int   g_layoutCount = -1;
static float g_layoutWidth = -1.0f;
static int   g_layoutHover = -2;
static bool  g_layoutDirty = false;

/* The base and target layouts in cells; g_targetOwner gives the tile on each cell of the target, -1 when empty */
static TileLayout        g_tiles;
static std::vector<Tile> g_targetTiles;
static std::vector<int>  g_targetOwner;
static float g_cellSize = 140.0f;
static bool  g_snapUI = false;           // jump straight to the final state for offscreen screenshots

/* Vertical band index: each frame only touches the bands near the viewport instead of scanning all 2000 tiles */
static const float BAND_H = 220.0f;
static std::vector<std::vector<int>> g_tgtBands;

/* Tiles visible this frame (deduplicated with a stamp) */
static std::vector<int> g_vis;
static std::vector<uint32_t> g_stamp;
static uint32_t g_stampVal = 0;

static const float GRID_TOP = UI::TITLE_H + 18.0f;

static TileBox BoxOf(const Tile& t) {
    const float pitch = g_cellSize + UI::GAP;
    return { UI::PAD + t.x * pitch, t.y * pitch, t.w * pitch - UI::GAP, t.h * pitch - UI::GAP };
}

/** Split tiles into vertical bands so drawing only reads the relevant ones (reuses capacity to avoid reallocating on every relayout) */
static void BuildBands(const std::vector<TileBox>& boxes, std::vector<std::vector<int>>& bands) {
    if (boxes.empty()) { bands.clear(); return; }

    float maxY = 0.0f;
    for (const auto& b : boxes) maxY = (std::max)(maxY, b.y + b.h);
    int nb = (int)(maxY / BAND_H) + 1;

    if ((int)bands.size() != nb) bands.resize((size_t)nb);
    for (auto& v : bands) v.clear();          // clear keeps capacity

    for (int i = 0; i < (int)boxes.size(); i++) {
        const TileBox& b = boxes[(size_t)i];
        int b0 = (int)(b.y / BAND_H);
        int b1 = (int)((b.y + b.h - 0.01f) / BAND_H);
        if (b0 < 0) b0 = 0;
        if (b1 >= nb) b1 = nb - 1;
        for (int k = b0; k <= b1; k++) bands[(size_t)k].push_back(i);
    }
}

/* Adopts a target layout given in cells: its rects in DIPs, the tile on each cell, and the band index */
static void SetTarget(std::vector<Tile> tiles) {
    g_targetTiles = std::move(tiles);
    const int cols = g_tiles.Columns();
    int rows = 0;
    g_target.clear();
    for (const Tile& t : g_targetTiles) {
        g_target.push_back(BoxOf(t));
        rows = (std::max)(rows, t.y + t.h);
    }
    g_targetOwner.assign((size_t)(rows * cols), -1);
    for (int i = 0; i < (int)g_targetTiles.size(); i++) {
        const Tile& t = g_targetTiles[(size_t)i];
        for (int y = t.y; y < t.y + t.h; y++)
            for (int x = t.x; x < t.x + t.w; x++) g_targetOwner[(size_t)(y * cols + x)] = i;
    }
    g_gridHeight = rows * (g_cellSize + UI::GAP) - UI::GAP;
    BuildBands(g_target, g_tgtBands);
    g_layoutDirty = true;
    if (g_draw.size() != g_target.size()) {
        g_draw = g_target;
        g_layoutDirty = false;
    }
}

static void EnsureBase(float W, int count) {
    if (count == g_layoutCount && W == g_layoutWidth) return;
    int cols = (int)((W - UI::PAD * 2 + UI::GAP) / 150.0f);
    if (cols < 3) cols = 3;
    if (cols > 10) cols = 10;
    g_cellSize = (W - UI::PAD * 2 - UI::GAP * (cols - 1)) / cols;
    if (cols != g_tiles.Columns() || count != (int)g_tiles.Rest().size()) g_tiles = TileLayout(cols, count);

    g_base.clear();
    for (const Tile& t : g_tiles.Rest()) g_base.push_back(BoxOf(t));
    SetTarget(g_tiles.Rest());
    g_layoutCount = count;
    g_layoutWidth = W;
    g_layoutHover = -1;
}

/* Mouse position in grid cells, where each cell also owns half of the gaps around it; false beside or above the grid */
static bool GridCell(POINT p, float& cx, float& cy) {
    const float pitch = g_cellSize + UI::GAP;
    cx = ((float)p.x - UI::PAD + UI::GAP * 0.5f) / pitch;
    cy = ((float)p.y - GRID_TOP + g_app.scroll + UI::GAP * 0.5f) / pitch;
    return cx >= 0.0f && cy >= 0.0f && cx < (float)g_tiles.Columns();
}

/* The hovered tile grows over the cell under the mouse, and the tiles around it make room */
static void EnsureTarget(float W, int count, int hoverIdx) {
    EnsureBase(W, count);
    if (hoverIdx == g_layoutHover) return;
    float cx = 0.0f, cy = 0.0f;
    if (hoverIdx >= 0 && GridCell(g_app.mouse, cx, cy)) {
        const float pitch = g_cellSize + UI::GAP;
        const int viewTop = (int)ceilf(g_app.scroll / pitch);
        const int viewBottom = (int)floorf((g_app.scroll + g_app.height - GRID_TOP + UI::GAP) / pitch);
        SetTarget(g_tiles.Expand(hoverIdx, g_targetTiles[(size_t)hoverIdx], cx, cy, viewTop, viewBottom));
    } else {
        SetTarget(g_tiles.Rest());
    }
    g_layoutHover = hoverIdx;
}

/** Which tile of the target layout the mouse is over (-1 = none) */
static int HitTile(POINT p) {
    App& a = g_app;
    if (a.songs.empty()) return -1;
    if (p.y < GRID_TOP - 6) return -1;
    EnsureBase(a.width, (int)a.songs.size());
    float cx = 0.0f, cy = 0.0f;
    if (!GridCell(p, cx, cy)) return -1;
    const size_t cell = (size_t)((int)cy * g_tiles.Columns() + (int)cx);
    return cell < g_targetOwner.size() ? g_targetOwner[cell] : -1;
}

/* ══════════════════════════════════════════
   Covers
   ══════════════════════════════════════════ */
/* ───────── Background decode threads ─────────
   Decoding a 400px JPEG with WIC takes 2~4ms; on the UI thread that is a disaster while scrolling.
   So decoding runs on worker threads and the main thread only wraps the result into a D2D bitmap.
   ─────────────────────────────── */
static std::mutex  g_decMutex;
static std::deque<std::wstring> g_decQueue;
static std::vector<std::pair<std::wstring, IWICBitmapSource*>> g_decDone;
static std::vector<std::thread> g_decThreads;
static std::atomic<bool> g_decRun{ false };
static std::unordered_set<std::wstring> g_decRequested;      // main thread only

static IWICBitmapSource* DecodeScaled(IWICImagingFactory* wic,
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

    // WIC decodes lazily when pixels are read; copying into an IWICBitmap reads them here on the worker
    IWICBitmap* bmp = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateFormatConverter(&conv))) {
        if (SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)))
            wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &bmp);
        conv->Release();
    }

    if (scaler) scaler->Release();
    frame->Release();
    dec->Release();
    return bmp;
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
        IWICBitmapSource* bmp = wic ? DecodeScaled(wic, path, 320) : nullptr;
        {
            std::lock_guard<std::mutex> lk(g_decMutex);
            g_decDone.emplace_back(std::move(path), bmp);
        }
        g_coverDirty = true;            // tell the UI a new cover is ready
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

/* ── On-demand cover download: songs whose cover is not local are fetched when scrolled into view ── */
static std::string  HttpGet(const std::wstring& url);
static std::wstring Widen(const std::string& s);

/* The queue carries copies of url/hash; worker threads never touch g_app (avoids racing cache clears/reloads) */
struct CovJob {
    int         idx;
    std::string url;
    std::string hash;
};

extern std::string g_token, g_userid;   // defined below

/* Cross-thread hand-off of the playlist list */
static std::mutex          g_plMx;
static std::vector<Playlist> g_plPending;
static std::atomic<bool>   g_plReady{ false };

static std::mutex        g_covMx;
static std::vector<CovJob> g_covQueue;
static std::vector<CovJob> g_covDone;
static std::vector<char> g_covFlag;      // 0 pending / 1 queued / 2 processed

std::wstring g_dataDir;            // %USERPROFILE%\Documents\Floral

/* Writable data lives in Documents, not next to the exe: a portable exe dropped in
   Downloads or on a read-only folder still has somewhere to keep its state. */
static std::wstring MakeDataDir() {
    wchar_t docs[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, nullptr, 0, docs)))
        return L".";
    std::wstring d = std::wstring(docs) + L"\\Floral";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static bool FileExistsW(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring ExeDir() {
    wchar_t exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L".";
    std::wstring s(exe);
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

/* One-time migration: an older build kept everything next to the exe */
static void MigrateLegacyData(const std::wstring& dst) {
    if (FileExistsW(dst + L"\\kugou-likes.tsv")) return;      // already migrated
    std::wstring cands[2] = { ExeDir(), ExeDir() + L"\\.." };
    for (int c = 0; c < 2; c++) {
        std::wstring src = cands[c];
        if (!FileExistsW(src + L"\\kugou-likes.tsv")) continue;
        static const wchar_t* files[] = {
            L"kugou-likes.tsv", L"netease-likes.tsv", L"qq-likes.tsv",
            L"kugou-session.json", L"netease-session.json", L"qq-session.json",
            L"playlists.tsv", L"lang.txt", L"source.txt", L"theme.txt",
            L"quality.txt", L"volume.txt", L"window.txt",
        };
        for (const wchar_t* f : files) {
            std::wstring s = src + L"\\" + f;
            if (!FileExistsW(s)) continue;
            CopyFileW(s.c_str(), (dst + L"\\" + f).c_str(), FALSE);
        }
        // covers
        std::wstring sc = src + L"\\covers";
        if (FileExistsW(sc)) {
            std::wstring dc = dst + L"\\covers";
            CreateDirectoryW(dc.c_str(), nullptr);
            WIN32_FIND_DATAW fd{};
            HANDLE hf = FindFirstFileW((sc + L"\\*.jpg").c_str(), &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    CopyFileW((sc + L"\\" + fd.cFileName).c_str(),
                              (dc + L"\\" + fd.cFileName).c_str(), FALSE);
                } while (FindNextFileW(hf, &fd));
                FindClose(hf);
            }
        }
        Logf("APP", "已把旧数据迁移到 %s", Narrow(dst).c_str());
        return;
    }
}

static std::wstring AssetDir() {
    if (g_dataDir.empty()) {
        g_dataDir = MakeDataDir();
        MigrateLegacyData(g_dataDir);
        Logf("APP", "数据目录 = %s", Narrow(g_dataDir).c_str());
    }
    return g_dataDir;
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
            g_covDone.push_back(job);
        }
        g_coverDirty = true;            // download finished, ask the UI to pick it up
    }
}

/* ══════════════════════════════════════════
   Playlist list (for the picker page)
   ══════════════════════════════════════════ */
static std::wstring PlaylistsFile() { return AssetDir() + L"\\playlists.tsv"; }

/* ── UI language ── */
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
        if (first && s.rfind("name\t", 0) == 0) { first = false; continue; }   // header row
        first = false;
        // name \t gcid \t count \t coverUrl \t ver
        std::vector<std::string> col;
        size_t p = 0;
        while (col.size() < 5) {
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
        pl.ver = (col.size() > 4) ? atoi(col[4].c_str()) : 0;
        g_app.playlists.push_back(std::move(pl));
    }
    fclose(f);
    Logf("APP", "歌单列表载入 %d 个", (int)g_app.playlists.size());
}

static void SavePlaylists() {
    FILE* f = nullptr;
    _wfopen_s(&f, PlaylistsFile().c_str(), L"wb");
    if (!f) return;
    fputs("name\tgcid\tcount\tcoverUrl\tver\n", f);
    for (const Playlist& pl : g_app.playlists) {
        std::string name;
        int n = WideCharToMultiByte(CP_UTF8, 0, pl.name.c_str(), (int)pl.name.size(),
                                    nullptr, 0, nullptr, nullptr);
        name.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, pl.name.c_str(), (int)pl.name.size(),
                            &name[0], n, nullptr, nullptr);
        fprintf(f, "%s\t%s\t%d\t%s\t%d\n", name.c_str(), pl.gcid.c_str(), pl.count, pl.coverUrl.c_str(), pl.ver);
    }
    fclose(f);
}

/* ── Playlist covers: path derived from gcid, decode directly if the file exists ── */
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
    if (g_plCovBusy.load() >= 4) return;      // at most 4 at a time

    g_plCovAsked.insert(pl.gcid);
    g_plCovBusy.fetch_add(1);
    std::string url = pl.coverUrl;
    // the URL from the API has a {size} placeholder; substitute the actual size
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
/* Worker thread: fetch all playlists from the API and write them to disk */
static void FetchPlaylistsAsync(const std::string& token, const std::string& userid) {
    std::vector<KgPlaylist> got;
    std::wstring err;
    int n = KgFetchPlaylists(token, userid, got, err);
    if (n <= 0) {
        g_bootRefreshPending = false;   // never hold up the splash screen, even on failure
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
        pl.ver = kp.ver;
        g_plPending.push_back(std::move(pl));
    }
    g_plReady = true;
    g_bootRefreshPending = false;       // the first screen waits on this alone
    Logf("APP", "歌单列表拉取完成 %d 个", n);
}
/** Main thread: adopt downloaded covers into the cache */

static void DrainCovers() {
    std::vector<CovJob> done;
    { std::lock_guard<std::mutex> lk(g_covMx); done.swap(g_covDone); }
    if (done.empty()) return;

    for (const CovJob& job : done) {
        const int i = job.idx;
        if (i < 0 || i >= (int)g_app.songs.size()) continue;
        Song& s = g_app.songs[(size_t)i];
        if (s.hash != job.hash) continue;   // queued for a song list that has since been replaced
        std::wstring path = AssetDir() + L"\\covers\\" +
            std::wstring(s.hash.begin(), s.hash.end()) + L".jpg";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && s.cover.empty()) {
            s.cover = path;
            QueueDecode(path);
        }
        if (i < (int)g_covFlag.size()) g_covFlag[(size_t)i] = 2;
    }
    g_coverDirty = true;                // a new cover entered the decode queue
}

/** Queue a download for viewport songs without a cover (g_app is read only here; workers get copies) */
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
    // one per core, max 8 (decoding is pure CPU; WIC needs one factory per thread)
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

/** Main thread: move decoded covers into the cache */
static void DrainDecoded() {
    std::vector<std::pair<std::wstring, IWICBitmapSource*>> tmp;
    {
        std::lock_guard<std::mutex> lk(g_decMutex);
        if (g_decDone.empty()) return;
        tmp.swap(g_decDone);
    }
    for (auto& p : tmp) {
        // a decode queued before the cache was cleared can arrive after the first one, even after its upload
        const bool have = g_coverBmps.count(p.first) || !g_coverSrcs.try_emplace(p.first, p.second).second;
        if (have && p.second) p.second->Release();
    }
    g_mouseDirty = true;              // a new cover is available, trigger a redraw
}

static IWICBitmapSource* CoverSource(const std::wstring& path);   // forward declaration

/** Whether all first-screen covers are decoded (used to wait a little longer on the splash) */
static bool CoversReadyForFirstScreen() {
    // startup loads only playlist covers; song covers are decoded on demand when the playlist is entered
    return true;
}
/** Re-adopt covers already on disk into s.cover and queue them for decoding
    Previously only column 5 of the TSV was trusted, but exported tsv leaves it empty, so already-downloaded covers were treated as missing */
static void AdoptLocalCovers(std::vector<Song>& songs) {
    int adopted = 0;
    for (Song& s : songs) {
        if (!s.cover.empty() || s.hash.empty()) continue;
        std::wstring p = AssetDir() + L"\\covers\\" +
                         std::wstring(s.hash.begin(), s.hash.end()) + L".jpg";
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        s.cover = p;            // path only; decode when the cover actually has to be drawn
        adopted++;
    }
    if (adopted) Logf("APP", "复用本地封面 %d 张", adopted);
}

/* Each new cover bitmap is an upload to the GPU (about 0.2 ms), so a screen of fresh covers is
   spread over several frames; covers still waiting show their placeholder */
static const int COVER_UPLOADS_PER_FRAME = 4;
static int g_coverUploads = 0;

/* Pre-build D2D bitmaps so scrolling does not create and upload them on the spot */
static IWICBitmapSource* CoverSource(const std::wstring& path);   // defined below
static CoverBmp* CoverEntry(const std::wstring& path);
static size_t g_prewarmIdx = 0;
static void PrewarmCovers(int budget) {
    if (!g_rt || g_app.songs.empty()) return;
    int made = 0;
    while (g_prewarmIdx < g_app.songs.size() && made < budget && g_coverUploads > 0) {
        const Song& s = g_app.songs[g_prewarmIdx++];
        if (s.cover.empty()) continue;
        if (!CoverSource(s.cover)) continue;      // not decoded yet, try again next round
        if (CoverEntry(s.cover)) made++;
    }
}

/** Get a cover (only once decoded) */
static IWICBitmapSource* CoverSource(const std::wstring& path) {
    auto it = g_coverSrcs.find(path);
    return it == g_coverSrcs.end() ? nullptr : it->second;
}

static ID2D1Bitmap* CoverBitmap(const std::wstring& path);
static CoverBmp* CoverEntry(const std::wstring& path) {
    auto it = g_coverBmps.find(path);
    if (it != g_coverBmps.end()) return &it->second;
    IWICBitmapSource* src = CoverSource(path);
    if (!src) {
        QueueDecode(path);           // not decoded yet: hand it to the background, draw the placeholder meanwhile
        return nullptr;
    }
    if (!g_rt || g_coverUploads <= 0) { g_mouseDirty = true; return nullptr; }   // uploaded on a following frame
    g_coverUploads--;
    CoverBmp e;
    if (FAILED(g_rt->CreateBitmapFromWicBitmap(src, nullptr, &e.bmp))) return nullptr;
    // Only the GPU copy is drawn from here on; if the render target is replaced, the file is decoded again
    src->Release();
    g_coverSrcs.erase(path);
    g_decRequested.erase(path);
    MarkCoverReady(path);
    return &g_coverBmps.emplace(path, e).first->second;
}

/** Fill a rounded rect with a bitmap brush — no Layer, an order of magnitude faster than PushLayer */

static void DrawBitmapRounded(CoverBmp* e, const D2D1_RECT_F& r, float rad, float alpha = 1.0f) {
    if (!e || !e->bmp) return;
    if (!e->brush) {
        D2D1_BITMAP_BRUSH_PROPERTIES props = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP);
        if (FAILED(g_rt->CreateBitmapBrush(e->bmp, &props, nullptr, &e->brush))) {
            // if the brush cannot be created, just draw the rect; at least it does not stall
            g_rt->DrawBitmap(e->bmp, &r, g_alpha * g_alphaMul * alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
    }
    D2D1_SIZE_F sz = e->bmp->GetSize();
    if (sz.width <= 0.0f || sz.height <= 0.0f) return;

    // cover-fit: scale by the larger ratio to fill, then center-crop the overflow; never stretch
    float rw = r.right - r.left;
    float rh = r.bottom - r.top;
    float sc = (std::max)(rw / sz.width, rh / sz.height);
    float ox = r.left + (rw - sz.width * sc) * 0.5f;
    float oy = r.top + (rh - sz.height * sc) * 0.5f;

    e->brush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) *
                           D2D1::Matrix3x2F::Translation(ox, oy));
    e->brush->SetOpacity(g_alpha * g_alphaMul * alpha);   // bitmap brushes ignore color alpha; set it separately
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), e->brush);
}

static void DrawCoverArt(const D2D1_RECT_F& r, const Song& s, int idx, float rad) {
    CoverBmp* e = CoverEntry(s.cover);
    float fade = e ? CoverFade(s.cover) : 0.0f;
    if (e && fade >= 1.0f) {                 // fully visible, draw directly
        DrawBitmapRounded(e, r, rad);
        return;
    }
    // no cover: gradient placeholder + first character (brushes are cached, not rebuilt every frame)
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
    // the initial fades out with it so it does not overlap the real image
    Text(ch, Fmt(size, DWRITE_FONT_WEIGHT_SEMI_BOLD), r, 0xFFFFFF, DWRITE_TEXT_ALIGNMENT_CENTER,
         0.55f * (1.0f - fade));

    if (e) DrawBitmapRounded(e, r, rad, fade * fade);   // fading in: drawn on top of the placeholder
}

/* ══════════════════════════════════════════
   Window buttons
   ══════════════════════════════════════════ */
static D2D1_RECT_F BtnRect(int i, float W) {
    float x = W - UI::BTN_W * (float)(UI::BTN_COUNT - i);
    float y = 11.0f;
    return Rc(x, y, UI::BTN_W, UI::BTN_H);
}

/* ── Settings panel ── */
std::string  g_token, g_userid;      // login state (filled by LoadSession)
static bool         g_settingsOpen = false;

/* Panel appear/disappear: 0 collapsed, 1 expanded. Drawing wraps it in PanelFx for a fade + slight scale */
static float g_playIconMix = 0.0f;    // 0 = play triangle, 1 = pause bars (geometric morph, interruptible)
static float g_textT = 0.0f;          // time elapsed in the current phase
static int   g_textPhase = 0;         // 0 idle / 1 slide out left / 2 slide in from right
static int   g_textDir = 1;           // +1 next track (moves left) / -1 previous track (moves right)
static std::wstring g_outTitle, g_outArtist, g_outAlbum;   // old text that is leaving
static std::wstring g_curTitle, g_curArtist, g_curAlbum;   // the previous frame's three lines (used to grab a snapshot)
static float g_setT  = 0.0f;
static float g_qualT = 0.0f;
static float g_volT  = 0.0f;
static float g_togT  = 0.0f;
static std::wstring g_qrPath;            // QR code file for this login (renamed each time to avoid an old bitmap)
static bool         g_togOpen = false;      // listen-together panel
static bool         g_togInput = false;     // input field focused
static std::wstring g_togInputText;         // connection code being typed
static std::wstring g_togMyCode;            // my connection code
static bool         g_togBusy    = false;   // opening a room / joining
static bool         g_togHosting = false;   // room open but nobody has joined yet
static bool         g_togForcePush = false; // force the next PushLocalState (seek/loading change)
static bool         g_togIsHost  = false;   // I am the host (can control playback)
static int          g_togAddrIdx = -1;      // selected local address index (-1 = auto)
static std::wstring g_togPeer;              // peer nickname
static bool         g_togLost = false;      // disconnect notice
static std::wstring g_togMsg;               // in-panel message
static ULONGLONG    g_togMsgAt = 0;
/* Toast fade: 180ms in, 300ms out, moves up 6px while fading in
   Returns opacity, dy carries the vertical offset; 0 means already expired */
static float g_toastOverride = -1.0f;   // screenshot mode: fixed toast opacity
static float ToastFade(ULONGLONG at, float life, float& dy) {
    dy = 0.0f;
    if (g_toastOverride >= 0.0f) { dy = 0.0f; return g_toastOverride; }
    if (!at) return 0.0f;
    float t = (float)(NowMs() - at) / 1000.0f;
    if (t < 0.0f || t > life) return 0.0f;

    const float inT = 0.18f, outT = 0.30f;   // note: IN/OUT are SDK macros and cannot be used as variable names
    float a = 1.0f;
    if (t < inT) {                      // fade in + slide up into place
        float k = t / inT;
        a = k;
        dy = (1.0f - k) * 6.0f;
    } else if (t > life - outT) {       // fade out (ease-in: leaves decisively)
        float k = (t - (life - outT)) / outT;
        a = 1.0f - k * k;
    }
    return a < 0.0f ? 0.0f : a;
}
static void TogToast(const std::wstring& s) { g_togMsg = s; g_togMsgAt = NowMs(); }
static bool         g_loginChoose = false;   // show the "choose Kugou/NetEase" mini panel
static bool         g_loginAsked  = false;   // auto-opened once already; avoid repeated popups
static std::wstring g_settingsMsg;
static ULONGLONG    g_settingsMsgAt = 0;
static bool         g_loginRunning = false;
static std::string  g_loginUser0;
static ULONGLONG    g_waitTsv = 0;
static ULONGLONG    g_tsvBefore = 0;

/* ── Login state: ask the API once at startup so an expired token is caught early instead of waiting for playback to fail ── */
static int          g_loginState = 0;    // 0 checking / 1 valid / 2 expired or not signed in
static std::wstring g_nickname;
static int          g_vipType = 0;


static void SettingsToast(const wchar_t* msg) {
    g_settingsMsg = msg;
    g_settingsMsgAt = NowMs();
}

static ULONGLONG FileStamp(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

/** Recursively delete a directory (leave it if it fails; clean up on the next start) */
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

/** Clear local cache: cover images, audio, in-memory bitmaps */
static void ClearCache() {
    wchar_t full[MAX_PATH] = { 0 };
    std::wstring rel = AssetDir();
    if (!GetFullPathNameW(rel.c_str(), MAX_PATH, full, nullptr)) wcscpy(full, rel.c_str());
    std::wstring dir = full;

    // 1) drop the covers held in memory; songs download and decode them again on demand
    for (auto& entry : g_coverBmps) {
        if (entry.second.brush) entry.second.brush->Release();
        if (entry.second.bmp) entry.second.bmp->Release();
    }
    g_coverBmps.clear();
    for (auto& entry : g_coverSrcs) if (entry.second) entry.second->Release();
    g_coverSrcs.clear();
    g_decRequested.clear();
    g_plCovAsked.clear();
    for (auto& s : g_app.songs) s.cover.clear();
    std::fill(g_covFlag.begin(), g_covFlag.end(), 0);

    // 2) rename the whole directory, then delete — renaming is unaffected by files in use
    const wchar_t* subs[2] = { L"covers", L"audio" };
    for (const wchar_t* sub : subs) {
        std::wstring a = dir + L"\\" + sub;
        std::wstring b = dir + L"\\" + sub + L"_old";
        RemoveTree(b);                       // clean up leftovers from last time
        if (MoveFileW(a.c_str(), b.c_str())) {
            CreateDirectoryW(a.c_str(), nullptr);
            RemoveTree(b);                   // still locked? leave it for the next start
        }
    }

    SettingsToast(T(L"缓存已清空", L"Cache cleared"));
}


/* ── QR code login & playlist export (pure C++, no more node) ── */
static std::mutex   g_lgMx;
static int          g_lgState = 0;      // 0 idle 1 waiting for scan 2 scanned 3 success 4 failed
static std::string  g_lgToken, g_lgUserid;
static std::wstring g_lgMsg;
static int          g_expState = 0;     // 0 idle 1 fetching 2 done 3 failed
static int          g_expCount = 0;
static std::wstring g_expMsg;

/** Write kugou-session.json */
static std::wstring LocalDir();

/* ── Source: 0 Kugou / 1 NetEase ── */
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

/** Fetch playlists in the background and write the tsv */
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

/** Make sure the local sidecar is running (defined below) */
/* ── Embedded sidecar: extracted locally on first run, reused afterwards ── */
static std::wstring SidecarRoot() { return LocalDir() + L"\\sidecars"; }

/* Sidecars version: bump it whenever the content changes to trigger re-extraction */
#define SIDECARS_VERSION L"2026-11-07-1"

static bool ExtractSidecars() {
    // The listen-together helper and the NetEase/QQ services may start on different threads at once
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    std::wstring root = SidecarRoot();
    CreateDirectoryW(root.c_str(), nullptr);
    std::wstring okFile = root + L"\\.ok";

    // already extracted: reuse only if the version matches, otherwise start over
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

    // Windows 10+ ships bsdtar, which can extract a zip directly
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

/** QR login: get the QR code → poll → on success hand token/userid to the main thread to apply */
static void StartRelogin() {
    if (g_loginRunning) return;
    // use a new file name each time: the same path hits the cover bitmap cache and keeps showing the old QR code
    g_qrPath = AssetDir() + L"\\qr-" + std::to_wstring((unsigned long long)GetTickCount64()) + L".png";
    std::wstring qr = g_qrPath;
    if (g_source == 2) {                       // QQ Music QR login
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
    if (g_source == 1) {                       // NetEase QR login
        DeleteFileW(qr.c_str());
        g_loginRunning = true;
        { std::lock_guard<std::mutex> lk(g_lgMx); g_lgState = 1; g_lgMsg.clear(); }
        SettingsToast(T(L"用网易云音乐 App 扫码", L"Scan with the NetEase Music app"));
        std::thread([qr]() {
            EnsureSidecar();          // the local service may not be running after switching platforms
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
                    g_lgToken = ck; g_lgUserid = "1";   // NetEase uses a cookie; userid is a placeholder
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
            {   // stop polling if the user clicked "close" meanwhile
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

/* Settings card position: shared by drawing and the "modal hit area" */
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


    // current account state: conclusive only after the startup API check
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
    const int btnCount = (ThemeCount() > 1) ? 3 : 2;     // no toggle when there is only one palette
    for (int i = 0; i < btnCount && !g_loginChoose; i++) {   // hide the buttons while choosing a platform
        D2D1_RECT_F b = Rc(card.left + 24, card.top + 74 + i * 58, 250, 44);
        bool hot = Inside(b, g_app.mouse);
        FillRound(b, 10.0f, hot ? UI::SELECT : UI::BG);
        if (hot) StrokeRound(b, 10.0f, UI::BTN_CLOSE, 1.0f);
        Text(names[i], Fmt(13.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(b.left + 18, b.top, 220, 44), UI::TEXT);
        Hit h; h.r = b; h.kind = kinds[i];
        g_app.hits.push_back(h);
    }

    // ── Language ──
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

    // choose a platform first, then show the QR code
    if (g_loginChoose && !g_loginRunning) {
        Text(T(L"选择要登录的平台", L"Choose a platform"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(card.right - 300, card.top + 74, 270, 20), UI::TEXT2);
        const wchar_t* pf[3] = { T(L"酷狗音乐", L"Kugou Music"), T(L"网易云音乐", L"NetEase Music"), T(L"QQ 音乐", L"QQ Music") };
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

    // logging in: show the QR code on the right
    std::wstring qr = g_qrPath;
    if (g_loginRunning && GetFileAttributesW(qr.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CoverBmp* e = CoverEntry(qr)) {
            D2D1_RECT_F q = Rc(card.right - 24 - 150, card.top + 74, 150, 150);
            FillRound(Rc(q.left - 6, q.top - 6, 162, 162), 10.0f, 0xFFFFFF);
            DrawBitmapRounded(e, q, 8.0f);

            // polling state: makes it visible that the program is waiting
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

    // settings button: left of the three window buttons
    D2D1_RECT_F g = Rc(BtnRect(0, W).left - 40.0f, BtnRect(0, W).top + 2.0f, 32.0f,
                       (BtnRect(0, W).bottom - BtnRect(0, W).top) - 4.0f);
    bool hot = Inside(g, g_app.mouse);
    if (hot || g_settingsOpen) FillRound(g, UI::R_BTN, UI::HOVER);

    {
        float cx = (g.left + g.right) * 0.5f;
        float cy = (g.top + g.bottom) * 0.5f;
        UINT32 fg = (hot || g_settingsOpen) ? UI::TEXT : UI::TEXT2;
        // three sliders as the settings icon
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

/** Login pill position: shared by drawing and WM_NCHITTEST */
static D2D1_RECT_F LoginPillRect(float W) {
    float rightEdge = W - (UI::BTN_W * UI::BTN_COUNT + 76.0f);
    float tw = 168.0f;                   // sized for the longest "service not responding · click to check"
    return Rc(rightEdge - tw - 12.0f, UI::TITLE_H * 0.5f - 11.0f, tw, 22.0f);
}

static void DrawTitleBar(float W) {


    // title bar background: same color as the page, opaque. During enter/exit this layer gets "wiped" in along with the clip
    FillRect(Rc(0, 0, W, UI::TITLE_H), UI::BG, 1.0f);

    Text(L"Floral", Fmt(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
         Rc(UI::PAD, 0, 240, UI::TITLE_H), UI::TEXT);
    {   // click the top-left corner to go back to the playlist picker
        Hit hb; hb.r = Rc(UI::PAD - 6, 0, 90, UI::TITLE_H); hb.kind = HK_PL_PICK;
        g_app.hits.push_back(hb);
    }

    float rightEdge = W - (UI::BTN_W * UI::BTN_COUNT + 76.0f);

    // back to the playlist picker: 2×2 small squares, left of the disc button
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
        if (!onPicker) rightEdge = pb2.left - 12.0f;   // keep the song count off the button
    }

    // go to the player page: small disc icon, left of the settings button
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

    // when the session is invalid or the service is down, show a clickable pill instead of letting people find out when playback fails
    if (g_loginState == 2 || g_loginState == 3) {
        const wchar_t* tip;
        if (g_loginState == 3) {
            tip = T(L"接口服务未启动 · 点此查看", L"Service not running · click to check");
        } else if (g_userid.empty() && g_token.empty()) {
            tip = T(L"未登录 · 点此登录", L"Not signed in · click to sign in");
        } else {
            tip = T(L"登录已过期 · 点此重登", L"Session expired · click to sign in");
        }
        D2D1_RECT_F pill = LoginPillRect(W);
        FillRound(pill, 11.0f, UI::SELECT);
        if (Inside(pill, g_app.mouse)) StrokeRound(pill, 11.0f, UI::BTN_CLOSE, 1.0f);
        Text(tip, Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL), pill, UI::TEXT,
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
   Grid page
   ══════════════════════════════════════════ */
static void DrawTile(const D2D1_RECT_F& t, const Song& s, int idx, float anim) {
    const float w = t.right - t.left;
    const float h = t.bottom - t.top;

    // Shadow
    if (anim > 0.02f) {
        D2D1_RECT_F sh = Rc(t.left + 4, t.top + 14, w - 8, h - 10);
        FillRound(sh, UI::R_TILE, UI::SHADOW, 0.18f * anim);
    }

    DrawCoverArt(t, s, idx, UI::R_TILE);

    if (anim > 0.01f) {
        // Bottom scrim: fill the rounded gradient directly; top alpha is 0, so the corner radius is invisible
        float sh = h * 0.46f;
        FillRoundGradV(Rc(t.left, t.bottom - sh, w, sh), UI::R_TILE,
                       0x000000, 0.0f, 0x000000, 0.66f * anim);

        Text(s.title, Fmt(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             Rc(t.left + 16, t.bottom - 58, w - 64, 22), 0xFFFFFF,
             DWRITE_TEXT_ALIGNMENT_LEADING, anim);
        Text(s.artist, Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(t.left + 16, t.bottom - 34, w - 64, 18), 0xFFFFFF,
             DWRITE_TEXT_ALIGNMENT_LEADING, anim * 0.8f);

        // Play badge (triangle)
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
        // No zoom while scrolling, and until the mouse moves again after scrolling
        bool gliding = std::fabs(a.scrollTarget - a.scroll) > 0.5f;
        a.hoverTile = (gliding || !g_hoverArmed || g_settingsOpen) ? -1 : HitTile(a.mouse);
    }
    int hov = (a.page == Page::Grid && a.pageAnim < 0.02f) ? a.hoverTile : -1;
    EnsureTarget(W, n, hov);

    float viewH = (std::max)(0.0f, H - GRID_TOP - UI::PAD);
    a.maxScroll = (std::max)(0.0f, g_gridHeight - viewH);
    if (a.scrollTarget > a.maxScroll) a.scrollTarget = a.maxScroll;
    if (a.scrollTarget < 0) a.scrollTarget = 0;
    if (a.scroll > a.maxScroll) a.scroll = a.maxScroll;
    if (a.scroll < 0) a.scroll = 0;

    // ── Scrollbar (right side, draggable) ──
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

    // Collect only bands near the viewport (one extra above and below)
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

    // Hit areas use the as-drawn layout (including hover zoom and displacement), so clicks land on what you see
    // No clicks until the fly-in lands, to avoid empty hit areas
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

    // Fetch covers on demand for viewport songs that lack one
    RequestVisibleCovers();

    // The clip stays on the screen target, so tiles just outside it can still blur into view
    g_rt->PushAxisAlignedClip(D2D1::RectF(0, GRID_TOP - 10, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
    BeginMotionBlur(g_gridBlur);

    // Enter progress: 0→1.45 is the "fly in from the right one by one" run
    const bool entering = (g_gridEnter < 0.999f);
    const float entT = g_gridEnter * 1.45f;

    // Still tiles first, then moving ones, then the hovered one, so tiles in motion pass over the still ones
    int order = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (int i : g_vis) {
            bool isHover = (i == hov);
            const TileBox& d = g_draw[(size_t)i];
            const TileBox& t = g_target[(size_t)i];
            const bool moving = d.x != t.x || d.y != t.y || d.w != t.w || d.h != t.h;
            if (pass != (isHover ? 2 : moving ? 1 : 0)) continue;
            float ty = GRID_TOP + d.y - a.scroll;

            float enterA = 1.0f, enterDx = 0.0f;
            if (entering) {
                const int ord = (order < 18) ? order : 18;       // stagger at most 18 slots so the tail isn't late
                order++;
                float p = (entT - (float)ord * 0.05f) / 0.42f;   // 50ms apart, 420ms per tile
                if (p <= 0.0f) continue;
                if (p > 1.0f) p = 1.0f;
                float e = 1.0f - powf(1.0f - p, 3.0f);
                enterA = e;
                enterDx = (1.0f - e) * (W + 60.0f - d.x);        // flies in from the right edge
            }

            float keep = g_alphaMul;
            g_alphaMul = keep * enterA;
            DrawTile(Rc(d.x + enterDx, ty, d.w, d.h), a.songs[i], i, isHover ? a.tileAnim[i] : 0.0f);
            g_alphaMul = keep;
        }
    }

    EndMotionBlur();
    g_rt->PopAxisAlignedClip();
}

/* ══════════════════════════════════════════
   Player page: spinning CD on the left + album cases above/below, song info on the right
   ══════════════════════════════════════════ */
static int   g_switchFrom = -1, g_switchTo = -1, g_switchDir = 1;
static float g_switchT = 1.0f;          // 0..1; 1 means no track switch in progress
static float g_discAngle = 0.0f;        // disc rotation angle (degrees)
static float g_lineT = 0.0f;            // divider unfold progress 0..1
static int   g_casePrev0 = -1, g_caseNext0 = -1;   // at switch time, which two songs the cases held before

/* Leaving the player page: full-screen fade out → grid flies in from the right like the intro */
static bool  g_exitOn = false;
static float g_exitT = 1.0f;

/* Entering the player page: 0..1 runs the whole transition */
static bool  g_enterOn = false;
static float g_enterT = 1.0f;
static int   g_enterIdx = -1;
static D2D1_RECT_F g_enterFrom{};       // on-screen rect of the clicked tile

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

/** Draw a CD: vinyl base + rotating cover + grooves + center hole */
static void DrawDisc(const D2D1_RECT_F& r, const Song& s, int idx, float angleDeg, float alpha) {
    if (alpha <= 0.01f) return;
    const float rad = (r.right - r.left) * 0.5f;
    if (rad < 4.0f) return;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float artR = rad * 0.66f;

    // Vinyl base + outer ring
    FillCircle(cx, cy, rad, UI::DISC, alpha);

    // Rotating cover (ellipse fill with a bitmap brush; no Layer)
    // During fade-in, lay the placeholder gradient first, then draw the real art on top
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

    if (!haveBrush || coverFade < 1.0f) {          // placeholder (also the fade-in base)
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

    // Vinyl grooves
    for (int i = 1; i <= 3; i++) {
        float rr = artR + (rad - artR) * (i / 4.0f);
        StrokeCircle(cx, cy, rr, UI::DISC_GROOVE, 0.55f * alpha, 1.0f);
    }
    // Fixed highlight to sell the disc shape
    if (g_brush) {
        for (int k = 0; k < 2; k++) {
            float a0 = -1.9f + k * 0.26f;
            StrokeCircle(cx, cy, rad * (0.90f - k * 0.05f), UI::DISC_GROOVE, 0.35f * alpha, 1.4f);
        }
    }
    // Center hole
    FillCircle(cx, cy, rad * 0.05f, UI::BG, alpha);
    StrokeCircle(cx, cy, rad * 0.05f, UI::DISC_GROOVE, 0.6f * alpha, 1.0f);
    // Outer edge
    StrokeCircle(cx, cy, rad - 0.5f, 0x000000, 0.18f * alpha, 1.0f);
}

/** Cover inside a small album case */
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

/** CD jewel case (modeled on the real thing): clear plastic shell → dark gray tray → booklet cover */
static void DrawCase(const D2D1_RECT_F& r, int idx, float alpha) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size() || alpha <= 0.01f) return;

    g_alphaMul = alpha;

    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const float rad  = (std::max)(3.0f, w * 0.012f);     // hard-edged box
    const float rim  = (std::max)(4.0f, w * 0.026f);     // clear plastic rim
    const float tray = (std::max)(5.0f, w * 0.030f);     // dark tray

    // 1) Clear plastic shell
    FillRoundGrad(r, rad, 0xFDFBFC, 0xDED3D8);

    // 2) Dark gray tray
    D2D1_RECT_F tr = Rc(r.left + rim, r.top + rim, w - rim * 2.0f, h - rim * 2.0f);
    FillRoundGrad(tr, rad, 0x4C464A, 0x312B2F);

    // 3) Booklet cover
    D2D1_RECT_F inner = Rc(tr.left + tray, tr.top + tray,
                           w - (rim + tray) * 2.0f, h - (rim + tray) * 2.0f);
    DrawCoverArt(inner, a.songs[idx], idx, rad);

    // 4) Gloss along the top of the booklet
    FillRound(Rc(inner.left, inner.top, inner.right - inner.left,
                 (std::max)(6.0f, h * 0.04f)), rad, 0xFFFFFF, 0.14f);

    // 5) Plastic rim highlight + outer outline
    StrokeRound(Rc(r.left + 1, r.top + 1, w - 2, h - 2), rad, 0xFFFFFF, 0.55f, 1.0f);
    StrokeRound(r, rad, 0x000000, 0.18f, 1.0f);

    g_alphaMul = 1.0f;
}

/** Record sleeve: as big as the CD, exposing only a strip at the top/bottom; can be tilted as a whole */
static void DrawSleeve(const D2D1_RECT_F& full, const D2D1_RECT_F& strip,
                       int idx, float alpha, float angleDeg) {
    if (idx < 0 || idx >= (int)g_app.songs.size() || alpha <= 0.01f) return;

    D2D1::Matrix3x2F saved;
    g_rt->GetTransform(&saved);

    // Set the clip first (identity transform, so it stays horizontal), then rotate for drawing
    g_rt->PushAxisAlignedClip(strip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float pcx = (full.left + full.right) * 0.5f;
    float pcy = (full.top + full.bottom) * 0.5f;
    g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F(pcx, pcy)));

    DrawCase(full, idx, alpha);

    g_rt->SetTransform(saved);
    g_rt->PopAxisAlignedClip();
}

/* ── Audio ── */
static bool g_audioOk = false;      // whether the current track is really playing

/* ── Playback loading animation ── */
static bool      g_audioLoading = false;
static bool      g_audioError = false;
static int       g_audioErrKind = 0;      // 1 resolve failed / 2 cannot open (undecodable) / 3 download broke off
static ULONGLONG g_loadingSince = 0;
static ULONGLONG g_loadingMin = 0;        // show at least until this tick; anything faster is invisible
static int       g_audioRetry = 0;        // allow one automatic retry when open succeeds but no sound comes out
static float     g_spinAngle = 0.0f;      // radians


/* Play URL resolution runs on a worker thread; the UI thread picks up the result from g_urlReady */
static std::mutex    g_urlMx;
static std::wstring  g_urlReady;
static int           g_urlState = 0;   // 0 idle, 1 resolving, 2 ready, 3 failed

/* Audio open runs on a worker thread: prebuffering waits on the network and would visibly stall the UI thread */
static std::mutex       g_openMx;
static std::atomic<int> g_openGen{ 0 };     // +1 per request; stale thread results are dropped
static std::atomic<int> g_openResult{ 0 };  // 0 none / 1 ok / -1 failed
static std::wstring     g_openUrl;          // last attempted URL (read by the UI thread)

/* Closing audio also goes async: stopping the device + joining the download thread measures 16~31ms, which drops a frame on the UI thread */
static void BeginCloseAsync() {
    int gen = ++g_openGen;  // invalidate in-flight opens
    g_openResult = 0;
    std::thread([gen] {
        std::lock_guard<std::mutex> lk(g_openMx);
        if (g_openGen.load() == gen) AudioClose();   // a newer open closes the old track itself
    }).detach();
}

static void BeginOpenAsync(const std::wstring& url) {
    if (url.empty()) return;
    g_openUrl = url;
    int gen = ++g_openGen;
    g_openResult = 0;
    std::thread([url, gen]() {
        std::lock_guard<std::mutex> lk(g_openMx);
        if (g_openGen.load() != gen) return;           // already superseded by a newer request
        bool ok = AudioOpen(url);                      // blocks until prebuffering completes
        if (g_openGen.load() != gen) {                 // track changed meanwhile: discard
            if (ok) AudioClose();
            return;
        }
        g_openResult = ok ? 1 : -1;
    }).detach();
}

/* Close audio on the UI thread; background opens still queued or running are dropped */
static void CloseAudioNow() {
    ++g_openGen;
    g_openResult = 0;
    AudioClose();
}

static int           g_urlIdx = -1;

static std::string   g_token2_unused;

/* Fade in/out */
static float g_vol = 1.0f;              // fade factor
static float g_userVol = 0.8f;          // user volume 0..1
static bool  g_volOpen = false;         // volume panel expanded
static bool  g_volDrag = false;
static D2D1_RECT_F g_volTrack{};        // slider rect inside the popup (needed for dragging)
static bool  g_volMuted = false;
static float g_volBeforeMute = 0.8f;

/** Volume handed to the audio engine: fade factor × user volume */
static void ApplyVolume() {
    if (g_audioOk) AudioSetVolume((int)(g_vol * g_userVol * 1000.0f + 0.5f));
}

static std::wstring LocalDir();

/* ── Quality selection ── */
static int  g_quality = 1;              // 0 128K / 1 320K / 2 lossless / 3 Hi-Res
static bool g_qualOpen = false;
static const int      kQualCount = 4;
static const wchar_t* kQualName[4] = { T(L"标准", L"Standard"), T(L"高品", L"High"), T(L"无损", L"Lossless"), L"Hi-Res" };
static const char*    kQualApi[4]  = { "128", "320", "flac", "high" };   // Kugou's quality parameter per tier

/* Which tiers the current song actually has: bit0=standard bit1=high bit2=lossless bit3=Hi-Res
   Unknown (0xFF) shows all of them; a request served below its tier rules out the tiers above the one served */
static std::mutex    g_qmMx;
static int           g_qmIdx = -1;
static unsigned char g_qmMask = 0xFF;

static unsigned char QualMaskFor(int idx) {
    std::lock_guard<std::mutex> lk(g_qmMx);
    return (idx == g_qmIdx) ? g_qmMask : 0xFF;
}
static void SetQualMask(int idx, int requested, int gotLevel) {
    std::lock_guard<std::mutex> lk(g_qmMx);
    const unsigned char upToServed = (unsigned char)((2u << gotLevel) - 1);
    if (gotLevel < requested) g_qmMask = upToServed;
    else g_qmMask = (idx == g_qmIdx) ? (unsigned char)(g_qmMask | upToServed) : 0xFF;   // keep tiers ruled out before
    g_qmIdx = idx;
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

/** Volume button + popup slider (drawn to the right of the next button) */
static void DrawVolume(float px, float by, float cy);
static int   g_fadeDir = 0;            // +1 fade in, -1 fade out
static int   g_pendingDir = 0;         // track to switch to after fade-out finishes
static int   g_pendingPick = -1;       // another song was picked in the list: load it after the old one fades out
static int   g_pendingSeekMs = -1;     // position to seek to after reload (used when changing quality)

/* Progress bar dragging */
static bool      g_seekDrag = false;
static ULONGLONG g_lastSeek = 0;

static std::string HttpGet(const std::wstring& url) {
    std::string out;
    HINTERNET hInet = InternetOpenW(L"KugouPlayer/1.0", INTERNET_OPEN_TYPE_DIRECT,
                                    nullptr, nullptr, 0);
    if (!hInet) return out;
    // Default timeouts are minute-scale; a dead service would hang the worker thread, so set them explicitly
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

/* ── Embedded resources: the data stays in the mapped exe image and is read from disk only when touched ── */
static bool FindRes(int id, const void*& data, DWORD& size) {
    HRSRC h = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    size = SizeofResource(nullptr, h);
    data = LockResource(g);
    return data && size;
}



/* Start a hidden service process inside a job that the system kills when Floral exits, crashes included */
static bool LaunchSidecar(std::wstring cmd, const std::wstring& dir) {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(j, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
        return j;
    }();

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    // stays suspended until it is in the job, so processes it spawns join the job as well
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, dir.c_str(), &si, &pi))
        return false;
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static std::wstring LocalDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n ? std::wstring(buf, n) : L".") + L"\\KugouPlayer";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

/* Whether a local service accepts connections on the port. Windows reports a refused
   connection only after retrying for about 2 s, so a short wait decides it instead */
static bool PortListening(unsigned short port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool listening = false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(s, (const sockaddr*)&addr, sizeof(addr));
        fd_set connected;
        FD_ZERO(&connected);
        FD_SET(s, &connected);
        timeval wait = { 0, 200 * 1000 };
        listening = select(0, nullptr, &connected, nullptr, &wait) == 1;
        closesocket(s);
    }
    WSACleanup();
    return listening;
}

/* Start a Node-style sidecar: skip if already running, else unpack and launch it, then wait for the port */
static bool StartNodeSidecar(const wchar_t* sub, const wchar_t* script,
                             unsigned short port, const char* name) {
    if (PortListening(port)) return true;                  // already running

    if (!ExtractSidecars()) {
        Logf("ERR", "%s：sidecars 解压失败", name);
        return false;
    }

    std::wstring dir     = SidecarRoot() + L"\\" + sub;
    std::wstring nodeExe = SidecarRoot() + L"\\node.exe";
    if (!LaunchSidecar(L"\"" + nodeExe + L"\" " + script, dir)) {
        Logf("ERR", "%s 启动失败 code=%lu", name, GetLastError());
        return false;
    }

    for (int i = 0; i < 80; i++) {                        // wait up to about 36 s
        if (PortListening(port)) break;
        Sleep(250);
    }
    Logf("APP", "%s 已启动", name);
    return true;
}

#ifndef FLORAL_NO_TOGETHER
/* Listen-together helper: MQTT credentials are passed via environment variables */
static void StartTogetherHelper() {
    SetEnvironmentVariableW(L"FLORAL_MQTT_HOST", L"k9bf6e3a.ala.cn-shenzhen.emqxsl.cn");
    SetEnvironmentVariableW(L"FLORAL_MQTT_PORT", L"8883");
    SetEnvironmentVariableW(L"FLORAL_MQTT_USER", L"Floral");
    SetEnvironmentVariableW(L"FLORAL_MQTT_PASS", L"qwertyuiop!@#$%^&*()");
    SetEnvironmentVariableW(L"FLORAL_LISTEN_PORT", L"4100");
    StartNodeSidecar(L"together", L"together-server.js", 4100, "一起听助手");
}
#endif

/* Start the API service of a music source and wait until it accepts connections */
static void EnsureSidecar(int source) {
    // Several threads call this; the lock keeps them from launching a service twice and makes them wait for it
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);

    if (source == 2) {                                    // QQ
        StartNodeSidecar(L"qq",
                         L"node_modules/@yakult-green-tea/qq-music-api/dist/src/app.js",
                         3200, "QQ 接口服务");
        return;
    }
    if (source == 1) {                                    // NetEase
        SetEnvironmentVariableW(L"REAL_IP", L"116.25.146.177");   // without this every request is a 502
        StartNodeSidecar(L"netease", L"server.js", 4000, "网易云接口服务");
        return;
    }

    // Kugou: embedded single-file pkg exe, no node needed
    if (PortListening(3000)) return;

    const void* data = nullptr;
    DWORD size = 0;
    if (!FindRes(IDR_KUGOU_API, data, size) || size < 100000) return;

    std::wstring dir = LocalDir();
    std::wstring exe = dir + L"\\kugou-api.exe";

    WIN32_FILE_ATTRIBUTE_DATA fad;
    bool need = !GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fad) ||
                fad.nFileSizeLow != size;
    if (need) {
        FILE* f = nullptr;
        _wfopen_s(&f, exe.c_str(), L"wb");
        if (!f) return;
        fwrite(data, 1, size, f);
        fclose(f);
    }

    LaunchSidecar(L"\"" + exe + L"\"", dir);
    for (int i = 0; i < 30; i++) {
        if (PortListening(3000)) break;
        Sleep(200);
    }
}

static void EnsureSidecar() { EnsureSidecar(g_source); }

/** Read a JSON string field, tolerating whitespace after the colon/comma (node-written json has spaces) */
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

/** Read token / userid from the current source's session file */
static void LoadSession() {
    FILE* f = nullptr;
    _wfopen_s(&f, SessionFile().c_str(), L"rb");
    if (!f) return;
    std::string s;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    if (g_source == 1 || g_source == 2) {     // NetEase / QQ: the whole cookie is the token
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

/** Read a JSON integer field, tolerating whitespace */
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

/** Device id (Kugou risk control wants dfid), cached for 1 hour, shared across threads */
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

/** Playback failed: also recheck sign-in state (at most once per 15s); if invalid, raise the banner right away */
static void RecheckLoginOnFailure() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLoginCheck < 15000) return;
    g_lastLoginCheck = now;
    CheckLoginAsync();
}

static void CheckLoginAsync() {
    g_lastLoginCheck = GetTickCount64();
    if (g_userid.empty() || g_token.empty()) {
        g_loginState = 2;                // no session file at all, no need to send a request
        return;
    }
    std::string uid = g_userid, tok = g_token;
    int src = g_source;
    Trace("login check start");
    std::thread([uid, tok, src]() {
        if (src == 2) {                       // QQ: query /login/status on 3200
            std::string raw = HttpGet(Widen("http://127.0.0.1:3200/login/status?cookie=qqmusic_session%3D" +
                                            UrlEnc(tok)));
            if (raw.empty()) { g_loginState = 3; return; }
            if (raw.find("qqmusic_session") == std::string::npos &&
                raw.find("\"code\":200") == std::string::npos) { g_loginState = 2; return; }
            g_loginState = 1;
            return;
        }
        if (src == 1) {                       // NetEase: query /user/account on 4000
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
        if (r.empty()) {                 // no response from the service; not a sign-in problem
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

/** Resolve a song's play URL at a quality tier (blocking; only call from worker threads). The song is a copy
    made on the UI thread, since g_app.songs may be replaced meanwhile; gotLevel receives the tier served */
static std::wstring ResolvePlayUrl(const Song& s, int quality, int& gotLevel) {
    const std::string& hash = s.hash;
    if (hash.empty()) return L"";

    if (g_source == 2) {                       // QQ Music
        Logf("SRC", "解析 qq id=%s 档位=%d", hash.c_str(), quality);
        std::wstring e;
        std::wstring u = QqResolveUrl(hash, g_token, quality, e, &gotLevel);
        if (u.empty()) {
            Logf("ERR", "QQ 解析失败: %s", Narrow(e).c_str());
            Trace("qq resolve fail");
        } else {
            Logf("SRC", "QQ 解析成功 档位=%d url=%s", gotLevel,
                 Narrow(u.substr(0, u.find(L'?') == std::wstring::npos ? 64 : u.find(L'?'))).c_str());
        }
        return u;
    }
    if (g_source == 1) {                       // NetEase
        Logf("SRC", "解析 netease id=%s 档位=%d", hash.c_str(), quality);
        std::wstring e;
        std::wstring u = NgResolveUrl(hash, quality, g_token, e, &gotLevel);
        if (u.empty()) Logf("ERR", "网易云解析失败: %s", Narrow(e).c_str());
        else Logf("SRC", "网易云解析成功 档位=%d", gotLevel);
        return u;
    }

    Logf("SRC", "解析 kugou hash=%s 档位=%d", hash.c_str(), quality);
    std::string dfid = GetDfid();
    std::string cookie = "dfid=" + dfid + ";userid=" + g_userid + ";token=" + g_token;
    char url[2048] = "";
    std::string lastResp;
    std::wstring best;
    int bestLevel = -1;
    for (int q = quality; q > bestLevel; q--) {   // a lower tier cannot beat the file already found
        sprintf(url,
                "http://127.0.0.1:3000/song/url?hash=%s&album_audio_id=%lld&album_id=%lld&quality=%s&cookie=%s",
                hash.c_str(), s.mixId, s.albumId, kQualApi[q], UrlEnc(cookie).c_str());
        for (int attempt = 0; attempt < 2; attempt++) {
            std::string r = HttpGet(Widen(url));
            lastResp = r;
            std::string u = JsonArrFirst(r, "url");
            if (u.empty()) u = JsonArrFirst(r, "backupUrl");
            if (u.empty()) {
                if (attempt < 1) Sleep(200);
                continue;
            }
            // Kugou answers a tier the song lacks with a 128k MP3, so the tier comes from the file returned
            int got = (JsonStr(r, "extName") == "flac") ? q : (JsonInt(r, "bitRate") >= 256000 ? 1 : 0);
            if (got > bestLevel) { best = Widen(u); bestLevel = got; }
            break;
        }
        if (bestLevel >= q) break;
    }
    if (!best.empty()) {
        gotLevel = bestLevel;
        Logf("SRC", "酷狗解析成功 档位=%d", bestLevel);
        return best;
    }

    {   // on failure dump the request and response to ease debugging
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

/** Local copy of this song at audio\<hash>.flac or .mp3 in the data directory; empty when absent, so it is streamed */
static std::wstring FindAudio(int idx) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size()) return L"";
    const std::string& h = a.songs[(size_t)idx].hash;
    if (h.empty()) return L"";
    std::wstring base = AssetDir() + L"\\audio\\" + std::wstring(h.begin(), h.end());
    for (const wchar_t* ext : { L".flac", L".mp3" })
        if (FileExistsW(base + ext)) return base + ext;
    return L"";
}

/** Trace log: off by default, only written when started with --trace (keeps the packaged build litter-free) */
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

/* ── URL prefetch: when the current track starts playing, resolve the next one too ──
   URLs are keyed by song hash, so a prefetch that ends after the song list changed stays with its own song */
struct CachedUrl {
    std::wstring url;
    int quality = -1;     // tier requested; the URL is used only while this is still the selected tier
    int level = 0;        // tier served
};
static std::mutex g_preMx;
static std::map<std::string, CachedUrl> g_urlCache;
static std::set<std::string> g_preFetching;

static void PrefetchUrl(int idx) {
    if (idx < 0 || idx >= (int)g_app.songs.size()) return;
    const Song song = g_app.songs[(size_t)idx];
    {
        std::lock_guard<std::mutex> lk(g_preMx);
        if (song.hash.empty() || g_urlCache.count(song.hash) || !g_preFetching.insert(song.hash).second) return;
    }
    std::thread([song, quality = g_quality]() {
        int got = 0;
        std::wstring u = ResolvePlayUrl(song, quality, got);
        std::lock_guard<std::mutex> lk(g_preMx);
        if (!u.empty()) g_urlCache[song.hash] = { u, quality, got };
        g_preFetching.erase(song.hash);
    }).detach();
}

/** Change track: try the local file first (unless useLocal is false), else resolve the URL in the background and stream */
static void LoadAudioFor(int idx, bool useLocal = true) {
    Trace("LoadAudioFor idx=%d", idx);
    BeginCloseAsync();
    g_audioOk = false;
    g_fadeDir = 0;
    g_pendingDir = 0;
    g_audioError = false;
    g_audioErrKind = 0;
    g_audioLoading = true;              // raise "loading" first; clear it once there's a position
    g_loadingSince = GetTickCount64();

    std::wstring local = useLocal ? FindAudio(idx) : L"";
    if (!local.empty()) {
        g_loadingMin = 0;
        BeginOpenAsync(local);
        return;
    }

    // Reuse a prefetched URL to save a round trip
    const Song song = (idx >= 0 && idx < (int)g_app.songs.size()) ? g_app.songs[(size_t)idx] : Song{};
    CachedUrl cached;
    {
        std::lock_guard<std::mutex> lk(g_preMx);
        auto it = g_urlCache.find(song.hash);
        if (it != g_urlCache.end()) {
            if (it->second.quality == g_quality) cached = it->second;
            g_urlCache.erase(it);
        }
    }
    Logf("AUD", "尝试打开本地缓存 idx=%d", idx);
    if (!cached.url.empty()) {
        SetQualMask(idx, cached.quality, cached.level);
        g_loadingMin = GetTickCount64() + 300;
        BeginOpenAsync(cached.url);              // network stream: open in the background, don't stall the UI
        PrefetchUrl((idx + 1) % (int)g_app.songs.size());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_urlMx);
        g_urlState = 1;
        g_urlReady.clear();
        g_urlIdx = idx;
    }
    g_loadingMin = GetTickCount64() + 600;   // keep the spinner up at least 0.6s when streaming
    std::thread([idx, song, source = g_source, quality = g_quality]() {
        Trace("resolve start idx=%d", idx);
        EnsureSidecar(source);          // the service may still be starting: cached playlists open right after launch
        int got = 0;
        std::wstring u = ResolvePlayUrl(song, quality, got);
        if (!u.empty()) SetQualMask(idx, quality, got);
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

    // Remember which two songs the cases held before switching so they show during the animation
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
    g_audioRetry = 0;              // new song: reset the retry count
    g_pendingSeekMs = -1;          // a new song starts from its beginning

    LoadAudioFor(to);
}

/** Track-switch entry: fade out first if audio is playing, then switch, avoiding a hard-cut pop */
static void SwitchTrack(int dir) {
    if (g_audioOk && g_pendingDir == 0 && g_fadeDir >= 0) {
        g_pendingDir = dir;
        g_fadeDir = -1;
        return;
    }
    SwitchTrackNow(dir);
}

/** Song info on a tile (scrim + title + artist) */
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

/** Tile → CD morph: discK=0 is the square art, 1 the disc; infoK drives the song info on the square */
static void DrawMorphDisc(const D2D1_RECT_F& r, const Song& s, int idx, float discK, float infoK) {
    if (discK < 0.0f) discK = 0.0f;
    if (discK > 1.0f) discK = 1.0f;
    if (discK <= 0.001f) {
        DrawCoverArt(r, s, idx, UI::R_TILE);
        if (infoK > 0.0f) DrawTileInfo(r, s, infoK);
        return;
    }

    // Fully continuous morph. Two things used to snap at t=0:
    //   1. only the tile's WIDTH was used, so a wide tile instantly became a square;
    //   2. the vinyl was drawn at full radius from the first frame, poking out of a rectangular cover.
    // Now the cover rect interpolates from the tile's real rect to a square, and the vinyl grows from 0.
    const float e = discK * discK * (3.0f - 2.0f * discK);      // smoothstep
    const float w0 = r.right - r.left;
    const float h0 = r.bottom - r.top;
    const float st = (w0 + h0) * 0.5f;                          // square side the disc settles on
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float R  = st * 0.5f;

    const float vr = R * e;                                     // vinyl radius: 0 → R
    if (vr > 0.5f) FillCircle(cx, cy, vr, 0x2A2026, 1.0f);

    // Cover: starts as the tile's own rectangle, ends as the 1.32R inner square of the disc
    const float artSide = R * 1.32f;
    const float cw = w0 + (artSide - w0) * e;
    const float ch = h0 + (artSide - h0) * e;
    const float radius = UI::R_TILE + (artSide * 0.5f - UI::R_TILE) * e;
    DrawCoverArt(Rc(cx - cw * 0.5f, cy - ch * 0.5f, cw, ch), s, idx, radius);

    // While still close to a tile, the song info fades out with it
    if (infoK > 0.0f && e < 0.55f) {
        float ia = infoK * (1.0f - e / 0.55f);
        if (ia > 0.0f) DrawTileInfo(r, s, ia);
    }
    // Grooves and center hole: fade in together with the disc
    if (e > 0.15f) {
        float k = (e - 0.15f) / 0.85f;
        StrokeCircle(cx, cy, vr * 0.93f, 0xFFFFFF, 0.05f * k, 1.0f);
        StrokeCircle(cx, cy, vr * 0.78f, 0xFFFFFF, 0.05f * k, 1.0f);
        FillCircle(cx, cy, R * 0.05f, UI::BG, 0.9f * k);
        StrokeCircle(cx, cy, R * 0.05f, 0xFFFFFF, 0.18f * k, 1.0f);
    }
}

static void DrawSpinner(float cx, float cy, float r, float ang, float alpha);

/** For transitions: scan the layout directly for tiles in the viewport, without relying on the band index (the band table may be empty or stale) */
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

/** Transition into the player page (draws only until the main disc is in place, then DrawPlayerPage takes over) */
static void DrawEntering() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.songs.size();
    const bool hasSel = (g_enterIdx >= 0 && g_enterIdx < n);
    if (n == 0) return;                 // no songs at all, nothing to scatter

    const float lineY = UI::TITLE_H + 4.0f;
    const float dd = (std::min)(W * 0.42f, H * 0.62f);
    const float cx = dd * 0.10f;
    const float cy = (lineY + H) * 0.5f;
    D2D1_RECT_F discRect = Rc(cx - dd * 0.5f, cy - dd * 0.5f, dd, dd);

    const float t = g_enterT;
    auto ease = [](float x) { return x * x * (3.0f - 2.0f * x); };
    auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    // ── 1. Other tiles turn into CDs: fly straight up / down from where they are ──
    // Note: the grid layer isn't drawn during the transition, so they must be drawn in place from frame 0
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

            // the farther from the center axis, the later it starts
            float sx = std::fabs(c0x - W * 0.5f) / (W * 0.5f);
            float eo = clamp01((t - 0.10f - sx * 0.10f) / 0.50f);
            if (eo >= 1.0f) continue;                  // already flown away

            float e = (eo <= 0.0f) ? 0.0f : ease(eo);
            float dirY = (c0y < cy) ? -1.0f : 1.0f;
            float tx = W * 0.5f;                                   // straight up / down
            float ty = (dirY < 0) ? (-dd * 0.75f) : (H + dd * 0.75f);

            float nx = c0x + (tx - c0x) * e;
            float ny = c0y + (ty - c0y) * e;
            // Do NOT snap to a square on frame 0: a rectangular tile kept its width and
            // jumped to a square the instant the transition started. Interpolate the real
            // tile rect toward the square instead, so the shape change is part of the motion.
            float sz = (std::min)(bx.w, bx.h) * (1.0f - 0.32f * e);
            float wNow = bx.w + (sz - bx.w) * e;
            float hNow = bx.h + (sz - bx.h) * e;
            DrawMorphDisc(Rc(nx - wNow * 0.5f, ny - hNow * 0.5f, wNow, hNow),
                          a.songs[i], i, clamp01(eo * 1.35f), 0.0f);
        }
    }

    // ── 2. The selected one: move it to the center and wait; only once all the other CDs have flown away does it morph into the disc ──
    //    When entered from the top button and there is no current song this tile doesn't exist, so the whole block is skipped
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
            r = k1;                 // rest at the center, waiting for the others to clear out
            discK = 0.0f;
        } else {
            float p = clamp01((t - 0.66f) / 0.14f);
            float pe = ease(p);
            r = LerpRect(k1, discRect, pe);
            discK = pe;
        }
        DrawMorphDisc(r, a.songs[(size_t)g_enterIdx], g_enterIdx, discK, 1.0f);
    }

    // ── 3. Divider ──
    {
        float e = g_lineT * g_lineT * (3.0f - 2.0f * g_lineT);
        float lw = W * e;
        if (lw > 0.5f) Line(W - lw, lineY, W, lineY, 0xC9AEB8, 1.0f);
    }
}


/** Ring loader: an arc that gradually brightens, spun as a spinner */
/* Minimal loader ring: a thin track + one sweeping arc, round caps, no glow */
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
    g_brush->SetOpacity(1.0f);          // brush is shared, must be reset after use
    if (ss) ss->Release();
    geo->Release();
}

static void DrawSpinner(float cx, float cy, float r, float ang, float alpha) {
    const float w = 2.0f;
    // track: full circle, very faint
    StrokeCircle(cx, cy, r, UI::ACCENT, alpha * 0.13f, w);
    // sweeping arc: 100°, round caps
    StrokeArc(cx, cy, r, ang, 100.0f, UI::ACCENT, alpha * 0.95f, w);
}
/** Arbitrary filled polygon */
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
    g_brush->SetOpacity(1.0f);          // same as above
    geo->Release();
}

/** Speaker icon: body drawn as one closed path, sound waves as knob-style arcs */
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

    if (muted || vol <= 0.02f) {                 // muted: cross
        Line(cx + s * 0.55f, cy - s * 0.5f, cx + s * 1.5f, cy + s * 0.5f, UI::TEXT3, 1.7f);
        Line(cx + s * 1.5f, cy - s * 0.5f, cx + s * 0.55f, cy + s * 0.5f, UI::TEXT3, 1.7f);
        return;
    }
    // one to three sound waves: radius increases, opacity decreases
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

/** Filled triangle (legacy interface kept) */
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
    g_brush->SetOpacity(1.0f);          // same as above
    geo->Release();
}

/** Volume button + pop-up slider */
static void DrawVolume(float px, float by, float cy) {
    D2D1_RECT_F vb = Rc(px + 186, by, 40, 40);
    bool hv = Inside(vb, g_app.mouse);
    if (hv || g_volOpen) FillRound(vb, 20.0f, UI::HOVER);

    // speaker icon
    DrawSpeakerIcon(vb.left + 19.0f, by + 20.0f, 8.0f, g_volMuted, g_userVol);
    Hit hv2; hv2.r = vb; hv2.kind = HK_VOL;
    if (g_alpha > 0.5f) g_app.hits.push_back(hv2);


    // ── Quality button (right of volume) ──
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
        if (!avail) {                                  // this song doesn't have this tier
            Text(T(L"不可用", L"Unavailable"), Fmt(10.0f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(row.right - 52, row.top, 44, rh), 0xC9B8BE,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            Hit hqi; hqi.r = row; hqi.kind = HK_QUAL_ITEM; hqi.index = i;
            if (g_alpha > 0.5f) g_app.hits.push_back(hqi);
        }
    }
    }   // ← quality panel ends here

#ifndef FLORAL_NO_TOGETHER
    // ── Together Listening button (right of quality) ──
    {
        D2D1_RECT_F tb2 = Rc(qb.right + 10.0f, by, 40.0f, 40.0f);
        bool th2 = Inside(tb2, g_app.mouse);
        bool on = TogetherActive();
        if (th2 || g_togOpen) FillRound(tb2, 20.0f, UI::HOVER);
        UINT32 icol = on ? UI::ACCENT : UI::TEXT2;
        const float ccx = tb2.left + 20.0f, ccy = by + 22.0f;
        // headset: upper semicircle + earcups on both sides
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
            {   // swallow clicks on the card's empty area so it doesn't close on any touch
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
                // first row: my connection code
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

                // second row: enter the peer's connection code
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

    if (g_volT <= 0.01f) return;         // ↓ volume panel; the two don't affect each other
    PanelFx fxv(g_volT, vb.left + 20.0f, by + 20.0f);

    // pop-up card
    const float pw = 190.0f, ph = 54.0f;
    // open to the right of the button so it doesn't cover the progress bar and time; flip to the left if there's no room on the right
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

/** Leaving the player page: fade the whole screen out, then let the grid fly in from the right (same sequence as the splash) */
static void DrawExiting() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.songs.size();
    const float t = g_exitT;

    // ── Phase A: fade the whole player page to the background color ──
    if (t < 0.22f) {
        DrawPlayerPage();
        g_alpha = 1.0f;
        DrawTitleBar(W);                 // Fade the title bar out too, or it vanishes with a pop the moment Back is clicked
        a.hits.clear();
        FillRect(Rc(0, 0, W, H), UI::BG, t / 0.22f);
        return;
    }
    if (n == 0) return;

    // ── Phase B: the grid flies in from the right ──
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

    // DrawUI draws the title bar opaquely on top; don't duplicate it with a translucent one here
}

static void DrawPlayerPage() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.songs.size();
    if (a.current < 0 || a.current >= n) {
        // ── No song selected: blank record + hint ──
        const float lineY = UI::TITLE_H + 4.0f;
        const float dd = (std::min)(W * 0.42f, H * 0.62f);
        const float cx = dd * 0.10f;
        const float cy = (lineY + H) * 0.5f;
        {
            // Blank record: vinyl disc + a few grooves + center label (no cover art)
            float r = dd * 0.5f;
            // Empty record: base color / grooves / center label all follow the theme
            FillCircle(cx, cy, r, UI::DISC);
            for (int i = 1; i <= 4; i++) {
                float rr = r * (0.50f + 0.11f * (float)i);
                StrokeRound(Rc(cx - rr, cy - rr, rr * 2.0f, rr * 2.0f), rr,
                            UI::DISC_GROOVE, 1.0f, 0.5f);
            }
            FillCircle(cx, cy, r * 0.33f, MixColor(UI::SELECT, UI::PANEL, 0.35f));
            StrokeRound(Rc(cx - r * 0.33f, cy - r * 0.33f, r * 0.66f, r * 0.66f),
                        r * 0.33f, UI::BTN_CLOSE, 1.2f);
            FillCircle(cx, cy, 4.5f, UI::PANEL);           // spindle hole
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

    // Back button (above the info column on the right; top-left is kept for the album box)
    D2D1_RECT_F back = Rc(W * 0.46f - 6.0f, UI::TITLE_H + 18.0f, 96, 28);
    bool bh = Inside(back, a.mouse);
    if (bh) FillRound(back, UI::R_BTN, UI::HOVER);
    Line(back.left + 10, back.top + 14, back.left + 18, back.top + 7, UI::TEXT2, 1.6f);
    Line(back.left + 10, back.top + 14, back.left + 18, back.top + 21, UI::TEXT2, 1.6f);
    Text(T(L"返回  Esc", L"Back  Esc"), Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
         Rc(back.left + 26, back.top, 70, 28), UI::TEXT2);
    Hit hb; hb.r = back; hb.kind = HK_BACK;
    if (g_alpha > 0.5f) a.hits.push_back(hb);

    // ── Left: main CD centered; prev / next shrink into small CDs tucked in the top-left and bottom-left corners, only a sliver showing ──
    const float lineY = UI::TITLE_H + 4.0f;
    const float dd = (std::min)(W * 0.42f, H * 0.62f);
    const float cx = dd * 0.10f;                            // disc center sits slightly past the left edge
    const float cy = (lineY + H) * 0.5f;

    // Rest of the transition: corners pop in, right side fades in
    float cornerK;
    {
        float k = (g_enterT - 0.80f) / 0.20f;
        k = (k < 0.0f) ? 0.0f : (k > 1.0f ? 1.0f : k);
        float ek = 1.0f - powf(1.0f - k, 3.0f);
        // Overshoot a little, then settle: 0 → 118% → 100%
        float pop = (ek <= 0.75f) ? (ek / 0.75f) * 1.18f
                                  : 1.18f - 0.18f * ((ek - 0.75f) / 0.25f);
        cornerK = (k <= 0.0f) ? 0.0f : pop;
    }
    float infoK = (g_enterT - 0.90f) / 0.10f;
    infoK = (infoK < 0.0f) ? 0.0f : (infoK > 1.0f ? 1.0f : infoK);

    int prevIdx = (a.current - 1 + n) % n;
    int nextIdx = (a.current + 1) % n;

    D2D1_RECT_F discRect = Rc(cx - dd * 0.5f, cy - dd * 0.5f, dd, dd);

    // Corner mini CD: center pushed into the corner, showing a bit more than a quarter
    const float sr = (std::max)(28.0f, dd * 0.115f);
    D2D1_RECT_F discPrev = Rc(sr * 0.25f - sr, lineY + sr * 0.25f - sr, sr * 2, sr * 2);
    D2D1_RECT_F discNext = Rc(sr * 0.25f - sr, H - sr * 0.25f - sr, sr * 2, sr * 2);

    const bool switching = (g_switchT < 1.0f) && (g_switchFrom >= 0) && (g_switchFrom < n);

    // Corners pop in from nothing during the transition
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

    // The two small corner discs (clipped below the divider so they don't cover the title)
    // Top corner: keep the "old disc" while switching, swap after the outgoing disc lands on it — exactly when it's hidden, so no flicker
    int cornerPrev = prevIdx;
    if (switching && g_casePrev0 >= 0 && g_casePrev0 < n) cornerPrev = g_casePrev0;

    // Flight start/end points: fully off-screen, so discs "spin in from / out to nowhere"
    D2D1_RECT_F offPrev = Rc(-sr * 2.2f, lineY - sr * 2.2f, sr * 2, sr * 2);
    D2D1_RECT_F offNext = Rc(-sr * 2.2f, H + sr * 1.2f,    sr * 2, sr * 2);

    g_rt->PushAxisAlignedClip(Rc(0, lineY, W, H - lineY), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (switching) {
        float t = g_switchT;
        float e = t * t * (3.0f - 2.0f * t);              // smoothstep

        const bool fwd = (g_switchDir > 0);

        // Where the centered disc goes / where the new one comes from
        D2D1_RECT_F toCorner   = fwd ? discPrev : discNext;
        D2D1_RECT_F fromCorner = fwd ? discNext : discPrev;
        // The disc being pushed out / the one sliding in
        D2D1_RECT_F pushOut = fwd ? discPrev : discNext;
        D2D1_RECT_F slideIn = fwd ? discNext : discPrev;
        D2D1_RECT_F outOff  = fwd ? offPrev  : offNext;
        D2D1_RECT_F inOff   = fwd ? offNext  : offPrev;

        int pushedIdx = fwd ? g_casePrev0 : g_caseNext0;
        int slideIdx  = fwd ? nextIdx : prevIdx;
        if (pushedIdx < 0 || pushedIdx >= n) pushedIdx = slideIdx;

        // All angles converge to g_discAngle; each disc spins one extra full turn in flight
        float angOut = g_discAngle + 360.0f * e;
        float angIn  = g_discAngle + 360.0f * (1.0f - e);

        // 1) Oldest corner disc: yields, spins off-screen
        DrawDisc(LerpRect(pushOut, outOff, e), a.songs[(size_t)pushedIdx], pushedIdx, angOut, 1.0f);
        // 2) New disc: comes in from off-screen to the corner
        DrawDisc(LerpRect(inOff, slideIn, e), a.songs[(size_t)slideIdx], slideIdx, angIn, 1.0f);
        // 3) Centered disc: shrinks into the corner
        DrawDisc(LerpRect(discRect, toCorner, e), a.songs[(size_t)g_switchFrom],
                 g_switchFrom, angOut, 1.0f);
        // 4) Corner disc: grows and spins into the center (topmost)
        DrawDisc(LerpRect(fromCorner, discRect, e), s, a.current, angIn, 1.0f);
    } else {
        if (cornerK > 0.01f) {
            // Corner mini discs stay still; click one to switch to that song
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

    // While loading, leave the disc alone; just spin a small ring on the play button
    const bool showSpin = g_audioLoading && (GetTickCount64() - g_loadingSince > 180);
    {
        float e = g_lineT * g_lineT * (3.0f - 2.0f * g_lineT);
        float lw = W * e;
        if (lw > 0.5f) Line(W - lw, lineY, W, lineY, 0xC9AEB8, 1.0f);
    }

    // ── Right: song info ──
    const float px = (std::max)(W * 0.46f, cx + dd * 0.5f + 90.0f);
    const float pw = W - px - UI::PAD * 2;
    g_alphaMul = infoK;
    if (pw > 80.0f) {
        // Three rows: next slides out left and in from the right; prev is reversed. Rows staggered by 30ms
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
                // Both offset and alpha are eased: enter decelerates (ease-out), exit accelerates (ease-in)
                float e = entering ? (1.0f - powf(1.0f - t, 3.0f)) : (t * t);
                float a = entering ? e : (1.0f - e);
                if (a <= 0.01f) continue;
                float dx = entering ? (1.0f - e) * SLIDE * (float)g_textDir
                                    : -e * SLIDE * (float)g_textDir;
                dx = floorf(dx + 0.5f);          // Round to integers: subpixels make glyphs jitter every frame
                Text(*rows[i].s, Fmt(rows[i].size, rows[i].w),
                     Rc(px + dx, rows[i].y, pw, rows[i].size + 12.0f),
                     rows[i].c, DWRITE_TEXT_ALIGNMENT_LEADING, a);
            }
        };



        if (g_textPhase == 1) {
            drawSet(g_outTitle, g_outArtist, g_outAlbum, g_textT, false);   // Old text slides out
        } else if (g_textPhase == 2) {
            drawSet(s.title, s.artist, s.album, g_textT, true);             // New text slides in
        } else {
            drawSet(s.title, s.artist, s.album, 10.0f, true);               // Idle: fully shown
        }

        if (g_audioError) {
            Text(g_audioErrKind == 3 ? T(L"播放中断 · 网络错误", L"Stopped · network error")
                 : g_audioErrKind == 2 ? T(L"无法播放 · 解码失败", L"Cannot play · decode failed")
                 : T(L"无法播放 · 地址解析失败", L"Cannot play · URL failed"), Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL),
                 Rc(px, cy - 10, pw, 18), UI::ACCENT);
        }

        // Progress
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

        // Previous / Play / Next
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
            // Cross-fade between pause (two bars) and play (triangle) over 120ms
            // Play ⇄ Pause: geometric morph. Two quads interpolate simultaneously,
            // the triangle's right vertex splits and grows into the right bar, while the body shrinks into the left bar.
            // No fading, so there is always exactly one solid shape and never an empty frame.
            {
                float e = g_playIconMix;
                e = e * e * (3.0f - 2.0f * e);            // smoothstep: soft start and finish
                auto L = [&](float a, float b) { return a + (b - a) * e; };

                // Left quad: triangle (duplicated vertex) → left bar
                D2D1_POINT_2F left[4] = {
                    D2D1::Point2F(bx + L(-6.0f, -6.0f), byy + L(-9.5f, -8.0f)),
                    D2D1::Point2F(bx + L( 8.0f, -2.6f), byy + L( 0.0f, -8.0f)),
                    D2D1::Point2F(bx + L( 8.0f, -2.6f), byy + L( 0.0f,  8.0f)),
                    D2D1::Point2F(bx + L(-6.0f, -6.0f), byy + L( 9.5f,  8.0f)),
                };
                // Right quad: collapsed into the triangle's vertex (invisible) → right bar
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
    g_alphaMul = 1.0f;      // Restore, so the title bar isn't affected
}

/* ── Playlist selected: tracks load on a worker thread, the UI thread swaps them in ──
   Tracks are cached per version as pl-<gcid>.<list_ver>.tsv; an older copy shows until the current one arrives */
static std::string PlSafe(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    return r;
}

static std::wstring PlTracksFile(const std::string& gcid, int ver) {
    const std::string safe = PlSafe(gcid);          // Note: don't call begin/end on two temporaries
    return L"pl-" + std::wstring(safe.begin(), safe.end()) + L"." + std::to_wstring(ver) + L".tsv";
}

/* Names of every cached track file of a playlist, including the unversioned pl-<gcid>.tsv of older builds */
static std::vector<std::wstring> PlCachedTracks(const std::string& gcid) {
    const std::string safe = PlSafe(gcid);
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((AssetDir() + L"\\pl-" + std::wstring(safe.begin(), safe.end()) + L".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do names.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}

/* Version in a cached track file's name: -1 for the unversioned name of older builds, INT_MIN for anything
   that is not a track list, such as the .part file of an interrupted write */
static int PlFileVer(const std::wstring& name) {
    if (name.size() < 4 || name.compare(name.size() - 4, 4, L".tsv") != 0) return INT_MIN;
    const size_t ext = name.size() - 4;
    const size_t dot = name.rfind(L'.', ext - 1);       // PlSafe leaves no dots in the gcid part
    return dot == std::wstring::npos ? -1 : _wtoi(name.substr(dot + 1, ext - dot - 1).c_str());
}

/* Newest cached copy as (version, file name); INT_MIN when there is none */
static std::pair<int, std::wstring> PlNewestCopy(const std::string& gcid) {
    std::pair<int, std::wstring> newest{ INT_MIN, L"" };
    for (const std::wstring& name : PlCachedTracks(gcid)) {
        const int ver = PlFileVer(name);
        if (ver > newest.first) newest = { ver, name };
    }
    return newest;
}

struct PlaylistTracks {
    int seq = 0;                // open request this answers; 0 when it updates the playlist on screen
    int idx = -1;
    std::string gcid;
    int ver = 0;                // version of these tracks; 0 when unknown
    std::wstring file;
    std::vector<Song> songs;
    std::wstring err;           // nothing could be loaded
};

static std::mutex                  g_plSwapMx;
static std::vector<PlaylistTracks> g_plArrived;          // written by workers, taken by the UI thread
static std::mutex                  g_plFetchMx;          // one fetch at a time, so two never write the same files

/* UI thread only */
static int  g_plOpenSeq = 0;                              // latest open request
static bool g_plWaiting = false;                          // the latest open request has no result yet
static std::optional<PlaylistTracks> g_plOpened, g_plUpdated;
static std::string g_plShownGcid;                         // playlist whose tracks are in g_app.songs,
static int         g_plShownVer = 0;                      // and the version of those tracks
static std::string g_plWantGcid;                          // playlist on screen or being opened,
static int         g_plWantVer = 0;                       // and the newest version requested for it

/* seq > 0 opens the playlist; seq == 0 only fetches its current version for the playlist on screen */
static void LoadPlaylistAsync(int idx, int seq) {
    const Playlist& pl = g_app.playlists[(size_t)idx];
    std::thread([idx, seq, gcid = pl.gcid, ver = pl.ver, count = pl.count,
                 token = g_token, userid = g_userid]() mutable {
        auto deliver = [&](int tracksVer, const std::wstring& file, std::vector<Song> songs, const std::wstring& err) {
            PlaylistTracks t;
            t.seq = seq;
            t.idx = idx;
            t.gcid = gcid;
            t.ver = tracksVer;
            t.file = file;
            t.songs = std::move(songs);
            t.err = err;
            AdoptLocalCovers(t.songs);
            std::lock_guard<std::mutex> lk(g_plSwapMx);
            g_plArrived.push_back(std::move(t));
            g_coverDirty = true;
        };

        // Version 0 means unknown, so no cached copy counts as current
        const std::wstring file = PlTracksFile(gcid, ver);
        if (seq > 0) {
            std::vector<Song> songs = (ver > 0) ? ReadSongs(file) : std::vector<Song>();
            if (!songs.empty()) {
                deliver(ver, file, std::move(songs), L"");
                return;
            }
            const auto [oldVer, old] = PlNewestCopy(gcid);
            if (!old.empty()) songs = ReadSongs(old);
            if (!songs.empty()) {
                deliver(oldVer, old, std::move(songs), L"");
                seq = 0;                // the fetched tracks below replace this copy
            }
        }

        std::lock_guard<std::mutex> lk(g_plFetchMx);
        const auto [newestVer, newest] = PlNewestCopy(gcid);   // another load may have fetched it while this one waited
        if (ver > 0 && newestVer >= ver) {
            std::vector<Song> songs = ReadSongs(newest);
            if (!songs.empty()) {
                deliver(newestVer, newest, std::move(songs), L"");
                return;
            }
        }
        EnsureSidecar(0);               // the Kugou service may still be starting right after launch
        std::wstring err;
        const ULONGLONG start = GetTickCount64();
        const int n = KgExportPlaylist(token, userid, gcid, count, AssetDir() + L"\\" + file, err);
        std::vector<Song> songs = (n > 0) ? ReadSongs(file) : std::vector<Song>();
        if (songs.empty()) {
            if (err.empty()) err = L"读不出刚拉取的歌单";
            Logf("ERR", "拉取歌单失败: %s", Narrow(err).c_str());
            deliver(ver, L"", {}, err);
            return;
        }
        Logf("APP", "拉取歌单 %d 首，用时 %llu ms", n, GetTickCount64() - start);
        for (const std::wstring& name : PlCachedTracks(gcid))
            if (PlFileVer(name) < ver) DeleteFileW((AssetDir() + L"\\" + name).c_str());
        deliver(ver, file, std::move(songs), L"");
    }).detach();
}

static void SelectPlaylistAsync(int idx) {
    if (idx < 0 || idx >= (int)g_app.playlists.size()) return;
    const Playlist& pl = g_app.playlists[(size_t)idx];
    Logf("APP", "切换歌单 idx=%d %s", idx, Narrow(pl.name).c_str());
    g_plWantGcid = pl.gcid;
    g_plWantVer = pl.ver;
    g_plWaiting = true;
    LoadPlaylistAsync(idx, ++g_plOpenSeq);
}

/* Drops an open request whose transition was left; its tracks are ignored when they arrive */
static void CancelPlaylistOpen() {
    ++g_plOpenSeq;
    g_plWaiting = false;
    g_plOpened.reset();
    g_plWantGcid = g_plShownGcid;
    g_plWantVer = g_plShownVer;
}

/* g_app.songs was reloaded from a file that belongs to no playlist, such as the likes of another source */
static void ForgetShownPlaylist() {
    g_plShownGcid.clear();
    g_plWantGcid.clear();
}

/* Runs whenever the playlist list arrives: fetches the playlist on screen again when the list has a newer
   version than requested so far, which also retries a fetch that failed */
static void CheckShownPlaylist() {
    for (int i = 0; i < (int)g_app.playlists.size(); i++) {
        const Playlist& pl = g_app.playlists[(size_t)i];
        if (pl.gcid != g_plWantGcid) continue;
        if (pl.ver > g_plWantVer) {
            g_plWantVer = pl.ver;
            LoadPlaylistAsync(i, 0);
        }
        break;
    }
}

/* Replaces g_app.songs with a loaded track list and resets the state that indexes into the old list */
static void ShowTracks(PlaylistTracks& t) {
    App& a = g_app;
    g_likesName = t.file;
    a.songs = std::move(t.songs);
    a.tileAnim.assign(a.songs.size(), 0.0f);
    g_covFlag.assign(a.songs.size(), 0);
    g_prewarmIdx = 0;
    { std::lock_guard<std::mutex> lk(g_preMx); g_urlCache.clear(); }
    a.current = -1;
    a.hoverTile = -1;
    g_plShownGcid = t.gcid;
    g_plShownVer = t.ver;
    EnsureBase(a.width, (int)a.songs.size());
    EnsureTarget(a.width, (int)a.songs.size(), -1);
}

/* Main thread: swap in the loaded playlist */
static void DrainPlaylistSwap() {
    {
        std::lock_guard<std::mutex> lk(g_plSwapMx);
        for (PlaylistTracks& t : g_plArrived) {
            if (t.seq > 0) {
                if (t.seq != g_plOpenSeq) continue;          // the open was cancelled or superseded
                g_plOpened = std::move(t);
                g_plWaiting = false;
            } else if (t.gcid != g_plWantGcid) {
                continue;
            } else if (!t.err.empty()) {
                // Lowered so the next arrival of the playlist list asks for this version again
                if (t.ver == g_plWantVer) g_plWantVer = (t.gcid == g_plShownGcid) ? g_plShownVer : 0;
            } else if (!g_plUpdated || t.ver > g_plUpdated->ver) {
                g_plUpdated = std::move(t);
            }
        }
        g_plArrived.clear();
    }

    App& a = g_app;
    if (g_plOpened && g_plPhase != 1) {                 // the picker finishes its exit first
        PlaylistTracks t = std::move(*g_plOpened);
        g_plOpened.reset();
        if (!t.err.empty()) {
            Logf("ERR", "切换歌单失败: %s", Narrow(t.err).c_str());
            g_plWantGcid = g_plShownGcid;
            g_plWantVer = g_plShownVer;
            g_plPhase = 0;                 // abort the transition, stay on the picker
            g_plT = 0.0f;
            g_gridEnter = 1.0f;
            return;
        }
        ShowTracks(t);
        a.playing = false;
        a.pos = 0.0;
        a.scroll = 0.0f;
        a.scrollTarget = 0.0f;
        a.pageAnim = 0.0f;
        a.page = Page::Grid;
        a.plActive = t.idx;
        // Hold the fly-in until the loading ring has faded out (phase 3 finishes it)
        g_gridEnter = (g_plPhase == 2) ? -1.0f : 0.0f;
        g_swapDone = true;
        g_coverDirty = true;
        Logf("APP", "歌单已切换：%d 首", (int)a.songs.size());
    }

    if (g_plUpdated && !g_plOpened) {
        if (a.page == Page::Grid && (g_plPhase != 0 || g_gridEnter < 1.0f)) return;   // let the fly-in land first
        PlaylistTracks& t = *g_plUpdated;
        // Updates with an unknown version (0) come straight from a fetch, so they are the newest too
        if (t.gcid == g_plShownGcid && (t.ver > g_plShownVer || t.ver == 0)) {
            // Once a song is picked, its index must stay valid; the next open reads the new tracks from the cache
            if (a.page == Page::Grid && a.current < 0) {
                ShowTracks(t);
                Logf("APP", "歌单已更新：%d 首", (int)a.songs.size());
            } else {
                g_likesName = t.file;           // older copies are deleted, so a reload by InitData reads this one
            }
        }
        g_plUpdated.reset();
    }
}
/* ══════════════════════════════════════════
   Playlist picker: circular honeycomb layout, draggable
   ══════════════════════════════════════════ */
static const float PL_D      = 96.0f;               // Disc diameter
static const float PL_PILL_W = 300.0f;              // Width of the expanded pill
static const float PL_GAP    = 1.16f;               // Leave enough breathing room between discs
static const float PL_SQ3    = 1.7320508f;

/* Hexagonal layout: ring 0 is the center, expanding outward;
   coordinates are axial (q, r), adjacent centers are one diameter apart */
struct PlCell { float x, y; };
static std::vector<PlCell> g_plCells;
static int                 g_plCellsFor = -1;

static void BuildPlCells(int n) {
    g_plCells.clear();
    g_plCellsFor = n;
    if (n <= 0) return;

    const float step = PL_D * PL_GAP;       // Distance between adjacent centers

    // Ring 0: the center
    g_plCells.push_back({ 0.0f, 0.0f });

    int left = n - 1;
    for (int ring = 1; left > 0; ring++) {
        int cap = 6 * ring;                 // How many fit in this ring
        int take = (left < cap) ? left : cap;
        if (take == cap) {
            // Full ring: walk the hexagonal lattice so neighbors stay equidistant
            // Start at (ring, 0), then advance along the 6 edges in order
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
            // Partial ring: take all cells in this ring, sort by angle, then pick `take` at equal intervals
            // This keeps cells on the hexagonal lattice with even spacing, and the whole stays a symmetric hexagon
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

/* Pan limits: outermost centers can be dragged at most to the window edge (the polygon of centers is the boundary) */
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

    // The test point is the window center: while dragging it must stay inside the outermost ring's centers
    // On screen the center is always (0,0) in content coords, i.e. require minX+plX <= 0 <= maxX+plX
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

/* Center the content when entering the playlist page */
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
    g_plLastX = g_app.plX;           // a jump, not motion: no blur on the next frame
    g_plLastY = g_app.plY;
}

/* Offset by which other discs are pushed away after selection */
/* On-screen center of disc i (before push-away) */
static void PlScreenCenter(int i, float& cx, float& cy) {
    App& a = g_app;
    float x = 0, y = 0;
    PlPos(i, x, y);
    cx = a.width * 0.5f + x + a.plX;
    cy = UI::TITLE_H + (a.height - UI::TITLE_H) * 0.5f + y + a.plY;
}

/* Screen rect of the expanded pill card (empty rect when nothing is selected) */
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

/* Target rect of the expanded card (window center) */
static D2D1_RECT_F PlCardTargetRect() {
    const float W = g_app.width, H = g_app.height;
    const float cardW = (std::min)(620.0f, W - 80.0f);
    const float cardH = 150.0f;
    const float cx = W * 0.5f;
    const float cy = UI::TITLE_H + (H - UI::TITLE_H) * 0.5f;
    return Rc(cx - cardW * 0.5f, cy - cardH * 0.5f, cardW, cardH);
}

/* Final on-screen centers of all discs this frame (with yield + de-overlap), shared by drawing and hit-testing */
static std::vector<D2D1_POINT_2F> g_plPos;
static int g_plPosFor = -1;

/* Container Transform: other discs don't take part in the layout; just fade them all out on selection */
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
/* Final on-screen center of disc i */
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

/* Centred loading ring, shared by the splash and the picker -> song transition */
static void DrawPlLoader(float W, float H, float alpha, float scale = 1.0f) {
    if (alpha <= 0.01f) return;
    FillRect(Rc(0, (float)UI::TITLE_H, W, H - (float)UI::TITLE_H), UI::BG, alpha);
    DrawSpinner(W * 0.5f, H * 0.5f, 20.0f * scale, g_plSpin, 0.95f * alpha);
}
static void DrawPlaylistPicker() {
    App& a = g_app;
    const float W = a.width, H = a.height;
    const int n = (int)a.playlists.size();

    if (n == 0) {
        // Empty state: a faint disc outline plus two lines, so the page still has a subject
        const float cyE = H * 0.44f;
        StrokeCircle(W * 0.5f, cyE, 34.0f, UI::BORDER, 0.55f, 1.6f);
        StrokeCircle(W * 0.5f, cyE, 9.0f, UI::BORDER, 0.55f, 1.6f);
        Text(T(L"还没有歌单数据", L"No playlists yet"), Fmt(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             Rc(0, cyE + 20, W, 24), UI::TEXT2, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(T(L"登录后会自动同步你的自建歌单", L"Your playlists sync after sign-in"),
             Fmt(12.0f, DWRITE_FONT_WEIGHT_NORMAL),
             Rc(0, cyE + 46, W, 20), UI::TEXT3, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    const float R = PL_D * 0.5f;

    PlBuildPositions();          // Final positions of all discs this frame (yield + de-overlap)

    // ── Container Transform: the selected disc grows into a card, others fade out ──
    float selE = 0.0f;
    D2D1_RECT_F cardR{};
    float cardRadius = 0.0f;
    float avatarCx = 0.0f, avatarCy = 0.0f;
    // Keep drawing during collapse: plActive clears first, but the card still has to fade away
    const int selIdx = (a.plActive >= 0) ? a.plActive : a.plSelLast;
    if (selIdx >= 0 && selIdx < n && a.plSelT > 0.004f) {
        selE = a.plSelT;
        selE = selE * selE * (3.0f - 2.0f * selE);

        // Collapsing: fade the whole card out with the same curve, so it dissolves instead of snapping off
        const float cardA = (a.plActive >= 0) ? 1.0f : selE;
        const float keepMul = g_alphaMul;
        g_alphaMul = keepMul * cardA;

        float ccx = 0, ccy = 0;
        PlScreenPos(selIdx, ccx, ccy);
        const Playlist& spl = a.playlists[(size_t)selIdx];

        // Target card: window center, 620 wide, 150 high
        D2D1_RECT_F from = Rc(ccx - R, ccy - R, R * 2, R * 2);
        D2D1_RECT_F to   = PlCardTargetRect();
        const float cardCY = (to.top + to.bottom) * 0.5f;

        auto L = [&](float p, float q) { return p + (q - p) * selE; };
        // Interpolate the four edges first, then convert to the width/height Rc needs
        float cardL = L(from.left, to.left);
        float cardT = L(from.top, to.top);
        float cardRt = L(from.right, to.right);
        float cardB = L(from.bottom, to.bottom);
        cardR = Rc(cardL, cardT, cardRt - cardL, cardB - cardT);
        cardRadius = L(R, 26.0f);

        // Darken the background so the card reads as a raised surface
        if (selE > 0.02f) FillRect(Rc(0, UI::TITLE_H, W, H - UI::TITLE_H), 0x000000, 0.34f * selE);

        // Two stacked shadows: a tight one for contact, a wider one for elevation.
        // They fade in with the card so the surface appears to rise, not merely appear.
        {
            float cw2 = cardR.right - cardR.left;
            float ch2 = cardR.bottom - cardR.top;
            FillRound(Rc(cardR.left + 2.0f, cardR.top + 10.0f, cw2, ch2),
                      cardRadius + 1.0f, UI::SHADOW, 0.16f * selE);
            FillRound(Rc(cardR.left - 6.0f, cardR.top + 26.0f, cw2 + 12.0f, ch2),
                      cardRadius + 2.0f, UI::SHADOW, 0.09f * selE);
        }

        FillRound(cardR, cardRadius, UI::CARD, 0.99f);
        StrokeRound(cardR, cardRadius, UI::BORDER, 0.80f, 1.0f);

        // Cover: slides from the disc center to the avatar slot on the card's left
        const float avR = R * 0.92f;
        float avToX = to.left + 30.0f + avR;
        float avToY = cardCY;
        avatarCx = L(ccx, avToX);
        avatarCy = L(ccy, avToY);

        // Text: title starts at 0.15 / track count at 0.42 / hint at 0.60
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

        g_alphaMul = keepMul;
    }

    // Draw the cover as a circle (also used as the selected item's avatar)
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

    // Unselected discs: fade out as the card expands ("fade everything else" of Container Transform)
    float othersA = 1.0f - selE * 0.92f;
    BeginMotionBlur(g_plBlur);
    for (int i = 0; i < n; i++) {
        if (i == a.plActive) continue;                 // The selected one is represented by the card avatar instead
        float cx = 0, cy = 0;
        PlScreenPos(i, cx, cy);
        if (cx < -R * 1.4f || cx > W + R * 1.4f) continue;
        if (cy < UI::TITLE_H - R * 1.4f || cy > H + R * 1.4f) continue;
        if (othersA <= 0.02f) continue;

        // Hover lifts the disc slightly (1.035x); press pushes it back (0.97x).
        // That pair is the whole feedback budget for a grid this dense.
        float hv = (i < (int)a.plHoverT.size()) ? a.plHoverT[(size_t)i] : 0.0f;
        float press = (a.plPress == i) ? 1.0f : 0.0f;
        float rd = R * (1.0f + 0.035f * hv - 0.03f * press);

        float keep = g_alphaMul;
        g_alphaMul = keep * othersA;

        // Soft drop shadow: one offset disc reads as depth; a real blur is not worth the cost here
        FillCircle(cx, cy + 3.0f, rd * 1.02f, UI::SHADOW, 0.10f + 0.06f * hv);

        drawCoverCircle(i, cx, cy, rd, 1.0f);

        bool hot = (hv > 0.35f);
        StrokeCircle(cx, cy, rd - 0.75f, hot ? UI::ACCENT : UI::BORDER,
                     hot ? 0.95f : 0.55f, 1.6f + 0.4f * hv);
        g_alphaMul = keep;

        Hit h; h.r = Rc(cx - rd, cy - rd, rd * 2, rd * 2); h.kind = HK_PL; h.index = i;
        if (g_alpha > 0.5f && a.plActive < 0) a.hits.push_back(h);   // faded out once selected
    }
    EndMotionBlur();

    // Selected item's avatar: slides with the card from the disc center to the card's left
    if (selIdx >= 0 && selIdx < n && selE > 0.01f) {
        const float cardA = (a.plActive >= 0) ? 1.0f : selE;
        drawCoverCircle(selIdx, avatarCx, avatarCy, R * 0.92f, cardA);
        StrokeCircle(avatarCx, avatarCy, R * 0.92f - 0.75f, UI::ACCENT, 0.9f * selE * cardA, 1.6f);
    }
}
/* ══════════════════════════════════════════
   Main draw
   ══════════════════════════════════════════ */
/* ══════════════════════════════════════════
   Splash: blank → tracks fly in from the right and line up → title bar appears
   ══════════════════════════════════════════ */
/** Phase 1: logo and text fade in, hold, then fade out */
static void DrawSplash(float t, float W, float H) {
    float a;
    if (t < SPLASH_IN)                 a = t / SPLASH_IN;
    else if (t < SPLASH_HOLD_END)      a = 1.0f;
    else                               a = 1.0f - (t - SPLASH_HOLD_END) / (SPLASH_OUT - SPLASH_HOLD_END);
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;

    // Rises slightly from below while fading in
    float in = (t < SPLASH_IN) ? (t / SPLASH_IN) : 1.0f;
    float e = 1.0f - powf(1.0f - in, 3.0f);
    float dy = (1.0f - e) * 18.0f;

    const float cx = W * 0.5f;

    g_alphaMul = a;

    // Icon + title as one group, centered as a whole
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

    // Loading phase: only a centered spinner, no logo or text
    if (!g_loadDone) {
        const float cx = W * 0.5f;
        const float cy = H * 0.5f;
        // Fade in over the first 200ms so it doesn't pop out of the flat background
        float fade = g_loadT / 0.20f;
        if (fade > 1.0f) fade = 1.0f;
        DrawSpinner(cx, cy, 20.0f, g_loadT * 260.0f, 0.95f * fade);
        DrawTitleBar(W);          // The title bar is fully usable from the first frame
        return;
    }

    // Phase 1: logo + text fade in → fade out
    if (g_introT < INTRO_TILE_START) {
        DrawSplash(g_introT, W, H);
        // The spinner fades out within the first 200ms, handing over to the logo fade-in
        if (g_introT < 0.20f) {
            float f = 1.0f - g_introT / 0.20f;
            DrawSpinner(W * 0.5f, H * 0.5f, 20.0f, g_loadT * 260.0f, 0.95f * f);
        }
        DrawTitleBar(W);
        return;
    }

    // After the splash, hand over directly to the playlist picker (it fades itself in); no more flying song cards
    DrawTitleBar(W);
}

static void DrawUI(ID2D1RenderTarget* rt) {
    Bind(rt);
    g_coverUploads = g_snapUI ? INT_MAX : COVER_UPLOADS_PER_FRAME;   // a screenshot is a single frame
    {   // Theme changed: rebuild the placeholder gradient too
        static int lastGen = -1;
        if (lastGen != ThemeGeneration()) {
            lastGen = ThemeGeneration();
            BuildCoverBrushes(g_rt);
            Logf("APP", "主题换代 %d：占位渐变已重建", lastGen);
        }
    }
    DrainDecoded();               // Bring in covers decoded in the background
    DrainCovers();                // On-demand downloaded covers arrive and queue up for decoding
    App& a = g_app;
    const float W = a.width, H = a.height;

    a.hits.clear();
    g_alpha = 1.0f;
    FillRect(Rc(0, 0, W, H), UI::BG);

    // While the settings panel is open, suppress background hover: move the mouse off-screen, restore after drawing the panel
    const POINT realMouse = a.mouse;
    if (g_settingsOpen) a.mouse = POINT{ -100000, -100000 };

    if (!g_introDone) {         // Splash phase: draw only the fly-in
        DrawIntro();
        return;
    }

    if (g_exitOn) {             // Exiting the player page: fade out + grid flies in
        DrawExiting();
        return;
    }

    const D2D1_POINT_2F center = D2D1::Point2F(W * 0.5f, H * 0.5f);

    // Playlist picker: drawn as a separate full page
    if (a.page == Page::Playlists) {
        // While leaving, the page grows 1.0 -> 1.02: a tiny bit of motion keeps the fade alive
        float scPage = 1.0f;
        if (g_plPhase == 1) {
            float e1 = g_plT * g_plT * (3.0f - 2.0f * g_plT);
            scPage = 1.0f + 0.02f * e1;
        }
        if (scPage > 1.0005f)
            g_rt->SetTransform(D2D1::Matrix3x2F::Scale(scPage, scPage, center));

        g_alpha = g_plEnter;          // Fade-in after the splash ends
        DrawPlaylistPicker();
        g_alpha = 1.0f;

        if (scPage > 1.0005f) g_rt->SetTransform(D2D1::Matrix3x2F::Identity());

        // Page exit: the whole page scales up a touch while fading into the background colour.
        // No direction, no particles - just a quiet cross-dissolve, which suits this app far
        // better than a showy effect. The scale is what keeps it from reading as a plain fade.
        if (g_plPhase == 1 && g_plT > 0.001f) {
            float e = g_plT * g_plT * (3.0f - 2.0f * g_plT);   // smoothstep
            FillRect(Rc(0, (float)UI::TITLE_H, W, H - (float)UI::TITLE_H), UI::BG, e);
            // Last 30%: the ring fades in underneath, so there is no blank beat between them
            if (g_plWaiting && g_plT > 0.70f) DrawPlLoader(W, H, (g_plT - 0.70f) / 0.30f);
        }
        // Page curtain: covers the picker while switching in or out
        if (g_curtainPhase != 0) {
            float e = g_curtainT * g_curtainT * (3.0f - 2.0f * g_curtainT);
            FillRect(Rc(0, 0, W, H), UI::BG, e);
        }

        // Loading ring (phases 2 and 3); the page is still the picker until the data lands
        if (g_plPhase == 2) {
            float sIn = (std::min)(1.0f, g_plT / 0.18f);
            DrawPlLoader(W, H, 1.0f, 0.90f + 0.10f * (1.0f - powf(1.0f - sIn, 3.0f)));
        } else if (g_plPhase == 3) {
            float p = g_plT;
            DrawPlLoader(W, H, powf(1.0f - p, 1.6f));      // eased tail: no hard edge at the end
        }
        
        DrawTitleBar(W);
        if (g_setT > 0.01f) {
            a.mouse = realMouse;
            // The panel is modal: the scrim's and card's hit areas must be registered outside PanelFx,
            // otherwise they're clipped while the animation runs and clicks pass through to the discs behind
            { Hit bg; bg.r = Rc(0, 0, W, H); bg.kind = HK_SET_BG; a.hits.push_back(bg); }
            { Hit hc; hc.r = SettingsCardRect(W, H); hc.kind = HK_SET_CARD; a.hits.push_back(hc); }
            FillRect(Rc(0, 0, W, H), 0x000000, 0.20f * g_setT);
            PanelFx fx(g_setT, W * 0.5f, H * 0.5f);
            DrawSettings(W, H);
        }
        return;
    }

    // Grid layer
    if (a.pageAnim < 0.999f) {
        g_alpha = 1.0f - a.pageAnim;
        float sc = 1.0f - 0.05f * a.pageAnim;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, center));
        DrawGrid();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Player layer
    if (a.pageAnim > 0.001f) {
        g_alpha = a.pageAnim;
        if (g_enterOn && g_enterT < 0.80f) {
            // Songs page animates first (tiles scatter upward), exactly as before
            DrawEntering();

            // With no selected song there is no tile to morph into the disc, so the player
            // page would pop in when the transition ends. Fade it in late, after the tiles
            // have mostly cleared, instead of replacing the grid animation.
            if (g_enterIdx < 0) {
                float p = (g_enterT - 0.30f) / 0.30f;
                if (p <= 0.0f) { /* still flying out */ }
                else {
                    if (p > 1.0f) p = 1.0f;
                    float pe = p * p * (3.0f - 2.0f * p);
                    float sc = 0.94f + 0.06f * pe;
                    rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, center));
                    g_alpha = a.pageAnim * pe;
                    DrawPlayerPage();
                    g_alpha = a.pageAnim;
                    rt->SetTransform(D2D1::Matrix3x2F::Identity());
                }
            }
        } else {
            rt->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, 1.0f, center));
            DrawPlayerPage();
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    g_alpha = 1.0f;

    // No account / login expired: auto-open platform selection (once per launch)
    if (g_loginState == 2 && !g_loginAsked && !g_loginRunning && !g_exitOn && !g_enterOn) {
        g_loginAsked = true;
        g_settingsOpen = true;
        g_loginChoose = true;
    }

    // Loading ring while the playlist data is still arriving (grid side, after the swap)
    if (g_plPhase == 2) DrawPlLoader(W, H, 1.0f);
    else if (g_plPhase == 3) DrawPlLoader(W, H, 1.0f - g_plT);

    // Page curtain (title-bar navigation between the three pages)
    if (g_curtainPhase != 0) {
        float e = g_curtainT * g_curtainT * (3.0f - 2.0f * g_curtainT);
        FillRect(Rc(0, 0, W, H), UI::BG, e);
    }
    
    // Title bar: always opaque, drawn every frame. No enter fade —
    // the old "hide for 1.4s" made buttons unclickable and the whole bar look translucent.
    DrawTitleBar(W);
    if (g_setT > 0.01f) {
        a.mouse = realMouse;         // The panel needs the real mouse for hover
        // Scrim and card are always clickable (modal); only buttons are clipped by the animation
        { Hit bg; bg.r = Rc(0, 0, W, H); bg.kind = HK_SET_BG; a.hits.push_back(bg); }
        { Hit hc; hc.r = SettingsCardRect(W, H); hc.kind = HK_SET_CARD; a.hits.push_back(hc); }
        FillRect(Rc(0, 0, W, H), 0x000000, 0.20f * g_setT);   // Scrim only fades in, no scaling
        PanelFx fx(g_setT, W * 0.5f, H * 0.5f);
        DrawSettings(W, H);
    }
}

/* ══════════════════════════════════════════
   Animation
   ══════════════════════════════════════════ */
/** Real elapsed seconds (animation time must not accumulate a fixed 16ms, or it stretches at low frame rates) */
static float DeltaSeconds() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last;
    const Clock::time_point now = Clock::now();
    if (last == Clock::time_point()) { last = now; return 0.016f; }
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (dt > 0.10f) dt = 0.10f;      // On hitches, don't let the animation jump too far
    return dt;
}

/** Convert "perFrame convergence" into a frame-rate-independent factor so wall-clock duration stays consistent */
static float Ease(float perFrame, float dt) {
    return 1.0f - powf(1.0f - perFrame, dt * 62.5f);
}

/* Critically damped spring: reaches the target without overshoot and keeps its velocity when the target changes.
   Steps by the exact solution, so the motion does not depend on the frame rate. Returns whether it still moves */
static bool SpringStep(float& value, float& velocity, float target, float dt) {
    const float stiffness = 16.0f;   // angular frequency; settles in about 0.3 s
    const float offset = value - target;
    const float drift = velocity + stiffness * offset;
    const float decay = expf(-stiffness * dt);
    value = target + (offset + drift * dt) * decay;
    velocity = (velocity - stiffness * drift * dt) * decay;
    if (std::fabs(value - target) > 0.1f || std::fabs(velocity) > 2.0f) return true;
    value = target;
    velocity = 0.0f;
    return false;
}

static void GoPlayer(int idx);  // Defined later
static void SwitchTrack(int delta);

/* ── Listen Together: cross-platform matching (async, don't block the UI) ── */
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
        if (key == g_matchKey) return;          // Don't re-match the same song
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
/* ── Listen Together: receive and apply the peer's state ── */
static void ApplyRemoteState(const TogetherEvent& ev) {
    App& a = g_app;
    if (ev.song.empty()) { Logf("TOG", "收到状态但没有曲目 id，忽略"); return; }
    Logf("TOG", "收到状态 song=%.20s pos=%d play=%d", ev.song.c_str(), ev.posMs, ev.playing ? 1 : 0);

    int idx = -1;
    for (int i = 0; i < (int)a.songs.size(); i++) {
        if (a.songs[i].hash == ev.song) { idx = i; break; }   // hash is a std::string
    }
    if (idx < 0) {
        Logf("TOG", "本地没有该曲目，开始跨平台匹配");
        TryMatchAsync(ev);
        return;
    }
    Logf("TOG", "本地命中 idx=%d 当前=%d", idx, a.current);

    // Expected position: peer's position + elapsed time (UTC ms, comparable across machines)
    long long now = (long long)TogetherNowMs();
    long long elapsed = (now > ev.stampMs) ? (now - ev.stampMs) : 0;
    if (elapsed > 60000) elapsed = 0;                 // If clock skew is too large, just use the received position
    long long expectedLL = (long long)ev.posMs + (ev.playing ? elapsed : 0);
    int dur = (idx >= 0 && idx < (int)a.songs.size()) ? a.songs[(size_t)idx].durationSec : 0;
    if (dur > 0 && expectedLL > (long long)dur * 1000 - 1000) expectedLL = (long long)dur * 1000 - 1000;
    if (expectedLL < 0) expectedLL = 0;
    int expected = (int)expectedLL;

    if (idx != a.current) {
        Logf("TOG", "跟随切歌 %d -> %d", a.current, idx);
        GoPlayer(idx);
        g_pendingSeekMs = expected;                 // Jump straight to the peer's position once opened
        if (!ev.playing) a.playing = false;
        return;
    }
    if (ev.playing != a.playing) {                  // Play/pause follows the peer
        a.playing = ev.playing;
        if (ev.playing) AudioPlay(); else AudioPause();
    }
    if (ev.playing) {
        int mine = (int)(a.pos * 1000.0);
        if (abs(mine - expected) > 800) {           // Correct only when the event point is off by more than 0.8s
            AudioSeekMS(expected);
            a.pos = expected / 1000.0;
        }
    }
}

/* ── Listen Together: push out our own state ── */
static void PushLocalState(bool force) {
    if (!TogetherActive()) return;
    // Both push: the host pushes authoritative state; a follower's push only lets the host know someone is listening
    App& a = g_app;
    if (a.current < 0 || a.current >= (int)a.songs.size()) return;

    static std::string  lastHash;
    static bool         lastPlaying = false;
    static ULONGLONG    lastPush = 0;
    static int          lastSeekMs = -1;

    const Song& s = a.songs[a.current];
    ULONGLONG now = GetTickCount64();
    int posMs = (int)(a.pos * 1000.0);

    // Sync only on key actions: song change, play/pause, seek, loading-state change
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
                      Narrow(s.title), Narrow(s.artist));
}
static void RefreshPlaylistsAsync();
static bool Animate() {
#ifndef FLORAL_NO_TOGETHER
    // ── Listen Together: consume events, push state ──
    {
        TogetherEvent ev;
        while (TogetherPoll(ev)) {
            if (ev.kind == 1) {
                Logf("TOG", "检测到对端：%s", ev.nick.c_str());
                g_togHosting = false;
                g_togForcePush = true;          // Just joined: push the current state right away
                TogetherSayHello();            // Send a hello back so the peer sees me too
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
                    // The host is authoritative; don't follow a visitor's state
                } else {
                    ApplyRemoteState(ev);
                }
            }
        }
        {   // Presence heartbeat: report the nickname every 15s so each side sees the other
            static ULONGLONG lastHello = 0;
            ULONGLONG nowH = GetTickCount64();
            if (nowH - lastHello > 15000) {
                lastHello = nowH;
                TogetherSayHello();
            }
        }
        {   // Loading state changed (buffering / recovered): sync once
            static bool lastLoading = false;
            if (g_audioLoading != lastLoading) {
                lastLoading = g_audioLoading;
                g_togForcePush = true;
            }
        }
        PushLocalState(false);

        // Cross-platform match result
        {
            PendPlay pp;
            {
                std::lock_guard<std::mutex> lk(g_ppMx);
                if (!g_pp.has) { /* no result */ }
                pp = g_pp;
                if (g_pp.has) g_pp.has = false;     // Consume only once
            }
            if (pp.has) {
                if (pp.failed) {
                    TogToast(T(L"该曲目没有对应版本，已通知房主跳过", L"No matching version, asked the host to skip"));
                    if (g_audioOk) { AudioPause(); g_app.playing = false; }
                    if (!g_togIsHost) TogetherRequestSkip();
                } else {
                    Logf("TOG", "注入匹配结果 hash=%.20s 并播放", pp.hash.c_str());
                    // Inject into the local list and play
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

    // Login / export state machine (worker threads only write state; UI-related work happens here)
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
            g_loginState = 1;                     // Login succeeded, valid
            g_loginAsked = false;
            ClearCache();                         // Cache isn't shared across platforms
            g_covFlag.assign(g_app.songs.size(), 0);
            g_layoutCount = -1;
            g_layoutWidth = -1;
            SettingsToast(changed ? T(L"已切换账号，正在拉取歌单…", L"Account switched, fetching playlist…") : T(L"登录成功，正在拉取歌单…", L"Signed in, fetching playlist…"));
            StartExport(tgUid, tgTok);
            RefreshPlaylistsAsync();
            CheckLoginAsync();                    // Fetch nickname
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
            ForgetShownPlaylist();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers(g_app.songs);
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

    // When idle, prewarm prev/next cover bitmaps so the switch frame doesn't build them on the spot (drops a frame)
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
                    // Not decoded yet; retry next round
                }
            }
        }
    }

    // Play/pause icon: morph parameter approaches the target (interruptible, no jumping)
    {
        float target = a.playing ? 1.0f : 0.0f;
        if (fabsf(g_playIconMix - target) > 0.0005f) {
            float step = dt / 0.16f;                 // Done in 160ms
            if (g_playIconMix < target) g_playIconMix = (std::min)(target, g_playIconMix + step);
            else                        g_playIconMix = (std::max)(target, g_playIconMix - step);
            need = true;
        } else {
            g_playIconMix = target;
        }
    }

    // Player text: on song change, slide the old text out, then the new text in
    {
        static int  lastCur   = -1;
        static Page lastPage2 = Page::Grid;

        const bool changed = (a.current != lastCur) || (a.page != lastPage2);
        if (changed) {
            const bool songChanged = (a.current != lastCur);
            lastCur = a.current;
            lastPage2 = a.page;

            if (a.page != Page::Player || g_exitOn) {
                // Leaving the player page: keep text static; never replay the enter animation on exit
                g_textPhase = 0;
                g_textT = 0.0f;
            } else {
                // Grab a copy of the old text; it's still drawn during exit
                g_outTitle  = g_curTitle;
                g_outArtist = g_curArtist;
                g_outAlbum  = g_curAlbum;
                g_textPhase = (songChanged && !g_outTitle.empty()) ? 1 : 2;   // Exit only if there is old text
                g_textT = 0.0f;
            }
        } else if (g_textPhase == 1) {
            g_textT += dt;
            if (g_textT >= 0.20f) {                 // Exit done, start slide-in
                g_textPhase = 2;
                g_textT = 0.0f;
            }
        } else if (g_textPhase == 2) {
            g_textT += dt;
            if (g_textT >= 0.42f) { g_textPhase = 0; g_textT = 0.0f; }
        }
        if (g_textPhase != 0) need = true;


        // Save the current three rows as a snapshot for the next song change
        if (a.current >= 0 && a.current < (int)a.songs.size()) {
            const Song& cs = a.songs[(size_t)a.current];
            g_curTitle = cs.title; g_curArtist = cs.artist; g_curAlbum = cs.album;
        } else {
            g_curTitle.clear(); g_curArtist.clear(); g_curAlbum.clear();
        }
    }

    // Keep redrawing while a toast is alive, or its fade won't animate
    {
        ULONGLONG nowT2 = NowMs();
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

    // Page curtain: cover the old page, swap, then reveal the new one
    if (g_curtainPhase != 0) {
        need = true;
        if (g_curtainPhase == 1) {
            g_curtainT += dt / 0.16f;                 // out: quick, so the click feels answered
            if (g_curtainT >= 1.0f) {
                g_curtainT = 1.0f;
                if (g_curtainAction == 1) {
                    GoPickerNow();
                } else if (g_curtainAction == 2) {
                    GoPlayerPageNow(g_playerFromPicker);
                }
                g_curtainPhase = 2;
            }
        } else {
            g_curtainT -= dt / 0.30f;                 // in: slower, so the new page settles
            if (g_curtainT <= 0.0f) {
                g_curtainT = 0.0f;
                g_curtainPhase = 0;
                g_curtainAction = 0;
            }
        }
    }
    // Playlist -> song page transition
    if (g_plPhase != 0) {
        g_plSpin += dt * 260.0f;
        need = true;
        if (g_plPhase == 1) {                       // wipe out, bottom to top
            g_plT += dt / 0.26f;
            if (g_plT >= 1.0f) {
                g_plT = 0.0f;
                g_plPhase = g_plWaiting ? 2 : 0;    // tracks already loaded: fly in without the loading ring
            }
        } else if (g_plPhase == 2) {                // loading ring
            g_plT += dt;
            if (g_swapDone && g_plT > 0.18f) { g_plT = 0.0f; g_plPhase = 3; }
        } else if (g_plPhase == 3) {                // ring fades out while the tracks start flying in
            g_plT += dt / 0.30f;
            // Overlap: launch the fly-in halfway through the ring fade, so the two motions
            // hand off to each other instead of leaving a beat of empty background
            if (g_plT > 0.5f && g_gridEnter < 0.0f) g_gridEnter = 0.0f;
            if (g_plT >= 1.0f) {
                g_plT = 0.0f;
                g_plPhase = 0;
                if (g_gridEnter < 0.0f) g_gridEnter = 0.0f;
            }
        }
    }
    // After the transition step: when the picker's exit ends this frame, the songs replace it in the same frame
    DrainPlaylistSwap();

    // ── Playlist picker: merge background data, inertia, hover ──
    if (g_plReady.load()) {
        std::vector<Playlist> got;
        { std::lock_guard<std::mutex> lk(g_plMx); got.swap(g_plPending); }
        g_plReady = false;
        if (!got.empty() && got != a.playlists) {
            // Selections are positions in the list; move them to where their playlists are in the new one
            auto follow = [&](int& i) {
                if (i < 0 || i >= (int)a.playlists.size()) return;
                const std::string gcid = a.playlists[(size_t)i].gcid;
                i = -1;
                for (int k = 0; k < (int)got.size(); k++)
                    if (got[(size_t)k].gcid == gcid) { i = k; break; }
            };
            follow(a.plActive);
            follow(a.plSelLast);
            follow(a.plPress);
            a.playlists = std::move(got);
            SavePlaylists();
            g_plCovAsked.clear();
            a.plHover = -1;
            need = true;
        }
        CheckShownPlaylist();
    }
    if (a.page == Page::Playlists) {
        // Selection progress
        if (a.plActive >= 0) {
            if (a.plSelT < 1.0f) {
                a.plSelT += dt / 0.34f;
                if (a.plSelT > 1.0f) a.plSelT = 1.0f;
                need = true;
            }
        } else if (a.plSelT > 0.0f) {
            a.plSelT -= dt / 0.24f;
            if (a.plSelT < 0.0f) { a.plSelT = 0.0f; a.plSelLast = -1; }   // collapse finished
            need = true;
        }

        if (!a.plDragging) {
            // Inertia: the velocity is per 16 ms step; scaling by dt keeps the glide the same at any frame rate
            if (a.plActive < 0) {
                const float steps = dt * 62.5f;
                const float decay = powf(0.90f, steps);
                a.plX += a.plVelX * steps;
                a.plY += a.plVelY * steps;
                a.plVelX *= decay;
                a.plVelY *= decay;
                if (fabsf(a.plVelX) < 0.25f) a.plVelX = 0.0f;
                if (fabsf(a.plVelY) < 0.25f) a.plVelY = 0.0f;
                if (a.plVelX != 0.0f || a.plVelY != 0.0f) need = true;
            }
            // Ease toward target
            float k = Ease(0.25f, dt);
            a.plX += (a.plTargetX - a.plX) * k;
            a.plY += (a.plTargetY - a.plY) * k;
            if (fabsf(a.plX - a.plTargetX) < 0.35f) a.plX = a.plTargetX;
            if (fabsf(a.plY - a.plTargetY) < 0.35f) a.plY = a.plTargetY;
            if (a.plX != a.plTargetX || a.plY != a.plTargetY) need = true;
        }
        if (a.plActive < 0) PlClampPan();          // No pan limits while selected; must be able to center

        int hov = -1;
        // 卡片展开后其余圆已经淡出，不再参与悬停（否则边框还会跟着鼠标亮）
        if (!a.plDragging && !g_settingsOpen && a.plActive < 0) {
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

        // Per-circle hover progress: fast ease-out, so the disc reacts the moment the pointer lands
        if ((int)a.plHoverT.size() != (int)a.playlists.size())
            a.plHoverT.assign(a.playlists.size(), 0.0f);
        {
            float kHov = Ease(0.16f, dt);
            for (int i = 0; i < (int)a.plHoverT.size(); i++) {
                float target = (a.plHover == i && a.plActive < 0 && !a.plDragging) ? 1.0f : 0.0f;
                float v = a.plHoverT[(size_t)i];
                if (fabsf(v - target) < 0.004f) { a.plHoverT[(size_t)i] = target; continue; }
                a.plHoverT[(size_t)i] = v + (target - v) * kHov;
                need = true;
            }
        }
    }
    // Dragging moves the pan between steps as well, so blur covers all travel since the last step
    g_plBlur = D2D1::Vector2F((a.plX - g_plLastX) * SHUTTER, (a.plY - g_plLastY) * SHUTTER);
    g_plLastX = a.plX;
    g_plLastY = a.plY;

    // Panel enter/exit progress
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

    // Splash timing (by real time)
    if (!g_loadDone) {
        g_loadT += dt;
        need = true;
        // The first screen is the playlist picker, which only needs the startup playlist refresh
        bool coversReady = CoversReadyForFirstScreen();
        bool refreshDone = (g_expState != 1) && !g_bootRefreshPending.load();

        if ((coversReady && refreshDone) || g_loadT > 12.0f) {
            g_loadDone = true;
            g_introT = 0.0f;
            Logf("APP", "启动加载完成 t=%.2fs（封面就绪=%d）", g_loadT, coversReady ? 1 : 0);
        }
    } else if (!g_introDone) {
        g_introT += dt;
        need = true;
        // Don't pre-decode song covers during the splash: the picker only shows playlist covers; song covers wait until inside
        // End the splash when the logo finishes fading and go straight to the picker (no waiting for covers, no flying cards)
        if (g_introT > SPLASH_OUT + 0.05f) {
            g_introDone = true;
            if (g_app.page == Page::Grid) { g_app.page = Page::Playlists; PlCenterContent(); }
            g_plEnter = 0.0f;                      // Playlist page fade-in
            Logf("APP", "开屏结束 t=%.2fs", g_introT);
        }
    }

    // Smooth scrolling: actual position eases toward target (frame-rate independent).
    // The blur follows this step only, so jumps such as loading another playlist stay sharp
    {
        const float before = a.scroll;
        float d = a.scrollTarget - a.scroll;
        if (std::fabs(d) > 0.4f) {
            a.scroll += d * Ease(0.22f, dt);
            need = true;
        } else if (a.scroll != a.scrollTarget) {
            a.scroll = a.scrollTarget;
            need = true;
        }
        g_gridBlur = D2D1::Vector2F(0.0f, (before - a.scroll) * SHUTTER);
    }
    // Content that has stayed still for a moment is drawn once more without blur. Pointer input slower
    // than the display moves it only every other frame, and the wait keeps those frames from flickering
    {
        static ULONGLONG lastMoved = 0;
        if (g_gridBlur.y != 0.0f || g_plBlur.x != 0.0f || g_plBlur.y != 0.0f) lastMoved = NowMs();
        else if (lastMoved && NowMs() - lastMoved > 30) { lastMoved = 0; need = true; }
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

    // Song-change animation + record spin
    if (g_switchT < 1.0f) {
        g_switchT += dt / 0.55f;
        if (g_switchT > 1.0f) g_switchT = 1.0f;
        need = true;
    }
    if (a.page == Page::Player && a.playing && !g_enterOn) {
        g_discAngle += dt * 55.0f;      // About 9s per revolution
        if (g_discAngle > 360.0f) g_discAngle -= 360.0f;
        need = true;
    }

    // Exit-player-page transition timer
    if (g_exitOn) {
        g_exitT += dt / 2.0f;
        if (g_exitT >= 1.0f) { g_exitT = 1.0f; g_exitOn = false; }
        need = true;
    }

    // Enter-player-page transition timer
    if (g_enterOn) {
        g_enterT += dt / 2.2f;
        if (g_enterT >= 1.0f) { g_enterT = 1.0f; g_enterOn = false; }
        // The divider grows only after all tiles have flown away
        float lk = (g_enterT - 0.70f) / 0.20f;
        g_lineT = (lk < 0.0f) ? 0.0f : (lk > 1.0f ? 1.0f : lk);
        need = true;
    }

    // Divider: extends right-to-left when entering the player page (finishes by itself after the transition)
    if (a.page == Page::Player) {
        if (!g_enterOn && g_lineT < 1.0f) {
            g_lineT += dt / 0.45f;
            if (g_lineT > 1.0f) g_lineT = 1.0f;
            need = true;
        }
    } else {
        g_lineT = 0.0f;
    }

    // A URL resolved for the current song is opened in the background
    {
        int st = 0, ui = -1;
        std::wstring u;
        {
            std::lock_guard<std::mutex> lk(g_urlMx);
            st = g_urlState; u = g_urlReady; ui = g_urlIdx;
        }
        if (st == 2 && ui == a.current && !g_audioOk && !g_audioError) {
            {
                // Clear state first so each URL is opened at most once; never retry every frame
                std::lock_guard<std::mutex> lk(g_urlMx);
                g_urlState = 0;
                g_urlReady.clear();
            }
            Trace("open  idx=%d url_ok=1", ui);
            BeginOpenAsync(u);                   // Open in the background; the UI keeps animating
        }
        // Result of the background open: finish on the UI thread (playback state is only changed here)
        {
            int r = g_openResult.exchange(0);
            if (r > 0) {
                g_audioOk = true;
                g_audioLoading = false;
                if (g_pendingSeekMs >= 0) {          // Quality switch / Listen Together: resume mid-track with a fade-in
                    AudioSeekMS(g_pendingSeekMs);
                    a.pos = g_pendingSeekMs / 1000.0;
                    g_pendingSeekMs = -1;
                    StartFadeIn();
                } else {                             // a track played from its start needs no fade
                    g_vol = 1.0f;
                    ApplyVolume();
                }
                if (a.playing) AudioPlay();          // stays paused if the user paused while it loaded
                Trace("open  ok idx=%d", a.current);
                PrefetchUrl((a.current + 1) % (int)a.songs.size());
            } else if (r < 0 && g_openUrl.rfind(L"http", 0) != 0) {   // a local file that won't open: stream instead
                LoadAudioFor(a.current, false);
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
        if (st == 3 && ui == a.current && !g_audioOk) {   // Only honor failures for our own song
            g_audioError = true;
            g_audioErrKind = 1;
            g_audioLoading = false;
            RecheckLoginOnFailure();
            std::lock_guard<std::mutex> lk(g_urlMx);
            g_urlState = 0;
        }
    }

    // Watchdog: the stream opened but its position never advances, so treat it as dead
    if (g_audioLoading && g_audioOk && a.playing && AudioPosMS() <= 120 &&
        GetTickCount64() - g_loadingSince > 4000) {
        Trace("wd1 fire idx=%d", a.current);
        AudioClose();
        g_audioOk = false;
        g_audioError = true;
        g_audioLoading = false;
        RecheckLoginOnFailure();
        need = true;
    }

    // Watchdog: resolution never returns (the sidecar died) — can't wait forever either
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

    // Also show the spinner during background seeks (streaming must download up to the target)
    const bool seeking = AudioIsSeeking();
    if (seeking) {
        g_audioLoading = true;
        g_loadingMin = 0;
        g_loadingSince = GetTickCount64();   // the watchdog above counts from the end of the seek
    }

    // Spinner: keep spinning until a position can be read (and show for at least g_loadingMin)
    if (g_audioLoading) {
        bool ready = g_audioOk && AudioPosMS() > 120 && !seeking;
        if (ready && GetTickCount64() > g_loadingMin) {
            g_audioLoading = false;
        } else {
            g_spinAngle += dt * 7.0f;       // roughly one revolution per second
            if (g_spinAngle > 6.2831853f) g_spinAngle -= 6.2831853f;
            need = true;
        }
    }

    // Fade in / fade out; a fade-in waits while a seek keeps the output silent
    if (g_fadeDir != 0) {
        if (g_fadeDir < 0 || !seeking) g_vol += g_fadeDir * (dt / (g_fadeDir > 0 ? 0.45f : 0.25f));
        if (g_vol > 1.0f) g_vol = 1.0f;
        if (g_vol < 0.0f) g_vol = 0.0f;
        ApplyVolume();

        if (g_fadeDir > 0 && g_vol >= 1.0f) {
            g_fadeDir = 0;
        } else if (g_fadeDir < 0 && g_vol <= 0.0f) {
            g_fadeDir = 0;
            if (g_pendingPick >= 0) {        // switching to one specific track
                int i = g_pendingPick;
                g_pendingPick = -1;
                LoadAudioFor(i);
            } else {
                int d = g_pendingDir;
                g_pendingDir = 0;
                if (d != 0) SwitchTrackNow(d);   // only really switch once the fade-out is clean
            }
        }
        need = true;
    }

    if (a.page == Page::Player && a.playing && a.current >= 0 &&
        a.current < (int)a.songs.size()) {
        if (g_audioOk) {
            if (!g_seekDrag) {                 // don't let the engine position fight back while dragging
                int ms = AudioPosMS();
                if (ms >= 0) a.pos = ms / 1000.0;

                if (AudioIsFailed()) {         // the download broke off: stop, and let a retry resume from here
                    Logf("ERR", "下载中断 ms=%d", ms);
                    g_pendingSeekMs = (int)(a.pos * 1000.0);
                    BeginCloseAsync();
                    g_audioOk = false;
                    g_audioError = true;
                    g_audioErrKind = 3;
                } else if (AudioIsEnded() && g_fadeDir >= 0) {   // a fade-out in progress already leads elsewhere
                    Logf("AUD", "自动连播：ms=%d", ms);
                    bool follower = false;
#ifndef FLORAL_NO_TOGETHER
                    follower = TogetherActive() && !g_togIsHost;
#endif
                    if (follower) {
                        a.playing = false;         // wait for the host to push the next track, don't switch on our own
                        AudioPause();
                    } else {
                        SwitchTrackNow(1);         // the track is over, so there is nothing to fade out
                    }
                }
            }
        } else if (g_urlState != 1 && !g_audioError) {   // while resolving/failed, don't pretend it's playing
            a.pos += dt;
            if (a.pos >= a.songs[a.current].durationSec) {
                // In Together only the host may switch tracks; guests stop and wait for the host's next track
                bool follower = false;
#ifndef FLORAL_NO_TOGETHER
                follower = TogetherActive() && !g_togIsHost;
#endif
                if (follower) {
                    a.pos = (float)a.songs[a.current].durationSec;   // park at the end, don't run past it
                    a.playing = false;
                    AudioPause();
                } else {
                    SwitchTrack(1);            // auto-advance to the next track when finished (with the disc animation)
                }
            }
        }
        need = true;
    }

    // Grid tiles follow their targets on springs; only the tiles near the viewport animate, the rest snap into place
    if (g_draw.size() == g_target.size()) {
        if (g_drawVel.size() != g_draw.size()) g_drawVel.assign(g_draw.size(), TileBox{});
        if (g_layoutDirty) {
            g_layoutDirty = false;
            ++g_stampVal;
            if (g_stamp.size() != g_base.size()) g_stamp.assign(g_base.size(), 0);
            for (int i : g_vis)
                if (i >= 0 && i < (int)g_stamp.size()) g_stamp[(size_t)i] = g_stampVal;
            for (int i = 0; i < (int)g_draw.size() && i < (int)g_stamp.size(); i++) {
                if (g_stamp[(size_t)i] != g_stampVal) {
                    g_draw[(size_t)i] = g_target[(size_t)i];
                    g_drawVel[(size_t)i] = TileBox{};
                }
            }
            need = true;
        }

        for (int i : g_vis) {
            if (i < 0 || i >= (int)g_draw.size()) continue;
            TileBox& d = g_draw[(size_t)i];
            TileBox& v = g_drawVel[(size_t)i];
            const TileBox& t = g_target[(size_t)i];
            // All four values must step, so the results are combined without short-circuiting
            bool moving = SpringStep(d.x, v.x, t.x, dt);
            moving |= SpringStep(d.y, v.y, t.y, dt);
            moving |= SpringStep(d.w, v.w, t.w, dt);
            moving |= SpringStep(d.h, v.h, t.h, dt);
            need |= moving;
        }
    }
    return need;
}

/* ══════════════════════════════════════════
   Initialization
   ══════════════════════════════════════════ */
static bool GfxInit() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_dw)))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_wic)))) return false;
    RegisterMotionBlur();
    return true;
}

static void GfxRelease() {
    for (auto& entry : g_fmts) entry.second->Release();
    g_fmts.clear();
    for (auto& entry : g_coverBmps) {
        if (entry.second.brush) entry.second.brush->Release();
        if (entry.second.bmp) entry.second.bmp->Release();
    }
    g_coverBmps.clear();
    for (auto& entry : g_coverSrcs) if (entry.second) entry.second->Release();
    g_coverSrcs.clear();
    if (g_blurFx)  { g_blurFx->Release();  g_blurFx = nullptr; }
    if (g_layerRt) { g_layerRt->Release(); g_layerRt = nullptr; }
    for (int i = 0; i < 8; i++) {
        if (g_covBrush[i]) { g_covBrush[i]->Release(); g_covBrush[i] = nullptr; }
    }
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    if (g_wic) { g_wic->Release(); g_wic = nullptr; }
    if (g_dw) { g_dw->Release(); g_dw = nullptr; }
    if (g_d2d) { g_d2d->Release(); g_d2d = nullptr; }
}

/* ══════════════════════════════════════════
   Tray icon
   ══════════════════════════════════════════ */
#define WM_TRAYICON (WM_APP + 1)

static NOTIFYICONDATAW g_nid{};
static bool   g_trayAdded = false;

static void AddTrayIcon(HWND hwnd) {
    if (g_trayAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
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
   Interaction
   ══════════════════════════════════════════ */
static Hit* Pick(POINT p) {
    for (int i = (int)g_app.hits.size() - 1; i >= 0; i--) {
        if (Inside(g_app.hits[i].r, p)) return &g_app.hits[i];
    }
    return nullptr;
}

/** Top button: uses the same transition as picking a song, the only difference is playback isn't restarted */
static void GoPlayerPageNow(bool fromPicker) {
    App& a = g_app;
    if (a.page == Page::Player) return;

    const int n = (int)a.songs.size();
    const bool hasSong = (a.current >= 0 && a.current < n);

    if (hasSong) {
        // Take off from the tile of "the current song", at the same spot as clicking it with the mouse
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
        g_enterIdx = -1;                // no song selected yet: just spread the other tiles, an empty disc in the middle
    }

    a.page = Page::Player;
    a.pageAnim = 1.0f;
    a.hoverTile = -1;
    g_switchT = 1.0f;                   // don't play the track-switch disc animation
    g_switchFrom = -1;
    g_discAngle = 0.0f;                 // spin starts from 0 degrees on entry so the hand-off doesn't jump
    g_enterT = fromPicker ? 1.0f : 0.0f;
    g_enterOn = !fromPicker;            // no entry transition when coming from the picker
    if (fromPicker) {                   // drop any leftover loading overlay, and the playlist it was loading
        g_plPhase = 0;
        g_plT = 0.0f;
        CancelPlaylistOpen();
    }
    g_exitOn = false;
    g_exitT = 0.0f;
    g_settingsOpen = false;
    g_alpha = 1.0f;
}

static void GoPlayer(int idx) {
    App& a = g_app;
    if (idx < 0 || idx >= (int)a.songs.size()) return;

    // Record where this tile is "currently drawn on screen" (including hover zoom); the transition starts there so nothing shrinks first
    EnsureBase(a.width, (int)a.songs.size());
    const std::vector<TileBox>& src = (g_draw.size() == g_base.size()) ? g_draw : g_base;
    if (idx < (int)src.size()) {
        const TileBox& b = src[(size_t)idx];
        g_enterFrom = Rc(b.x, b.y + GRID_TOP - a.scroll, b.w, b.h);
    } else {
        g_enterFrom = Rc(a.width * 0.5f - 80, a.height * 0.5f - 80, 160, 160);
    }

    // Clicking the track that's already playing: just go back to the player page, don't restart from the top
    const bool sameTrack = (idx == a.current) && (g_audioOk || g_audioLoading) && !g_audioError;

    g_textDir = (idx >= a.current) ? 1 : -1;   // the text slide direction follows the tile that was clicked
    a.current = idx;
    a.playing = true;
    a.page = Page::Player;
    a.pageAnim = 1.0f;         // no fade in/out, that's left to the transition
    a.hoverTile = -1;
    g_switchT = 1.0f;          // entering the player page doesn't play the track-switch animation
    g_switchFrom = -1;

    g_enterIdx = idx;
    g_enterT = 0.0f;
    g_enterOn = true;
    g_audioRetry = 0;
    g_pendingSeekMs = -1;               // switching to another track: start from the beginning

    if (sameTrack) {
        // Keep progress and angle; if it was paused before, keep playing
        if (g_audioOk) AudioPlay();
    } else {
        a.pos = 0.0;
        g_discAngle = 0.0f;
        if (g_audioOk && !g_audioLoading && g_fadeDir == 0) {
            // The previous track is still playing: fade out first, then start the new one once volume reaches zero
            g_pendingPick = idx;
            g_fadeDir = -1;
        } else {
            LoadAudioFor(idx);
        }
    }
}

/* Back to the playlist selection page */
static void GoGrid();                       // defined below
static void GoPickerNow();                  // immediate page switches, defined below
static void GoPlayerPageNow(bool fromPicker);

/* Ask for a page change: cover -> swap -> reveal. Any click while covering is ignored. */
static void RequestPage(int action) {
    if (g_curtainPhase != 0) return;
    g_curtainAction = action;
    g_curtainT = 0.0f;
    g_curtainPhase = 1;
}

static void GoPicker() { RequestPage(1); }
static void GoPlayerPage() {
    if (g_app.page == Page::Player) return;
    g_playerFromPicker = (g_app.page == Page::Playlists);

    // From the song grid the entry is already a full transition (tiles fly into the disc),
    // so adding the curtain on top would read as "fade out, then fly out" - two animations
    // for one move. Go straight into the fly.
    if (!g_playerFromPicker) { GoPlayerPageNow(false); return; }

    RequestPage(2);
}

/* Leaving the player page returns to whichever page we entered it from */
static void ExitPlayer() {
    if (g_playerFromPicker) { RequestPage(1); return; }
    GoGrid();
}

/* Fetches the playlist list again in the background; a playlist whose version changed has outdated cached tracks */
static void RefreshPlaylistsAsync() {
    static std::atomic<bool> running{ false };
    if (g_source != 0 || g_token.empty() || running.exchange(true)) return;
    std::thread([token = g_token, userid = g_userid] {
        EnsureSidecar(0);
        FetchPlaylistsAsync(token, userid);
        running = false;
    }).detach();
}

static void GoPickerNow() {
    if (g_app.page == Page::Playlists) return;
    g_app.page = Page::Playlists;
    g_app.pageAnim = 0.0f;
    g_app.plHover = -1;
    g_app.plActive = -1;
    g_app.plDragging = false;
    g_app.plVelX = g_app.plVelY = 0.0f;
    g_app.plPress = -1;
    g_app.plSelLast = -1;
    g_app.plSelT = 0.0f;
    g_app.plTargetX = 0.0f;
    g_app.plTargetY = 0.0f;
    g_plPhase = 0;                      // cancel any half-finished picker -> song transition
    g_plT = 0.0f;
    g_exitOn = false;
    g_enterOn = false;
    g_enterT = 1.0f;
    PlCenterContent();
    g_plEnter = 1.0f;          // the curtain does the reveal, so no double fade
    g_mouseDirty = true;
    RefreshPlaylistsAsync();
}

static void GoGrid() {
    g_app.page = Page::Grid;
    g_app.pageAnim = 0.0f;
    g_gridEnter = 0.0f;          // back to the song grid: tracks fly in from the right (same as the original splash)
    g_enterOn = false;
    g_enterT = 1.0f;
    g_exitT = 0.0f;
    g_exitOn = false;            // no longer use the "grid flies in from the right" approach
    g_settingsOpen = false;      // leaving the player page also closes the settings panel
    g_seekDrag = false;
}

/** Position of the player page progress bar (drawing and hit-testing both use it so the two coordinate sets can't diverge) */
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

/** Map an x coordinate to a point in time in the current song */
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
    if (g_curtainPhase != 0) return;      // page is mid-swap: ignore clicks

    // Window buttons take priority
    if (p.y < UI::TITLE_H) {
        for (int i = 0; i < UI::BTN_COUNT; i++) {
            if (Inside(BtnRect(i, a.width), p)) {
                HWND hwnd = GetActiveWindow();
                if (i == WB_MIN) ShowWindow(hwnd, SW_MINIMIZE);
                else if (i == WB_MAX) {
                    WINDOWPLACEMENT wpl = { sizeof(wpl) };
                    GetWindowPlacement(hwnd, &wpl);
                    ShowWindow(hwnd, wpl.showCmd == SW_MAXIMIZE ? SW_RESTORE : SW_MAXIMIZE);
                } else ShowWindow(hwnd, SW_HIDE);   // close = hide to the tray, don't exit
                return;
            }
        }
        // There are still a bunch of buttons in the title bar, don't swallow the whole strip
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
    case HK_BACK:  ExitPlayer(); break;
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
        if (g_volMuted) {                       // unmute first
            g_volMuted = false;
            g_userVol = g_volBeforeMute;
            ApplyVolume();
        }
        g_volOpen = !g_volOpen;
        if (g_volOpen) g_qualOpen = false;   // don't open both panels at the same time
        break;
    case HK_SRC:
        if (h->index != g_source) {
            g_source = h->index;
            SaveSource();
            g_likesName = std::wstring(SourceTag()) + L"-likes.tsv";
    Logf("APP", "音源=%d 歌单文件=%s", g_source, Narrow(g_likesName).c_str());
            { std::lock_guard<std::mutex> lk(g_preMx); g_urlCache.clear(); }
            CloseAudioNow();
            g_audioOk = false;
            g_audioError = false;
            g_token.clear();
            g_userid.clear();
            g_nickname.clear();
            g_loginState = 0;
            LoadSession();
            InitData();
            ForgetShownPlaylist();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers(g_app.songs);
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
        g_scrollDrag = true;                       // follows the mouse as soon as it's pressed
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
            { std::lock_guard<std::mutex> lk(g_preMx); g_urlCache.clear(); }   // old-tier entries would stop PrefetchUrl
            if (g_audioOk && !g_audioLoading && g_fadeDir == 0 && a.current >= 0) {
                g_pendingSeekMs = (int)(a.pos * 1000.0);   // remember the progress, resume after the reload
                g_pendingPick = a.current;      // reload with the new quality after the fade-out
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
    case HK_SET_BG:                          // click on the scrim: close the panel
    case HK_SET_CLOSE:
        g_settingsOpen = false;
        g_loginChoose = false;
        if (g_loginRunning) {                  // closing the panel cancels this QR sign-in
            g_loginRunning = false;
            std::lock_guard<std::mutex> lk(g_lgMx);
            g_lgState = 0;
        }
        break;
    case HK_SET_CARD: break;                 // inside the card: swallow it, don't pass through
    case HK_SET_CLEAR: ClearCache(); break;
    case HK_SET_THEME: {
        ThemeNext();
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);   // colours changed, repaint immediately
        std::wstring msg = std::wstring(T(L"主题已切换：", L"Theme: ")) + ThemeName(ThemeCurrent());
        SettingsToast(msg.c_str());
        break;
    }
    case HK_SET_LOGIN: {
        // Sign in again: clear the cache and every platform's sign-in data, then pick a platform
        ClearCache();
        DeleteFileW((AssetDir() + L"\\kugou-session.json").c_str());
        DeleteFileW((AssetDir() + L"\\netease-session.json").c_str());
        DeleteFileW((AssetDir() + L"\\qq-session.json").c_str());
        g_token.clear();
        g_userid.clear();
        g_nickname.clear();
        g_vipType = 0;
        g_loginState = 0;
        g_loginAsked = false;
        g_loginRunning = false;                 // cancel the previous poll, which may still be running
        { std::lock_guard<std::mutex> lk(g_lgMx); g_lgState = 0; g_lgMsg.clear(); }
        CloseAudioNow();
        g_audioOk = false;
        g_audioError = false;
        SettingsToast(T(L"已清空缓存和登录信息", L"Cache and sign-in cleared"));
        g_loginChoose = !g_loginChoose;
        break;
    }
    case HK_LOGIN_PICK: {
        g_loginChoose = false;
        if (h->index != g_source) {              // switching platform: switch the data source along with it
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
            ForgetShownPlaylist();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers(g_app.songs);
            g_covFlag.assign(g_app.songs.size(), 0);
            g_layoutCount = -1;
            g_layoutWidth = -1;
        }
        StartRelogin();
        break;
    }
    case HK_PLAY:
        if (g_audioError) {               // after a failure, clicking again counts as a retry
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
        g_seekDrag = true;          // start dragging as soon as it's pressed
        g_lastSeek = GetTickCount64();
        SeekToX(p.x);
        break;
    }
    default: break;
    }
}

/* ══════════════════════════════════════════
   Window
   ══════════════════════════════════════════ */
/* ── DPI ─────────────────────────────────────────────
   The whole UI is laid out in DIPs (96 = 1x). The render target is told the real
   DPI, so Direct2D scales the drawing itself - every existing pixel constant keeps
   working, and text is rasterised at the native resolution instead of being stretched. */
static float g_dpiScale = 1.0f;     // physical pixels per DIP
static UINT  g_dpi      = 96;

typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);

static UINT WindowDpi(HWND hwnd) {
    static PFN_GetDpiForWindow fn =
        (PFN_GetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d >= 72) return d;
    }
    HDC dc = hwnd ? GetDC(hwnd) : nullptr;
    UINT d = 96;
    if (dc) { d = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(hwnd, dc); }
    return d ? d : 96;
}

static void RefreshDpi(HWND hwnd) {
    g_dpi = WindowDpi(hwnd);
    g_dpiScale = (float)g_dpi / 96.0f;
}

/* Mouse messages arrive in physical pixels; the UI works in DIPs */
static POINT DipPoint(LPARAM lp) {
    POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    p.x = (LONG)lroundf((float)p.x / g_dpiScale);
    p.y = (LONG)lroundf((float)p.y / g_dpiScale);
    return p;
}
/* Draws one frame and returns whether it reached the screen. EndDraw (with D2D1_PRESENT_OPTIONS_NONE)
   waits for the next vertical blank, so frames drawn back to back follow the display's refresh rate */
static bool Render(HWND hwnd) {
    ++g_paintCount;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT w = (UINT)(std::max)(1L, rc.right - rc.left);
    UINT h = (UINT)(std::max)(1L, rc.bottom - rc.top);

    RefreshDpi(hwnd);
    g_app.width  = (float)w / g_dpiScale;      // layout units: DIPs
    g_app.height = (float)h / g_dpiScale;

    if (!g_hwndRt) {
        g_d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(w, h)),
            &g_hwndRt);
    } else {
        D2D1_SIZE_U sz = g_hwndRt->GetPixelSize();
        if (sz.width != w || sz.height != h) g_hwndRt->Resize(D2D1::SizeU(w, h));
    }
    if (g_hwndRt) g_hwndRt->SetDpi((float)g_dpi, (float)g_dpi);

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
        }
    }
    return g_hwndRt != nullptr;   // false also while the target cannot be created, such as during a device reset
}

/* A window in the background draws about 60 frames per second, each shown for the same whole number of
   display refreshes. The next frame is drawn early in the last refresh before the vertical blank it is for */
static LONGLONG g_nextFrameQpc = 0;   // a window in the background may draw again from this QPC time
static LONGLONG QpcFrequency() {
    static const LONGLONG f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x.QuadPart; }();
    return f;
}
static void ScheduleBackgroundFrame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || !ti.qpcRefreshPeriod) {
        g_nextFrameQpc = now.QuadPart + QpcFrequency() / 60;
        return;
    }
    const LONGLONG period = (LONGLONG)ti.qpcRefreshPeriod;
    const LONGLONG refreshes = (std::max)(1LL, std::llround((double)QpcFrequency() / period / 60.0));
    // The frame just presented appears on the next vertical blank
    const LONGLONG sinceBlank = ((now.QuadPart - (LONGLONG)ti.qpcVBlank) % period + period) % period;
    const LONGLONG shown = now.QuadPart - sinceBlank + period;
    g_nextFrameQpc = shown + (refreshes - 1) * period + period / 4;
}

/* Advances animation and background work by one step, then draws a frame if anything changed.
   Returns how long the caller may sleep: 0 after drawing a frame, INFINITE when no frame is waiting */
static DWORD Tick(HWND hwnd) {
    const bool background = GetForegroundWindow() != hwnd;
    if (background) {
        // Stepping only when the frame is due makes each step, and so the motion blur, span the whole frame
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart < g_nextFrameQpc) return (DWORD)((g_nextFrameQpc - now.QuadPart) * 1000 / QpcFrequency() + 1);
    }
    bool need = Animate();
    if (g_mouseDirty) { g_mouseDirty = false; need = true; }
    if (g_coverDirty.exchange(false)) need = true;   // a cover was decoded in the background, repaint once
    static int shownLogin = 0;                       // worker threads update the login state; repaint to show the hint
    if (g_loginState != shownLogin) { shownLogin = g_loginState; need = true; }
    if (g_coverFading) { g_coverFading = false; need = true; }
    size_t bmpsBefore = g_coverBmps.size();
    if (g_app.page == Page::Grid) PrewarmCovers(2);   // only prewarm on the song grid
    if (g_coverBmps.size() != bmpsBefore) need = true;
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
    if (!need) return INFINITE;
    // A hidden, minimized or occluded window does not wait for the vertical blank, and neither does a frame
    // that fails to draw, so the loop would spin. The frame stays requested until it reaches the screen
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
        !(g_hwndRt && (g_hwndRt->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)) && Render(hwnd)) {
        if (background) ScheduleBackgroundFrame();
        return 0;
    }
    g_mouseDirty = true;
    return INFINITE;
}

/* The system asks for a frame, for example after InvalidateRect or when the window is uncovered.
   It goes through Tick so that animation steps first and the frame does not repeat the previous one */
static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    g_mouseDirty = true;
    Tick(hwnd);
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
        if (g_hwndRt) return 1;   // Direct2D draws every pixel once its render target exists
        break;

    case WM_NCCALCSIZE:
        return 0;   // the whole window is client area, also at creation when wParam is FALSE

    case WM_NCACTIVATE:
        // lParam -1 stops DefWindowProc from painting the classic border over the edges of the client area
        return DefWindowProc(hwnd, msg, wp, -1);

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        mmi->ptMinTrackSize.x = (LONG)lroundf(720.0f * g_dpiScale);
        mmi->ptMinTrackSize.y = (LONG)lroundf(520.0f * g_dpiScale);
        return 0;
    }

    case WM_DPICHANGED: {
        // Moved to a monitor with a different scale: resize to the suggested rect and
        // let the next paint pick up the new DPI for the render target
        RefreshDpi(hwnd);
        RECT* pr = (RECT*)lp;
        if (pr) {
            SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                         pr->right - pr->left, pr->bottom - pr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_mouseDirty = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        RECT rc;
        GetClientRect(hwnd, &rc);
        p.x = (LONG)lroundf((float)p.x / g_dpiScale);   // compare in DIPs below
        p.y = (LONG)lroundf((float)p.y / g_dpiScale);
        rc.right  = (LONG)lroundf((float)rc.right  / g_dpiScale);
        rc.bottom = (LONG)lroundf((float)rc.bottom / g_dpiScale);

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
            if (p.x >= btnLeft) return HTCLIENT;          // the three window buttons
            // The settings button must be HTCLIENT too, otherwise it's treated as a title-bar drag and can't be clicked
            float gearX = BtnRect(0, (float)rc.right).left - 40.0f;
            if (p.x >= gearX - 4.0f && p.x <= gearX + 36.0f) return HTCLIENT;
            float pbX = gearX - 44.0f;
            if (p.x >= pbX - 4.0f && p.x <= pbX + 36.0f) return HTCLIENT;
            // Back-to-playlists button (another 44px left of the disc button): it must be HTCLIENT too,
            // otherwise it falls into the HTCAPTION branch above and is treated as a window drag
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
        if (g_app.page == Page::Playlists && g_app.plDragging && !g_settingsOpen) {   // finish the drag even if a transition started
            POINT mp = DipPoint(lp);
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
                    POINT mp = DipPoint(lp);
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
        POINT mpd = DipPoint(lp);
        int mx = mpd.x, my = mpd.y;
        if (g_volDrag) {                        // dragging the volume: follow it directly
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
        if (g_seekDrag) {                       // dragging the progress bar: follow the mouse
            ULONGLONG now = GetTickCount64();
            if (now - g_lastSeek > 60) {        // at most one seek request every 60 ms while dragging
                g_lastSeek = now;
                SeekToX((float)mx);
            }
        }
        if (g_app.mouse.x != mx || g_app.mouse.y != my) {
            g_app.mouse.x = mx;
            g_app.mouse.y = my;
            g_hoverArmed = true;      // mouse moved, re-enable hover zoom
            g_mouseDirty = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.mouse = { -1, -1 };
        g_app.hoverTile = -1;
        g_mouseDirty = true;
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p = DipPoint(lp);
        if (g_app.page == Page::Playlists && p.y > (LONG)UI::TITLE_H && !g_settingsOpen && g_plPhase == 0) {
            // Only drag below the top bar, and only while the settings panel is closed;
            // when the panel is open mouse events must reach OnClick, otherwise nothing in the panel can be clicked
            // Press feedback: remember which disc is held so it can scale down until release
            g_app.plPress = -1;
            if (g_app.plActive < 0) {
                float best = PL_D * 0.5f;
                for (int i = 0; i < (int)g_app.playlists.size(); i++) {
                    float cx = 0, cy = 0;
                    PlScreenPos(i, cx, cy);
                    float ddx = (float)p.x - cx, ddy = (float)p.y - cy;
                    float d2 = ddx * ddx + ddy * ddy;
                    if (d2 <= best * best) { best = sqrtf(d2); g_app.plPress = i; }
                }
            }
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
        if (g_app.page == Page::Playlists && g_app.plDragging && !g_settingsOpen) {   // finish the drag even if a transition started
            g_app.plDragging = false;
            g_app.plPress = -1;
            ReleaseCapture();
            if (g_app.plDragMoved) {
                g_app.plActive = -1;                // dragging cancels the selection
                g_app.plTargetX = g_app.plX;
                g_app.plTargetY = g_app.plY;
            } else {
                // Treat it as a click: find the circle under the mouse
                float best = PL_D * 0.5f;
                int hit = -1;
                for (int i = 0; i < (int)g_app.playlists.size(); i++) {
                    float cx = 0, cy = 0;
                    PlScreenPos(i, cx, cy);
                    float dx = (float)g_app.mouse.x - cx, dy = (float)g_app.mouse.y - cy;
                    float d2 = dx * dx + dy * dy;
                    if (d2 <= best * best) { best = sqrtf(d2); hit = i; }
                }
                // Expanded state: the whole card is clickable and any click enters the playlist; clicking outside collapses it
                if (g_app.plActive >= 0) {
                    POINT mp = DipPoint(lp);
                    D2D1_RECT_F card = PlCardTargetRect();
                    bool inCard = (mp.x >= card.left - 6 && mp.x <= card.right + 6 &&
                                   mp.y >= card.top - 6 && mp.y <= card.bottom + 6);
                    if (inCard && g_app.plSelT > 0.9f) {
                        // Start the transition: the page breaks apart, then the loading ring
                        g_plPhase = 1;
                        g_plEnterX = (float)g_app.mouse.x;
                        g_plEnterY = (float)g_app.mouse.y;
                        g_plT = 0.0f;
                        g_swapDone = false;
                        SelectPlaylistAsync(g_app.plActive);
                    } else {
                        g_app.plActive = -1;
                        g_app.plTargetX = 0.0f;
                        g_app.plTargetY = 0.0f;
                        g_app.plHover = -1;
                    }
                    g_mouseDirty = true;
                    return 0;
                }

                if (hit >= 0) {
                    if (g_app.plActive == hit && g_app.plSelT > 0.9f) {
                        g_plPhase = 1;              // clicked again: enter the playlist
                        g_plEnterX = (float)g_app.mouse.x;
                        g_plEnterY = (float)g_app.mouse.y;
                        g_plT = 0.0f;
                        g_swapDone = false;
                        SelectPlaylistAsync(hit);
                    } else {
                        // First click: select it + move this card to the centre of the window
                        g_app.plActive = hit;
                        g_app.plSelLast = hit;      // kept for the collapse animation
                        g_app.plSelT = 0.0f;
                        g_app.plHover = -1;
                        if (g_plCellsFor != (int)g_app.playlists.size())
                            BuildPlCells((int)g_app.playlists.size());
                        // The card expands toward the window centre, the camera stays put
                        g_app.plTargetX = 0.0f;
                        g_app.plTargetY = 0.0f;
                        g_app.plVelX = g_app.plVelY = 0.0f;
                    }
                    g_mouseDirty = true;
                }
            }
            return 0;
        }
        if (g_seekDrag) g_togForcePush = true;      // finished dragging the progress bar: sync the position once
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
        g_hoverArmed = false;         // no hover pop-open during or after scrolling
        g_app.hoverTile = -1;
        g_mouseDirty = true;
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_settingsOpen) {
                g_settingsOpen = false;
            } else if (g_togOpen) {
                g_togOpen = false;
            } else if (g_app.page == Page::Player) {
                ExitPlayer();
            } else if (g_app.page == Page::Grid) {
                GoPicker();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // Song grid: arrow keys / PgUp PgDn to scroll
        if (g_app.page == Page::Grid && !g_settingsOpen && !g_togOpen) {
            const float viewH = (std::max)(0.0f, (float)g_app.height - GRID_TOP - UI::PAD);
            float step = 0.0f;
            if (wp == VK_UP)        step = -110.0f;                    // about one row
            else if (wp == VK_DOWN) step = 110.0f;
            else if (wp == VK_PRIOR) step = -viewH + 40.0f;            // PgUp: one screen
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
        // Together input box: Ctrl+V to paste (a custom-drawn box has no system editor, so read the clipboard ourselves)
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
        // Ctrl+A clears everything so it's easy to retype
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
            if (c == 8) {                                  // backspace
                if (!g_togInputText.empty()) g_togInputText.pop_back();
            } else if (c == 13) {                          // Enter to join
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
    case WM_TIMER:
        Tick(hwnd);
        return 0;

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
            PostMessage(hwnd, WM_NULL, 0, 0);      // let the menu dismiss properly
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
        CloseAudioNow();                    // stop the device and join the audio threads
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ══════════════════════════════════════════
   Offscreen rendering (dev only)
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

/* ── Remember the window position ── */
static std::wstring WindowFile() { return LocalDir() + L"\\window.txt"; }

static void SaveWindowRect(HWND hwnd) {
    // rcNormalPosition is the restored rect, also while the window is minimized or maximized
    WINDOWPLACEMENT wpl = { sizeof(wpl) };
    if (!GetWindowPlacement(hwnd, &wpl)) return;
    const RECT& r = wpl.rcNormalPosition;
    FILE* f = nullptr;
    _wfopen_s(&f, WindowFile().c_str(), L"wb");
    if (!f) return;
    fprintf(f, "%ld %ld %ld %ld", r.left, r.top, r.right - r.left, r.bottom - r.top);
    fclose(f);
}

/* Read the last position; ignore it if it lands off-screen (must still open with a secondary monitor unplugged) */
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
    if (!mon) return false;                 // that monitor is gone now
    x = (int)a; y = (int)b; w = (int)c; h = (int)d;
    return true;
}
/* ══════════════════════════════════════════
   Entry point
   ══════════════════════════════════════════ */
/** On a crash, write down the exception code and address so it can be located later */
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n ? std::wstring(buf, n) : L".") + L"\\KugouPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = nullptr;
    _wfopen_s(&f, (dir + L"\\crash.txt").c_str(), L"wb");
    if (f) {
        const EXCEPTION_RECORD* r = ep->ExceptionRecord;
        // Look up which module the exception address belongs to, and the offset inside that module
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

/* Unhandled exception: record exactly what it was, don't just abort silently */
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
    // Belt and braces: the manifest already declares PerMonitorV2, but if it is ever
    // stripped (some packers do), this keeps the window from being bitmap-stretched.
    {
        typedef BOOL (WINAPI* PFN_SetCtx)(HANDLE);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        auto setCtx = u32 ? (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
        if (setCtx) setCtx((HANDLE)-4);      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(OnTerminate);
    timeBeginPeriod(1);              // raise timer resolution, reduce frame interval jitter
    LogInit();
    AssetDir();                      // resolve/create the data directory before anything reads it
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
    LoadSource();                    // the source must be set first so LoadSession knows which file to read
    g_likesName = std::wstring(SourceTag()) + L"-likes.tsv";
    Logf("APP", "音源=%d 歌单文件=%s", g_source, Narrow(g_likesName).c_str());
    LoadSession();
    LoadLang();
    InitData();
    Logf("APP", "歌单载入 %d 首", (int)g_app.songs.size());
    LoadPlaylists();
    AdoptLocalCovers(g_app.songs);   // only fills in paths, no decoding: startup no longer loads song covers
    // The first screen waits for the playlist list only when none is cached; a cached list shows at once
    // and the refresh below replaces it if it changed
    if (!g_token.empty() && g_app.playlists.empty()) g_bootRefreshPending = true;
    std::thread([token = g_token, userid = g_userid]() {
        EnsureSidecar();
        CheckLoginAsync();
        if (!token.empty()) {
            Logf("APP", "启动刷新歌单列表…");
            FetchPlaylistsAsync(token, userid);
        }
    }).detach();
#ifndef FLORAL_NO_TOGETHER
    std::thread(StartTogetherHelper).detach();   // login and playlists do not depend on it, so it starts separately
#endif
    if (!GfxInit()) {
        MessageBoxW(nullptr, T(L"图形初始化失败", L"Graphics init failed"), T(L"错误", L"Error"), MB_ICONERROR);
        return 1;
    }

    // Start the background decode thread and queue up all the covers
    StartDecoder();

    // Download covers for songs that lack one, on demand
    g_covFlag.assign(g_app.songs.size(), 0);
    std::thread(CoverWorkerMain).detach();

    // Command line
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
            g_settingsOpen = true; g_setT = 1.0f;   // for screenshots: open the settings panel directly
        } else if (a == L"--loading") {
            g_audioLoading = true;        // for screenshots: keep the spinner visible
            g_loadingSince = GetTickCount64() - 1000;
            g_spinAngle = 0.8f;
        } else if (a == L"--trace") {
            g_traceOn = true;
        } else if (a == L"--choose") {
            g_loginChoose = true; g_settingsOpen = true; g_setT = 1.0f;
        } else if (a == L"--icont" && i + 1 < argc) {
            iconT = (float)_wtof(argv[++i]);        // for screenshots: play-icon crossfade progress
        } else if (a == L"--toastt" && i + 1 < argc) {
            toastT = (float)_wtof(argv[++i]);      // for screenshots: toast opacity
        } else if (a == L"--textphase" && i + 1 < argc) {
            textPhase = _wtoi(argv[++i]);          // for screenshots: 1 exit / 2 enter
        } else if (a == L"--textt" && i + 1 < argc) {
            textT = (float)_wtof(argv[++i]);       // for screenshots: text enter progress
        } else if (a == L"--covert" && i + 1 < argc) {
            coverT = (float)_wtof(argv[++i]);      // for screenshots: cover fade-in progress
        } else if (a == L"--panelt" && i + 1 < argc) {
            panelT = (float)_wtof(argv[++i]);     // for screenshots: freeze the panel at a given progress
        } else if (a == L"--volopen") {
            g_volOpen = true; g_volT = 1.0f;   // for screenshots
        } else if (a == L"--qualopen") {
            g_qualOpen = true; g_qualT = 1.0f;
        } else if (a == L"--enter" && i + 1 < argc) {
            enterT = (float)_wtof(argv[++i]);   // for screenshots: freeze the enter animation at one moment
        } else if (a == L"--loginstate" && i + 1 < argc) {
            g_loginState = _wtoi(argv[++i]);   // for screenshots: fake the sign-in state 1/2/3
        } else if (a == L"--audiotest" && i + 1 < argc) {
            audioTest = argv[++i];
        }
    }
    if (argv) LocalFree(argv);

    // Audio engine self-test: open, play for 1.2 s, write the state to audiotest.txt
    if (!audioTest.empty()) {
        std::wstring log;
        bool ok = AudioOpen(audioTest);
        log += ok ? L"open OK\n" : (L"open FAIL: " + AudioLastError() + L"\n");
        if (ok) {
            AudioPlay();
            Sleep(1200);
            wchar_t b[128];
            swprintf(b, 128, L"pos=%d playing=%d\n", AudioPosMS(), (int)AudioIsPlaying());
            log += b;

            // Also verify that seek works on a stream
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
        StopDecoder();
        CoUninitialize();
        return 0;
    }

    if (!mode.empty()) {
        if (outPath.empty()) outPath = L"shot.png";
        g_app.width = 960.0f;
        g_app.height = 640.0f;
        g_snapUI = true;              // screenshots go straight to the final state
        g_introDone = true;           // screenshots skip the splash animation
        g_lineT = 1.0f;               // the divider extends all the way

        // Screenshots must be complete: wait for the background to decode all covers
        {
            size_t want = g_decRequested.size();
            for (int i = 0; i < 2000 && g_coverSrcs.size() < want; i++) {
                DrainDecoded();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            DrainDecoded();
        }

        if (page == L"playlists") {            // for screenshots: the playlist selection page
            g_app.page = Page::Playlists;
            PlCenterContent();
        } else if (page == L"empty") {         // for screenshots: player page with no song selected
            g_app.current = -1;
            g_app.page = Page::Player;
            g_app.pageAnim = 1.0f;
        } else if (page == L"player" && !g_app.songs.empty()) {
            g_app.current = 0;
            g_app.playing = true;
            g_app.pos = 62.0;
            g_app.page = Page::Player;
            g_app.pageAnim = 1.0f;
            if (enterT >= 0.0f) {              // freeze one frame of the enter animation
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
            // Compute the end state directly so the screenshot shows the "pushed apart" layout
            EnsureBase(g_app.width, (int)g_app.songs.size());
            // The mouse must land on this tile in the base layout, otherwise hit-testing picks a neighbour
            const TileBox& bb = g_base[(size_t)hoverIdx];
            g_app.mouse.x = (LONG)(bb.x + bb.w * 0.5f);
            g_app.mouse.y = (LONG)(GRID_TOP + bb.y + bb.h * 0.5f);
            EnsureTarget(g_app.width, (int)g_app.songs.size(), hoverIdx);
            g_draw = g_target;
        }

        g_noCoverFade = true;   // screenshots: covers at full opacity, don't capture the placeholder colour
    if (coverT >= 0.0f) { g_noCoverFade = false; g_coverFadeOverride = coverT; }
    if (iconT >= 0.0f) {          // screenshots: pin the morph parameter at a value (0 = play, 1 = pause)
        g_app.playing = (iconT > 0.5f);
        g_playIconMix = iconT;
    }
    if (toastT >= 0.0f) {
        g_toastOverride = toastT;
        g_settingsMsg = L"歌单已更新";     // fabricate a toast for the screenshot
        g_settingsMsgAt = NowMs();
        g_togMsg = std::wstring(T(L"已和 ", L"Connected with ")) + L"朋友的电脑";
        g_togMsgAt = NowMs();
    }
    if (textT >= 0.0f) {
        g_textT = textT;
        if (textPhase >= 0) g_textPhase = textPhase;
        if (g_textPhase == 1 && g_outTitle.empty() && !g_app.songs.empty()) {
            // No real track switch during screenshots, so fabricate the old text by hand
            g_outTitle = L"上一首歌的名字";
            g_outArtist = L"上一位歌手";
            g_outAlbum = L"上一张专辑";
        }
    }
    if (panelT >= 0.0f) {   // only pin the progress of the panel that's currently open
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

    // Frameless window
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    // Fills the window until Direct2D draws the first frame; without it the window shows white
    wc.hbrBackground = CreateSolidBrush(RGB((UI::BG >> 16) & 0xFF, (UI::BG >> 8) & 0xFF, UI::BG & 0xFF));
    wc.lpszClassName = L"SongListWnd";
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    // Size the window in physical pixels for the DPI we start on
    HDC screenDc = GetDC(nullptr);
    UINT startDpi = screenDc ? (UINT)GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) ReleaseDC(nullptr, screenDc);
    if (startDpi < 72) startDpi = 96;
    g_dpi = startDpi;
    g_dpiScale = (float)g_dpi / 96.0f;
    const int startW = (int)lroundf(960.0f * g_dpiScale);
    const int startH = (int)lroundf(640.0f * g_dpiScale);

    HWND hwnd = CreateWindowExW(0, L"SongListWnd", L"Floral", style,
        CW_USEDEFAULT, CW_USEDEFAULT, startW, startH, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    int cornerPref = 2;
    DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));

    g_hwnd = hwnd;
    AddTrayIcon(hwnd);
    {
        int wx, wy, ww, wh;
        if (LoadWindowRect(wx, wy, ww, wh)) {
            // The saved rect is in workspace coordinates, which SetWindowPlacement expects and SetWindowPos does not.
            // SW_HIDE keeps the window hidden until ShowWindow below
            WINDOWPLACEMENT wpl = { sizeof(wpl) };
            wpl.showCmd = SW_HIDE;
            wpl.rcNormalPosition = { wx, wy, wx + ww, wy + wh };
            SetWindowPlacement(hwnd, &wpl);
            Logf("APP", "恢复窗口位置 %d,%d %dx%d", wx, wy, ww, wh);
        }
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    // Keeps animation and background work going while the loop below sleeps, the window is hidden,
    // or a modal loop (window drag, tray menu) holds the thread
    SetTimer(hwnd, 1, 8, nullptr);

    // While anything animates, each pass draws a frame and EndDraw paces it to the display;
    // otherwise the thread sleeps until a message, the timer or the next background frame arrives
    MSG msg;
    for (;;) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else if (DWORD wait = Tick(hwnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
        }
    }

    if (g_hwndRt) g_hwndRt->Release();
    StopDecoder();
    GfxRelease();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
