#include "vinyl.h"
#include "log.h"

#include <d2d1_1.h>
#include <d2d1effectauthor.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <future>
#include <string>

namespace {

/* Lighting is computed in screen space, so the highlights stay roughly in place while the record turns.
   The groove texture, the dust and a slight warp of the surface use the record's own angle and turn with it */
const char VINYL_HLSL[] = R"(
cbuffer Params : register(b0) {
    float2 center;   // pixels
    float  radius;   // pixels
    float  opacity;
    float2 spin;     // cosine and sine of the rotation
    float  label;    // label radius over disc radius
    uint   seed;     // varies the track layout and texture from record to record
    float3 base;     // linear color of the vinyl
};

static const float TAU = 6.2831853;
// Point lights in disc radii from the center: x right, y down, z toward the viewer
static const float3 KEY  = float3(0.8, -1.4, 1.3);
static const float3 FILL = float3(1.7, 0.8, 1.1);
static const float3 LIGHT = float3(1.0, 0.97, 0.93);

uint Hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

// Random bits for cell i of the given noise layer
uint Cell(uint i, uint layer) {
    return Hash(i * 0x9e3779b9u + layer * 0x85ebca6bu + seed * 0xc2b2ae35u);
}

// Uniform in [0, 1) for cell i of the given noise layer
float Rand(uint i, uint layer) {
    return Cell(i, layer) * (1.0 / 4294967296.0);
}

// Smooth value noise along x >= 0
float Noise(float x, uint layer) {
    float i = floor(x), f = x - i;
    return lerp(Rand((uint)i, layer), Rand((uint)i + 1u, layer), f * f * (3.0 - 2.0 * f));
}

// Smooth value noise along groove line `ring`: u in [0, 1) goes once around and wraps over `cells` cells
float Around(float u, uint ring, uint cells, uint layer) {
    float x = u * cells, i = floor(x), f = x - i;
    uint x0 = (uint)i % cells, x1 = (x0 + 1u) % cells;
    return lerp(Rand(ring * cells + x0, layer), Rand(ring * cells + x1, layer), f * f * (3.0 - 2.0 * f));
}

// Share of the pixel footprint [x - w / 2, x + w / 2] inside [a, b]
float Cover(float x, float a, float b, float w) {
    return saturate((min(x + 0.5 * w, b) - max(x - 0.5 * w, a)) / w);
}

// Mirror highlight on a surface tilted outward by `tilt`, for half vector h and its component `along` the ring.
// Across the ring it widens by the tilt range one pixel covers, so thin rings do not break into dots
float Glint(float3 h, float2 n, float along, float tilt, float spread) {
    float off = tilt - atan2(dot(h.xy, n), h.z);
    float m = 1.0 / (1.0 / 150.0 + spread * spread / 12.0);
    return sqrt(m / 150.0) * exp(-75.0 * along * along - 0.5 * m * off * off);
}

float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION) : SV_Target {
    float2 p = scene.xy - center;
    float d = length(p);
    // Opaque from a pixel inside the label edge, so the label drawn over it leaves no seam
    float alpha = opacity * saturate(radius + 0.5 - d) * saturate(d - label * radius + 1.5);
    if (alpha <= 0.0) return 0;

    float2 n = p / d;
    float width = (1.0 - label) * radius;     // the vinyl ring in pixels
    float s = (d - label * radius) / width;   // 0 at the label edge, 1 at the rim
    float ps = 1.0 / width;                   // one pixel in s

    // Track layout: grooved music between a smooth run-out and a lead-in, with a gap after each track
    float tracks = 4.0 + (Hash(seed) & 3u);
    float t = (s - 0.17) / 0.76 * tracks;
    float k = round(t);
    // Edges between two tracks are shifted a little and leave a gap; the music starts and ends at fixed radii
    bool between = k >= 1.0 && k < tracks;
    float edge = k + (between ? (Rand((uint)k, 1u) - 0.5) * 0.6 : 0.0);
    float edgeS = 0.17 + edge / tracks * 0.76;
    float gap = between ? Cover(s, edgeS - 0.006, edgeS + 0.006, ps) : 0.0;
    float grooves = Cover(s, 0.17, 0.93, ps) * (1.0 - gap)
                  + 0.4 * Cover(s, 0.93, 0.955, ps)
                  + 0.5 * (Cover(s, 0.105, 0.112, ps) + Cover(s, 0.135, 0.142, ps));
    float track = t < edge ? k - 1.0 : k;
    float level = (0.75 + 0.5 * Rand((uint)(track + 1.0), 2u)) * (0.7 + 0.6 * Noise(s * 12.0, 3u));

    // Groove lines, each with its own brightness that also varies along it, so it turns with the record.
    // A cosine across each line keeps the rings free of moire; it fades out as the lines near two pixels apart
    float2 q = float2(dot(p, spin), p.y * spin.x - p.x * spin.y);
    float u = frac(atan2(q.y, q.x) / TAU + 1.0);
    float phase = s * 50.0, ring = floor(phase), period = width / 50.0;
    float strand = (0.3 + 0.7 * Rand((uint)ring, 5u)) * (0.3 + 1.4 * Around(u, (uint)ring, 40u, 6u));
    float crest = 0.5 - 0.5 * cos(TAU * phase);
    float ridges = lerp(1.0, 0.45 + 1.7 * crest * strand, saturate(period - 2.0));

    // Dust: each groove line is split into roughly square cells, one speck in about 256 of them
    float cols = floor(TAU * (label / (1.0 - label) * 50.0 + ring + 0.5));
    float col = u * cols, i = floor(col);
    uint h = Cell((uint)(ring * 4096.0 + i), 9u);
    float2 at = float2(h & 255u, (h >> 8) & 255u) / 255.0 * 0.6 + 0.2;
    float size = 0.4 + (h >> 24) / 255.0 * 0.6;
    float dust = ((h >> 16) & 255u) == 0u
               ? saturate(size + 0.5 - length((float2(col - i, phase - ring) - at) * period)) * saturate(period - 1.5)
               : 0.0;

    float3 P = float3(p / radius, 0.0);
    float3 lk = KEY - P, lf = FILL - P;
    float3 hk = normalize(normalize(lk) + float3(0.0, 0.0, 1.0));
    float3 hf = normalize(normalize(lf) + float3(0.0, 0.0, 1.0));
    float fk = dot(KEY, KEY) / dot(lk, lk), ff = dot(FILL, FILL) / dot(lf, lf);

    // The record is not quite flat: its height rises and falls a few times around the circle, so as it turns
    // the surface under the lights tilts slightly, and the streaks waver and grow brighter and dimmer
    float2 e1 = q / d, e2 = float2(e1.x * e1.x - e1.y * e1.y, 2.0 * e1.x * e1.y);
    float2 e3 = float2(e2.x * e1.x - e2.y * e1.y, e2.x * e1.y + e2.y * e1.x);
    float2 e4 = float2(e2.x * e2.x - e2.y * e2.y, 2.0 * e2.x * e2.y);
    float4 cosK = float4(e1.x, e2.x, e3.x, e4.x), sinK = float4(e1.y, e2.y, e3.y, e4.y);
    float4 amp = float4(0.012, 0.012, 0.008, 0.005);
    float4 wa = (float4(Rand(0u, 10u), Rand(1u, 10u), Rand(2u, 10u), Rand(3u, 10u)) - 0.5) * amp;
    float4 wb = (float4(Rand(4u, 10u), Rand(5u, 10u), Rand(6u, 10u), Rand(7u, 10u)) - 0.5) * amp;
    float r = d / radius;
    float rise = 2.0 * r * (dot(wa, cosK) + dot(wb, sinK));                         // slope outward
    float lean = r * dot(float4(1.0, 2.0, 3.0, 4.0), wb * cosK - wa * sinK);          // slope along the grooves

    // Grooves reflect a light where the half vector lies across them, which draws a streak through the center.
    // The lobes are as wide as those of large lights, so the streaks fade out softly on both sides
    float2 T = float2(-n.y, n.x);
    float ak = dot(hk.xy, T) + hk.z * lean, af = dot(hf.xy, T) + hf.z * lean;
    float sheen = (1.0 + 8.0 * rise)
                * (fk * (0.45 * exp(-250.0 * ak * ak) + 0.05 * exp(-50.0 * ak * ak))
                 + ff * (0.15 * exp(-150.0 * af * af) + 0.02 * exp(-35.0 * af * af)));

    // Smooth surfaces tilt outward at the raised ring beside the label and at the rounded rim
    float bead = saturate((s - 0.04) / 0.04), rim = saturate((s - 0.955) / 0.045);
    float tilt = 0.3 * sin(TAU * bead) + 1.4 * rim * rim - rise;
    float spread = ((bead > 0.0 && bead < 1.0 ? 47.0 * abs(cos(TAU * bead)) : 0.0) + 62.0 * rim) * ps;
    // On small records the ring and the rim are a few pixels wide, too narrow to show a highlight
    float gloss = saturate(0.02 * width - 1.0)
                * (0.6 * fk * Glint(hk, n, ak, tilt, spread) + 0.2 * ff * Glint(hf, n, af, tilt, spread));

    float3 c = base * (1.0 + 0.3 * grooves * ridges)
             + LIGHT * (grooves * level * ridges * sheen + gloss + dust * (0.02 + 0.5 * sheen));
    c = saturate(1.0 - exp(-c));
    float dither = (Rand((uint)pos.x + ((uint)pos.y << 16), 8u) - 0.5) / 255.0;
    return float4((pow(c, 1.0 / 2.2) + dither) * alpha, alpha);
}
)";

const wchar_t VINYL_XML[] = LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='Vinyl'/>
  <Property name='Author' type='string' value='Floral'/>
  <Property name='Category' type='string' value='Source'/>
  <Property name='Description' type='string' value='Draws the vinyl of a record'/>
  <Inputs/>
  <Property name='Params' type='blob'>
    <Property name='DisplayName' type='string' value='Params'/>
  </Property>
</Effect>)";

const GUID CLSID_Vinyl      = { 0x0e9643bc, 0xf9f2, 0x4049, { 0xb4, 0x72, 0xe0, 0x25, 0x27, 0x00, 0xea, 0xbf } };
const GUID GUID_VinylShader = { 0x7f3e903c, 0x4cbf, 0x4d9e, { 0x8f, 0x15, 0xed, 0xc1, 0x8a, 0xeb, 0x6f, 0xeb } };

/* The shader's constant buffer, laid out by the HLSL packing rules */
struct Params {
    float cx, cy, radius, opacity;
    float cosA, sinA, label;
    UINT32 seed;
    float r, g, b, pad;
};

std::future<std::string> g_compiling;   // pixel shader being compiled on a worker thread
std::string    g_code;                  // compiled pixel shader, empty when compiling failed
ID2D1Effect*   g_fx = nullptr;

class VinylEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    static HRESULT CALLBACK Create(IUnknown** effect) {
        *effect = static_cast<ID2D1EffectImpl*>(new VinylEffect());
        return S_OK;
    }

    /* Accessors for the Params property; Direct2D passes back the IUnknown returned by Create */
    static HRESULT CALLBACK SetParams(IUnknown* effect, const BYTE* data, UINT32 size) {
        if (size != sizeof(Params)) return E_INVALIDARG;
        memcpy(&static_cast<VinylEffect*>(static_cast<ID2D1EffectImpl*>(effect))->m_params, data, size);
        return S_OK;
    }
    static HRESULT CALLBACK GetParams(const IUnknown* effect, BYTE* data, UINT32 size, UINT32* actual) {
        if (actual) *actual = sizeof(Params);
        if (!data || size < sizeof(Params)) return E_NOT_SUFFICIENT_BUFFER;
        memcpy(data, &static_cast<const VinylEffect*>(static_cast<const ID2D1EffectImpl*>(effect))->m_params,
               sizeof(Params));
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
        HRESULT hr = context->LoadPixelShader(GUID_VinylShader, reinterpret_cast<const BYTE*>(g_code.data()),
                                              (UINT32)g_code.size());
        return SUCCEEDED(hr) ? graph->SetSingleTransformNode(this) : hr;
    }
    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) override {
        return m_info->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&m_params), sizeof(m_params));
    }
    IFACEMETHODIMP SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }

    IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* info) override {
        m_info = info;
        m_info->AddRef();
        return info->SetPixelShader(GUID_VinylShader);
    }
    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L*, D2D1_RECT_L*, UINT32) const override {
        return S_OK;
    }
    /* With no inputs, the output is the disc's bounding box */
    IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L*, const D2D1_RECT_L*, UINT32,
                                           D2D1_RECT_L* output, D2D1_RECT_L* opaque) override {
        const float reach = m_params.radius + 1.0f;
        *output = { (LONG)floorf(m_params.cx - reach), (LONG)floorf(m_params.cy - reach),
                    (LONG)ceilf(m_params.cx + reach), (LONG)ceilf(m_params.cy + reach) };
        *opaque = { 0, 0, 0, 0 };
        return S_OK;
    }
    IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L invalid, D2D1_RECT_L* output) const override {
        *output = invalid;
        return S_OK;
    }
    IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 0; }

private:
    ~VinylEffect() { if (m_info) m_info->Release(); }

    ULONG          m_refs = 1;
    Params         m_params = {};
    ID2D1DrawInfo* m_info = nullptr;
};

float Linear(UINT32 channel) { return powf((channel & 0xFF) / 255.0f, 2.2f); }

/* Compiles the pixel shader with the system's d3dcompiler_47.dll; returns empty bytecode on failure */
std::string Compile() {
    std::string bytecode;
    HMODULE dll = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    pD3DCompile compile = dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (compile && SUCCEEDED(compile(VINYL_HLSL, sizeof(VINYL_HLSL) - 1, nullptr, nullptr, nullptr, "main",
                                     "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        bytecode.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        code->Release();
    } else {
        Logf("ERR", "唱片着色器编译失败：%s",
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "无法加载 d3dcompiler_47.dll");
    }
    if (errors) errors->Release();
    if (dll) FreeLibrary(dll);
    return bytecode;
}

}  // namespace

void VinylInit(ID2D1Factory* factory) {
    g_compiling = std::async(std::launch::async, Compile);

    ID2D1Factory1* factory1 = nullptr;
    HRESULT hr = factory->QueryInterface(IID_PPV_ARGS(&factory1));
    if (SUCCEEDED(hr)) {
        const D2D1_PROPERTY_BINDING bindings[] = {
            { L"Params", VinylEffect::SetParams, VinylEffect::GetParams },
        };
        hr = factory1->RegisterEffectFromString(CLSID_Vinyl, VINYL_XML, bindings, 1, VinylEffect::Create);
        factory1->Release();
    }
    if (FAILED(hr)) Logf("ERR", "唱片效果注册失败");
}

bool VinylDraw(ID2D1RenderTarget* rt, float cx, float cy, float radius, float label,
               float angleDeg, unsigned seed, UINT32 color, float opacity) {
    if (g_compiling.valid()) g_code = g_compiling.get();   // the first draw waits for the compiler if needed
    if (g_code.empty()) return false;
    ID2D1DeviceContext* dc = nullptr;
    if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc)))) return false;
    if (!g_fx) dc->CreateEffect(CLSID_Vinyl, &g_fx);
    if (g_fx) {
        // Effects work in pixels
        float dpiX = 96.0f, dpiY = 96.0f;
        rt->GetDpi(&dpiX, &dpiY);
        const float sx = dpiX / 96.0f, sy = dpiY / 96.0f;
        const float a = angleDeg * 3.14159265f / 180.0f;
        const Params p = { cx * sx, cy * sy, radius * sx, opacity, cosf(a), sinf(a), label, seed,
                           Linear(color >> 16), Linear(color >> 8), Linear(color), 0.0f };
        g_fx->SetValue(0, p);
        dc->DrawImage(g_fx);
    }
    dc->Release();
    return g_fx != nullptr;
}

void VinylRelease() {
    if (g_fx) { g_fx->Release(); g_fx = nullptr; }
}
