// NVOF interpolator test: synthetic scenes with known motion rendered on the GPU, PSNR of the
// interpolated frame vs the analytically correct intermediate frame (flow interpolator vs blend),
// GPU/CPU timings, and a cross-engine (D3D11 queue <-> OFA engine) ordering check.
//
// Usage: nvof_test [--dump <dir>] [--frames N] [--quick] [--probe]
//   --quick  PSNR table only;  --probe  raw NVOF D3D11-API ordering probe only;
//   --dump   writes [ground truth | output | |error|x4] crops for t=0.5 of the last pair of each case.
#include "interp/interpolator.h"
#include "gfx/device.h"
#include "nvOpticalFlowD3D11.h"
#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>

using namespace camss;

static const UINT W = 1920, H = 1080;

// ------------------------------------------------------------------------------------------ synth
static const char* kSynthCS = R"(
cbuffer CB : register(b0) {
    uint W, H, heavy, hasObj;
    float2 bgOff; float bgScale; float pad0;
    float2 center; float2 objC;
    float objR; float3 pad1;
};
RWTexture2D<unorm float4> O : register(u0);

uint Hash(int2 p, uint seed) {
    uint h = (uint)p.x * 374761393u + (uint)p.y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float Rnd(int2 p, uint seed) { return (float)(Hash(p, seed) & 0xffffff) / 16777215.0; }
float VNoise(float2 p, uint seed) {
    float2 i = floor(p); float2 f = p - i;
    float2 u = f * f * f * (f * (f * 6 - 15) + 10);
    int2 ii = int2(i);
    float a = Rnd(ii, seed), b = Rnd(ii + int2(1, 0), seed), c = Rnd(ii + int2(0, 1), seed), d = Rnd(ii + int2(1, 1), seed);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float Fbm(float2 p, uint seed) {
    return 0.32 * VNoise(p / 64, seed) + 0.24 * VNoise(p / 32, seed + 1) + 0.18 * VNoise(p / 16, seed + 2)
         + 0.14 * VNoise(p / 8, seed + 3) + 0.12 * VNoise(p / 4, seed + 4);
}
float3 Bg(float2 p) {
    float a = Fbm(p, 1), b = Fbm(p + 1000, 7), c = Fbm(p, 13);
    float3 col = float3(a, 0.6 * a + 0.4 * b, 0.5 * c + 0.5 * b);
    float st = smoothstep(-0.2, 0.2, sin(dot(p, float2(0.021, 0.013)) * 6.2831853));
    return lerp(col, col * 0.5 + 0.45, st * 0.6);
}
float3 Obj(float2 q) {
    float ch = smoothstep(-0.3, 0.3, sin(q.x * 3.14159 / 12) * sin(q.y * 3.14159 / 12));
    float n = Fbm(q + 5000, 31);
    return lerp(float3(0.9, 0.35, 0.2), float3(0.15, 0.3, 0.85), ch) * (0.5 + 0.7 * n);
}
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float2 p = float2(id.xy) + 0.5;
    float2 bp = center + (p - center) / bgScale - bgOff;
    float3 c = Bg(bp);
    if (hasObj) {
        float d = length(p - objC);
        float a = saturate(objR - d + 0.5);
        if (a > 0) c = lerp(c, Obj(p - objC), a);
    }
    float acc = 0;
    for (uint i = 0; i < heavy; i++) acc = sin(acc + p.x * 1e-3 + i);
    c += acc * 1e-9;
    O[id.xy] = float4(saturate(c), 1);
}
)";

struct Scene {
    float bgOffX = 0, bgOffY = 0, bgScale = 1;
    bool obj = false;
    float objX = 0, objY = 0, objR = 0;
};
static Scene LerpScene(const Scene& a, const Scene& b, float t) {
    Scene s = a;
    s.bgOffX = a.bgOffX + (b.bgOffX - a.bgOffX) * t;
    s.bgOffY = a.bgOffY + (b.bgOffY - a.bgOffY) * t;
    s.bgScale = a.bgScale + (b.bgScale - a.bgScale) * t;
    s.objX = a.objX + (b.objX - a.objX) * t;
    s.objY = a.objY + (b.objY - a.objY) * t;
    return s;
}

struct Case { const char* name; Scene (*at)(int n); };
static const Case kCases[] = {
    {"static", [](int) { return Scene{}; }},
    {"pan (7.3,-3.6)", [](int n) { Scene s; s.bgOffX = 7.3f * n; s.bgOffY = -3.6f * n; return s; }},
    {"pan (41.5,17.2)", [](int n) { Scene s; s.bgOffX = 41.5f * n; s.bgOffY = 17.2f * n; return s; }},
    {"object (23,9) / static bg", [](int n) { Scene s; s.obj = true; s.objR = 150; s.objX = 700 + 23.f * n; s.objY = 540 + 9.f * n; return s; }},
    {"object (18,-6) / pan (-9,2)", [](int n) { Scene s; s.bgOffX = -9.f * n; s.bgOffY = 2.f * n; s.obj = true; s.objR = 120; s.objX = 900 + 18.f * n; s.objY = 560 - 6.f * n; return s; }},
    {"zoom 1.5%/frame", [](int n) { Scene s; s.bgScale = 1.f + 0.015f * n; return s; }},
};
// Non-periodic, alternating motion (used for the ordering check): stale flow or stale luma gives wrong motion.
static Scene SyncScene(int n) {
    Scene s;
    s.bgOffX = 6.f * n + ((n & 1) ? 8.f : 0.f);
    s.bgOffY = 2.f * n + ((n & 1) ? -5.f : 0.f);
    s.obj = true; s.objR = 140;
    s.objX = 900 + 15.f * n + ((n & 1) ? -20.f : 0.f);
    s.objY = 500 + ((n & 1) ? 12.f : 0.f);
    return s;
}

// ------------------------------------------------------------------------------------------ helpers
struct Ctx {
    Device dev;
    ComPtr<ID3D11ComputeShader> synth;
    ComPtr<ID3D11Buffer> synthCb;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> evt;
    std::vector<uint8_t> gtPix;
};

static void Render(Ctx& c, const Device::Tex& dst, const Scene& s, UINT heavy = 0) {
    struct {
        UINT W, H, heavy, hasObj;
        float bgOff[2]; float bgScale; float pad0;
        float center[2]; float objC[2];
        float objR; float pad1[3];
    } cb{W, H, heavy, s.obj ? 1u : 0u, {s.bgOffX, s.bgOffY}, s.bgScale, 0, {W * 0.5f, H * 0.5f}, {s.objX, s.objY}, s.objR, {}};
    c.dev.UpdateCB(c.synthCb.Get(), &cb, sizeof(cb));
    auto* ctx = c.dev.ctx();
    ctx->CSSetShader(c.synth.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, c.synthCb.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 1, dst.uav.GetAddressOf(), nullptr);
    ctx->Dispatch((W + 15) / 16, (H + 7) / 8, 1);
    c.dev.ClearCS();
}

static void WaitIdle(Ctx& c) {
    auto* ctx = c.dev.ctx();
    ctx->End(c.evt.Get());
    ctx->Flush();
    BOOL done = FALSE;
    while (ctx->GetData(c.evt.Get(), &done, sizeof(done), 0) != S_OK) YieldProcessor();
}

static void Readback(Ctx& c, const Device::Tex& t, std::vector<uint8_t>& px) {
    auto* ctx = c.dev.ctx();
    ctx->CopyResource(c.staging.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    HR_CHECK(ctx->Map(c.staging.Get(), 0, D3D11_MAP_READ, 0, &m));
    px.resize((size_t)W * H * 4);
    for (UINT y = 0; y < H; y++) memcpy(&px[(size_t)y * W * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, W * 4);
    ctx->Unmap(c.staging.Get(), 0);
}

struct Sse { double full = 0, inner = 0; uint64_t nFull = 0, nInner = 0; };
static void AddSse(Sse& s, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int margin = 48) {
    for (UINT y = 0; y < H; y++) {
        const bool iy = (int)y >= margin && (int)y < (int)H - margin;
        const uint8_t* pa = &a[(size_t)y * W * 4];
        const uint8_t* pb = &b[(size_t)y * W * 4];
        double row = 0, rowIn = 0;
        for (UINT x = 0; x < W; x++) {
            double e = 0;
            for (int k = 0; k < 3; k++) { double d = (double)pa[x * 4 + k] - (double)pb[x * 4 + k]; e += d * d; }
            row += e;
            if (iy && (int)x >= margin && (int)x < (int)W - margin) rowIn += e;
        }
        s.full += row; s.inner += rowIn;
    }
    s.nFull += (uint64_t)W * H * 3;
    s.nInner += (uint64_t)(W - 2 * margin) * (H - 2 * margin) * 3;
}
static double Psnr(double sse, uint64_t n) {
    double mse = sse / (double)n;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Minimal PNG writer (stored deflate blocks) for visual inspection.
static uint32_t Crc(const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t tab[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1; tab[i] = v; } init = true; }
    for (size_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void WritePng(const std::string& path, const std::vector<uint8_t>& rgba, UINT pw = W, UINT ph = H) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(pw * 3 + 1) * ph);
    for (UINT y = 0; y < ph; y++) { raw.push_back(0); for (UINT x = 0; x < pw; x++) for (int k = 0; k < 3; k++) raw.push_back(rgba[((size_t)y * pw + x) * 4 + k]); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t v : raw) { s1 = (s1 + v) % 65521; s2 = (s2 + s1) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t len = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + len == raw.size() ? 1 : 0);
        z.push_back((uint8_t)(len & 0xff)); z.push_back((uint8_t)(len >> 8));
        z.push_back((uint8_t)(~len & 0xff)); z.push_back((uint8_t)((~len >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
    }
    uint32_t ad = (s2 << 16) | s1;
    for (int i = 3; i >= 0; i--) z.push_back((uint8_t)(ad >> (i * 8)));
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto be32 = [&](uint32_t v) { uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; fwrite(b, 1, 4, f); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        be32((uint32_t)d.size());
        std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
        fwrite(td.data(), 1, td.size(), f);
        be32(Crc(td.data(), td.size()) ^ 0xffffffffu);
    };
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(pw >> 24), (uint8_t)(pw >> 16), (uint8_t)(pw >> 8), (uint8_t)pw,
                                 (uint8_t)(ph >> 24), (uint8_t)(ph >> 16), (uint8_t)(ph >> 8), (uint8_t)ph, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
    fclose(f);
}

// Side-by-side crop [ground truth | output | |error| x4], 2x nearest upscale, for visual inspection.
static void WriteCompare(const std::string& path, const std::vector<uint8_t>& gt, const std::vector<uint8_t>& out, int cx, int cy) {
    const int cw = 320, ch = 240, sc = 2;
    const int x0 = std::clamp(cx - cw / 2, 0, (int)W - cw), y0 = std::clamp(cy - ch / 2, 0, (int)H - ch);
    const UINT pw = cw * sc * 3, ph = ch * sc;
    std::vector<uint8_t> img((size_t)pw * ph * 4);
    for (UINT y = 0; y < ph; y++)
        for (UINT x = 0; x < pw; x++) {
            int panel = (int)x / (cw * sc), sx = x0 + ((int)x % (cw * sc)) / sc, sy = y0 + (int)y / sc;
            size_t si = ((size_t)sy * W + sx) * 4, di = ((size_t)y * pw + x) * 4;
            for (int k = 0; k < 3; k++) {
                int v = panel == 0 ? gt[si + k] : panel == 1 ? out[si + k] : std::min(255, 4 * std::abs((int)gt[si + k] - (int)out[si + k]));
                img[di + k] = (uint8_t)v;
            }
        }
    WritePng(path, img, pw, ph);
}

// GPU timestamp range helper.
struct TsRange {
    ComPtr<ID3D11Query> dj, ts[4];
    void Init(ID3D11Device* d) {
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HR_CHECK(d->CreateQuery(&q, &dj));
        q.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& t : ts) HR_CHECK(d->CreateQuery(&q, &t));
    }
    // Returns ms between marks i and j (after the GPU has finished).
    bool Get(ID3D11DeviceContext* ctx, std::vector<double>& msBetween, int marks) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
        while (ctx->GetData(dj.Get(), &d, sizeof(d), 0) != S_OK) YieldProcessor();
        if (d.Disjoint) return false;
        UINT64 v[4] = {};
        for (int i = 0; i < marks; i++) while (ctx->GetData(ts[i].Get(), &v[i], 8, 0) != S_OK) YieldProcessor();
        msBetween.clear();
        for (int i = 1; i < marks; i++) msBetween.push_back((double)(v[i] - v[i - 1]) * 1000.0 / (double)d.Frequency);
        return true;
    }
};

static double Median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))];
}


// ------------------------------------------------------------------------------------------ raw ordering probe
// Drives NVOF directly (same configuration as the interpolator) to find out which D3D11 <-> OFA
// dependencies the driver orders by itself. Input: R8 noise shifted horizontally by a known,
// non-periodic amount per frame; output consumed by a compute shader that averages the flow.
static const char* kProbeCS = R"(
cbuffer CB : register(b0) { uint W, H, heavy, pad; float shift; float3 pad1; };
RWTexture2D<unorm float> L : register(u0);
uint Hash(int2 p) { uint h = (uint)p.x * 374761393u + (uint)p.y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
float Rnd(int2 p) { return (float)(Hash(p) & 0xffffff) / 16777215.0; }
float VN(float2 p) { float2 i = floor(p); float2 f = p - i; float2 u = f * f * (3 - 2 * f); int2 ii = int2(i);
  return lerp(lerp(Rnd(ii), Rnd(ii + int2(1, 0)), u.x), lerp(Rnd(ii + int2(0, 1)), Rnd(ii + int2(1, 1)), u.x), u.y); }
[numthreads(16, 8, 1)]
void Pattern(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float2 p = float2(id.xy) + 0.5 - float2(shift, 0);
    float v = 0.5 * VN(p / 8) + 0.3 * VN(p / 4) + 0.2 * VN(p / 16);
    float acc = 0;
    for (uint i = 0; i < heavy; i++) acc = sin(acc + p.x * 1e-3 + i);
    L[id.xy] = v + acc * 1e-9;
}
Texture2D<int2> Fl : register(t0);
RWByteAddressBuffer Acc : register(u1);
[numthreads(16, 8, 1)]
void Mean(uint3 id : SV_DispatchThreadID) {
    uint fw = W / 4, fh = H / 4;
    if (id.x < 16 || id.y < 16 || id.x >= fw - 16 || id.y >= fh - 16) return;
    int2 f = Fl[id.xy];
    Acc.InterlockedAdd(0, (uint)f.x);
    Acc.InterlockedAdd(4, (uint)f.y);
    Acc.InterlockedAdd(8, 1);
}
)";

static double Median(std::vector<double> v);
static void OrderingProbe(Ctx& c) {
    typedef NV_OF_STATUS(NVOFAPI * PFN_C)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
    HMODULE m = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { printf("probe: no nvofapi64.dll\n"); return; }
    NV_OF_D3D11_API_FUNCTION_LIST F{};
    if (((PFN_C)GetProcAddress(m, "NvOFAPICreateInstanceD3D11"))(NV_OF_API_VERSION, &F)) { printf("probe: create instance failed\n"); return; }
    auto* dev = c.dev.dev();
    auto* ctx = c.dev.ctx();
    NvOFHandle h = nullptr;
    F.nvCreateOpticalFlowD3D11(dev, ctx, &h);
    NV_OF_INIT_PARAMS ip{};
    ip.width = W; ip.height = H; ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4; ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    ip.mode = NV_OF_MODE_OPTICALFLOW; ip.perfLevel = NV_OF_PERF_LEVEL_MEDIUM; ip.enableOutputCost = NV_OF_TRUE;
    ip.predDirection = NV_OF_PRED_DIRECTION_BOTH; ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    if (F.nvOFInit(h, &ip)) { printf("probe: init failed\n"); return; }
    Device::Tex in[2] = {c.dev.CreateTex(W, H, DXGI_FORMAT_R8_UNORM, true, false), c.dev.CreateTex(W, H, DXGI_FORMAT_R8_UNORM, true, false)};
    Device::Tex fo = c.dev.CreateTex(W / 4, H / 4, DXGI_FORMAT_R16G16_SINT, false, false);
    Device::Tex bo = c.dev.CreateTex(W / 4, H / 4, DXGI_FORMAT_R16G16_SINT, false, false);
    Device::Tex co = c.dev.CreateTex(W / 4, H / 4, DXGI_FORMAT_R8_UINT, false, false);
    Device::Tex cb2 = c.dev.CreateTex(W / 4, H / 4, DXGI_FORMAT_R8_UINT, false, false);
    Device::Tex foCopy = c.dev.CreateTex(W / 4, H / 4, DXGI_FORMAT_R16G16_SINT, false, false);
    NvOFGPUBufferHandle hin[2], hfo, hbo, hco, hcb;
    F.nvOFRegisterResourceD3D11(h, in[0].tex.Get(), &hin[0]); F.nvOFRegisterResourceD3D11(h, in[1].tex.Get(), &hin[1]);
    F.nvOFRegisterResourceD3D11(h, fo.tex.Get(), &hfo); F.nvOFRegisterResourceD3D11(h, bo.tex.Get(), &hbo);
    F.nvOFRegisterResourceD3D11(h, co.tex.Get(), &hco); F.nvOFRegisterResourceD3D11(h, cb2.tex.Get(), &hcb);
    auto csPat = c.dev.CompileCS(kProbeCS, "Pattern", "probe_pattern");
    auto csMean = c.dev.CompileCS(kProbeCS, "Mean", "probe_mean");
    auto cb = c.dev.CreateConstantBuffer(32);
    ComPtr<ID3D11Buffer> acc, accStage;
    ComPtr<ID3D11UnorderedAccessView> accUav;
    {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        HR_CHECK(dev->CreateBuffer(&bd, nullptr, &acc));
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        HR_CHECK(dev->CreateUnorderedAccessView(acc.Get(), &ud, &accUav));
        bd.BindFlags = 0; bd.MiscFlags = 0; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR_CHECK(dev->CreateBuffer(&bd, nullptr, &accStage));
    }
    ComPtr<ID3D11Query> ev;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    HR_CHECK(dev->CreateQuery(&qd, &ev));

    auto pattern = [&](const Device::Tex& t, float shift, UINT heavy) {
        struct { UINT W, H, heavy, pad; float shift, p1, p2, p3; } d{W, H, heavy, 0, shift, 0, 0, 0};
        c.dev.UpdateCB(cb.Get(), &d, sizeof(d));
        ctx->CSSetShader(csPat.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, t.uav.GetAddressOf(), nullptr);
        ctx->Dispatch((W + 15) / 16, (H + 7) / 8, 1);
        c.dev.ClearCS();
    };
    auto meanFlow = [&](const Device::Tex& flow, double& mx) {
        const UINT zero[4] = {0, 0, 0, 0};
        ctx->ClearUnorderedAccessViewUint(accUav.Get(), zero);
        struct { UINT W, H, heavy, pad; float shift, p1, p2, p3; } d{W, H, 0, 0, 0, 0, 0, 0};
        c.dev.UpdateCB(cb.Get(), &d, sizeof(d));
        ctx->CSSetShader(csMean.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetShaderResources(0, 1, flow.srv.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(1, 1, accUav.GetAddressOf(), nullptr);
        ctx->Dispatch((W / 4 + 15) / 16, (H / 4 + 7) / 8, 1);
        c.dev.ClearCS();
        ctx->CopyResource(accStage.Get(), acc.Get());
        D3D11_MAPPED_SUBRESOURCE mm{};
        HR_CHECK(ctx->Map(accStage.Get(), 0, D3D11_MAP_READ, 0, &mm));
        const int32_t* v = (const int32_t*)mm.pData;
        mx = v[2] ? (double)v[0] / v[2] / 32.0 : 0.0;
        ctx->Unmap(accStage.Get(), 0);
    };
    auto waitEvent = [&] {
        ctx->End(ev.Get()); ctx->Flush();
        BOOL done = FALSE;
        while (ctx->GetData(ev.Get(), &done, sizeof(done), 0) != S_OK) YieldProcessor();
    };

    // Shifts with varying consecutive differences.
    float shifts[64];
    uint32_t lcg = 12345;
    shifts[0] = 0;
    for (int i = 1; i < 64; i++) { lcg = lcg * 1664525u + 1013904223u; shifts[i] = shifts[i - 1] + (float)((int)(lcg >> 28) - 8) * 1.5f + 0.5f; }

    struct Variant { const char* name; UINT heavyIn; int sync; };
    // sync: 0 none, 1 Flush after execute, 2 event-wait after execute, 3 CopyResource(flow) after execute,
    //       4 event-wait BEFORE execute + Sleep after, 5 Sleep(20ms) after execute, 6 Flush before execute + Sleep after
    const Variant vars[] = {
        {"idle GPU, no sync", 0, 0},
        {"idle GPU, Flush after Execute", 0, 1},
        {"idle GPU, event wait after Execute", 0, 2},
        {"idle GPU, CopyResource(flow) then consume copy", 0, 3},
        {"idle GPU, Sleep 20ms after Execute", 0, 5},
        {"heavy input render, Sleep 20ms after Execute", 3000, 5},
        {"heavy input render, Flush before Execute + Sleep", 3000, 6},
        {"heavy input render, event wait before Execute + Sleep", 3000, 4},
        {"heavy input render, event wait after Execute", 3000, 2},
    };
    for (const auto& v : vars) {
        int ok = 0, stale = 0, other = 0, n = 0;
        double worst = 0;
        pattern(in[0], shifts[0], 0);
        waitEvent();
        for (int i = 1; i < 24; i++) {
            const Device::Tex& cur = in[i & 1];
            pattern(cur, shifts[i], v.heavyIn);
            if (v.sync == 4) waitEvent();
            if (v.sync == 6) ctx->Flush();
            NV_OF_EXECUTE_INPUT_PARAMS ei{};
            ei.inputFrame = hin[(i - 1) & 1]; ei.referenceFrame = hin[i & 1]; ei.disableTemporalHints = NV_OF_TRUE;
            NV_OF_EXECUTE_OUTPUT_PARAMS eo{};
            eo.outputBuffer = hfo; eo.outputCostBuffer = hco; eo.bwdOutputBuffer = hbo; eo.bwdOutputCostBuffer = hcb;
            if (F.nvOFExecute(h, &ei, &eo)) { printf("probe: execute failed\n"); break; }
            const Device::Tex* consume = &fo;
            if (v.sync == 1) ctx->Flush();
            if (v.sync == 2) waitEvent();
            if (v.sync == 3) { ctx->CopyResource(foCopy.tex.Get(), fo.tex.Get()); consume = &foCopy; }
            if (v.sync >= 4) { ctx->Flush(); Sleep(20); }
            double mx = 0;
            meanFlow(*consume, mx);
            double expect = shifts[i] - shifts[i - 1];
            double prevExpect = i >= 2 ? shifts[i - 1] - shifts[i - 2] : 1e9;
            double inStale = i >= 2 ? shifts[i - 2] - shifts[i - 1] : 1e9;   // reference slot still holding frame i-2
            if (i < 4) continue;   // warm-up
            n++;
            if (std::fabs(mx - expect) < 0.5) ok++;
            else if (std::fabs(mx - prevExpect) < 0.5 || std::fabs(mx - inStale) < 0.5) stale++;
            else other++;
            worst = std::max(worst, std::fabs(mx - expect));
        }
        printf("  %-56s correct %2d/%d, stale %2d, other %2d (max err %.1f px)\n", v.name, ok, n, stale, other, worst);
    }
    // Poll: how long after nvOFExecute (+Flush) until a CPU readback (CopyResource -> staging -> Map) sees the result?
    {
        D3D11_TEXTURE2D_DESC td{};
        fo.tex->GetDesc(&td);
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        HR_CHECK(dev->CreateTexture2D(&td, nullptr, &st));
        auto cpuMean = [&]() {
            ctx->CopyResource(st.Get(), fo.tex.Get());
            D3D11_MAPPED_SUBRESOURCE mm{};
            HR_CHECK(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mm));
            double sx = 0; int cnt = 0;
            for (UINT y = 16; y < td.Height - 16; y++) {
                const int16_t* row = (const int16_t*)((const uint8_t*)mm.pData + (size_t)y * mm.RowPitch);
                for (UINT x = 16; x < td.Width - 16; x++) { sx += row[x * 2]; cnt++; }
            }
            ctx->Unmap(st.Get(), 0);
            return sx / cnt / 32.0;
        };
        pattern(in[0], shifts[0], 0);
        waitEvent();
        std::vector<double> lat;
        int firstOk = 0, n = 0;
        for (int i = 1; i < 24; i++) {
            pattern(in[i & 1], shifts[i], 0);
            waitEvent();
            NV_OF_EXECUTE_INPUT_PARAMS ei{};
            ei.inputFrame = hin[(i - 1) & 1]; ei.referenceFrame = hin[i & 1]; ei.disableTemporalHints = NV_OF_TRUE;
            NV_OF_EXECUTE_OUTPUT_PARAMS eo{};
            eo.outputBuffer = hfo; eo.outputCostBuffer = hco; eo.bwdOutputBuffer = hbo; eo.bwdOutputCostBuffer = hcb;
            int64_t t0 = Qpc::Now();
            F.nvOFExecute(h, &ei, &eo);
            int64_t t1 = Qpc::Now();
            ctx->Flush();
            double expect = shifts[i] - shifts[i - 1];
            bool first = true;
            for (;;) {
                double mx = cpuMean();
                if (std::fabs(mx - expect) < 0.5) { if (i >= 4) { lat.push_back(Qpc::ToMs(Qpc::Now() - t0)); firstOk += first; n++; } break; }
                first = false;
                if (Qpc::ToMs(Qpc::Now() - t0) > 100) { if (i >= 4) n++; break; }
            }
            if (i == 5) printf("  (nvOFExecute CPU call %.3f ms)\n", Qpc::ToMs(t1 - t0));
        }
        printf("  CPU readback poll after Execute: first Map correct %d/%d, time-to-correct median %.3f ms max %.3f ms\n",
               firstOk, n, Median(lat), lat.empty() ? -1.0 : *std::max_element(lat.begin(), lat.end()));
    }
    for (auto x : {hin[0], hin[1], hfo, hbo, hco, hcb}) F.nvOFUnregisterResourceD3D11(x);
    F.nvOFDestroy(h);
}


// Crash reporter: prints the faulting module/offset and a symbolized stack (debug aid).
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    printf("CRASH code %08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)sf.AddrPC.Offset, &hm)) GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = strrchr(mod, '\\');
        printf("  #%d %s+0x%llx %s\n", i, base ? base + 1 : mod, (unsigned long long)(sf.AddrPC.Offset - (DWORD64)hm),
               SymFromAddr(proc, sf.AddrPC.Offset, &disp, si) ? si->Name : "");
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
// ------------------------------------------------------------------------------------------ main
int main(int argc, char** argv) {
    std::string dumpDir;
    int frames = 8;
    bool quick = false;
    bool probeOnly = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probeOnly = true;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quick")) quick = true;
    }
    SetUnhandledExceptionFilter(CrashFilter);
    SetLogLevel(LogLevel::Info);
    try {
        Ctx c;
        if (!c.dev.Create(false)) return 1;
        c.synth = c.dev.CompileCS(kSynthCS, "main", "synth");
        c.synthCb = c.dev.CreateConstantBuffer(64);
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = W; sd.Height = H; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR_CHECK(c.dev.dev()->CreateTexture2D(&sd, nullptr, &c.staging));
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        HR_CHECK(c.dev.dev()->CreateQuery(&qd, &c.evt));

        printf("\n=== raw NVOF ordering probe (grid4 MEDIUM BOTH, flow consumed by a D3D11 compute shader) ===\n");
        OrderingProbe(c);
        if (probeOnly) return 0;
        SourceFrame fr[3];
        for (auto& f : fr) f.rgb = c.dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
        Device::Tex out = c.dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
        Device::Tex gt = c.dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
        Device::Tex scratch = c.dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
        uint64_t gen = 0;

        struct Named { std::string name; std::unique_ptr<Interpolator> ip; };
        std::vector<Named> ips;
        ips.push_back({"blend", CreateBlendInterpolator()});
        for (int q = 0; q <= 2; q++) ips.push_back({"nvof q" + std::to_string(q), CreateFlowInterpolator(q)});
        for (auto& n : ips) if (!n.ip || !n.ip->Init(c.dev, W, H)) { printf("init failed: %s\n", n.name.c_str()); return 1; }

        const float ts[3] = {0.25f, 0.5f, 0.75f};
        std::vector<uint8_t> outPix, gtPix;

        // -------------------------------------------------------------- quality
        printf("\n=== PSNR (dB) vs analytic intermediate frame, 1920x1080, %d frames/case (full frame | interior w/o 48px border) ===\n", frames);
        printf("%-30s %5s", "case", "t");
        for (auto& n : ips) printf(" | %-15s", n.name.c_str());
        printf("\n");
        for (const Case& cs : kCases) {
            // sse[interp][t]
            std::vector<std::array<Sse, 3>> sse(ips.size());
            for (size_t ii = 0; ii < ips.size(); ii++) {
                Interpolator* ip = ips[ii].ip.get();
                for (int n = 0; n < frames; n++) {
                    SourceFrame& B = fr[n % 3];
                    Render(c, B.rgb, cs.at(n));
                    B.n = n; B.generation = ++gen;
                    if (n == 0) continue;
                    SourceFrame& A = fr[(n - 1) % 3];
                    ip->Prepare(A, B);
                    for (int k = 0; k < 3; k++) {
                        ip->Interpolate(A, B, ts[k], out);
                        Render(c, gt, LerpScene(cs.at(n - 1), cs.at(n), ts[k]));
                        Readback(c, out, outPix);
                        Readback(c, gt, gtPix);
                        AddSse(sse[ii][k], outPix, gtPix);
                        if (!dumpDir.empty() && n == frames - 1 && k == 1) {
                            std::string base = dumpDir + "/" + std::to_string(&cs - kCases) + "_";
                            Scene sc = LerpScene(cs.at(n - 1), cs.at(n), ts[k]);
                            int cx = sc.obj ? (int)(sc.objX + sc.objR * 0.7f) : (int)W / 2, cy = sc.obj ? (int)sc.objY : (int)H / 2;
                            WriteCompare(base + ips[ii].name.substr(0, 4) + ips[ii].name.substr(ips[ii].name.size() - 1) + ".png", gtPix, outPix, cx, cy);
                        }
                    }
                }
            }
            for (int k = 0; k < 3; k++) {
                printf("%-30s %5.2f", k == 0 ? cs.name : "", ts[k]);
                for (size_t ii = 0; ii < ips.size(); ii++)
                    printf(" | %6.2f | %6.2f", Psnr(sse[ii][k].full, sse[ii][k].nFull), Psnr(sse[ii][k].inner, sse[ii][k].nInner));
                printf("\n");
            }
        }
        if (quick) return 0;

        // -------------------------------------------------------------- ordering check
        printf("\n=== D3D11 <-> OFA ordering check (alternating non-periodic motion, heavy GPU render before Prepare) ===\n");
        {
            // Calibrate a heavy render (~4 ms).
            TsRange tr; tr.Init(c.dev.dev());
            UINT heavy = 2000;
            {
                WaitIdle(c);
                auto* ctx = c.dev.ctx();
                ctx->Begin(tr.dj.Get()); ctx->End(tr.ts[0].Get());
                Render(c, scratch, SyncScene(0), heavy);
                ctx->End(tr.ts[1].Get()); ctx->End(tr.dj.Get());
                std::vector<double> ms; tr.Get(ctx, ms, 2);
                printf("heavy render: %.2f ms GPU\n", ms.empty() ? -1.0 : ms[0]);
            }
            for (int q = 1; q < (int)ips.size(); q++) {
                Interpolator* ip = ips[q].ip.get();
                for (int mode = 0; mode < 2; mode++) {
                    Sse s;
                    for (int n = 0; n < 16; n++) {
                        SourceFrame& B = fr[n % 3];
                        Render(c, B.rgb, SyncScene(n), heavy);
                        B.n = n; B.generation = ++gen;
                        if (n == 0) continue;
                        SourceFrame& A = fr[(n - 1) % 3];
                        if (mode == 1) WaitIdle(c);
                        ip->Prepare(A, B);
                        if (mode == 1) WaitIdle(c);
                        ip->Interpolate(A, B, 0.5f, out);
                        Render(c, gt, LerpScene(SyncScene(n - 1), SyncScene(n), 0.5f));
                        Readback(c, out, outPix);
                        Readback(c, gt, gtPix);
                        AddSse(s, outPix, gtPix);
                    }
                    printf("%-8s %-36s PSNR %.2f dB (interior %.2f)\n", ips[q].name.c_str(),
                           mode == 0 ? "no CPU sync between stages:" : "full CPU wait between stages:",
                           Psnr(s.full, s.nFull), Psnr(s.inner, s.nInner));
                }
            }
        }

        // -------------------------------------------------------------- timing
        printf("\n=== timing, 1920x1080 (median / p95 over %d pairs) ===\n", 60);
        {
            auto* ctx = c.dev.ctx();
            TsRange tr; tr.Init(c.dev.dev());
            const Case& cs = kCases[4];
            for (size_t ii = 0; ii < ips.size(); ii++) {
                Interpolator* ip = ips[ii].ip.get();
                std::vector<double> prepCpu, prepLat, prepTs, indepTs, interpCpu, interpTs, interpLat, e2e, e2eP, e2eI, e2eW;
                for (int n = 0; n < 61; n++) {
                    SourceFrame& B = fr[n % 3];
                    Render(c, B.rgb, cs.at(n));
                    B.n = n; B.generation = ++gen;
                    if (n == 0) continue;
                    SourceFrame& A = fr[(n - 1) % 3];
                    WaitIdle(c);
                    // Prepare: event latency (submit -> GPU incl. OFA done), timestamps, and whether
                    // independent graphics-queue work submitted right after it is delayed by the OFA work.
                    int64_t t0 = Qpc::Now();
                    ctx->Begin(tr.dj.Get()); ctx->End(tr.ts[0].Get());
                    ip->Prepare(A, B);
                    int64_t t1 = Qpc::Now();
                    ctx->End(tr.ts[1].Get());
                    Render(c, scratch, Scene{});     // independent work (~synth cost)
                    ctx->End(tr.ts[2].Get()); ctx->End(tr.dj.Get());
                    WaitIdle(c);
                    int64_t t2 = Qpc::Now();
                    std::vector<double> ms;
                    if (tr.Get(ctx, ms, 3)) { prepTs.push_back(ms[0]); indepTs.push_back(ms[1]); }
                    prepCpu.push_back(Qpc::ToMs(t1 - t0));
                    // Prepare-only latency (separately, without the independent work)
                    // is measured on the second half of the run below via e2e.
                    (void)t2;
                    // Interpolate x3 (flow already computed): GPU timestamps. The first Interpolate of a pair
                    // contains the GPU-side fence wait for the OFA engine, so do one unmeasured call first.
                    ip->Interpolate(A, B, 0.1f, out);
                    for (int k = 0; k < 3; k++) {
                        WaitIdle(c);
                        int64_t a0 = Qpc::Now();
                        ctx->Begin(tr.dj.Get()); ctx->End(tr.ts[0].Get());
                        ip->Interpolate(A, B, ts[k], out);
                        int64_t a1 = Qpc::Now();
                        ctx->End(tr.ts[1].Get()); ctx->End(tr.dj.Get());
                        WaitIdle(c);
                        int64_t a2 = Qpc::Now();
                        if (tr.Get(ctx, ms, 2)) interpTs.push_back(ms[0]);
                        interpCpu.push_back(Qpc::ToMs(a1 - a0));
                        interpLat.push_back(Qpc::ToMs(a2 - a0));
                    }
                }
                // Prepare latency alone and frame-arrival -> first interpolated frame ready latency.
                for (int n = 0; n < 61; n++) {
                    SourceFrame& B = fr[n % 3];
                    Render(c, B.rgb, cs.at(n));
                    B.n = n; B.generation = ++gen;
                    if (n == 0) continue;
                    SourceFrame& A = fr[(n - 1) % 3];
                    WaitIdle(c);
                    int64_t t0 = Qpc::Now();
                    ip->Prepare(A, B);
                    WaitIdle(c);
                    int64_t t1 = Qpc::Now();
                    prepLat.push_back(Qpc::ToMs(t1 - t0));
                    n++;
                    if (n >= 61) break;
                    SourceFrame& B2 = fr[n % 3];
                    Render(c, B2.rgb, cs.at(n));
                    B2.n = n; B2.generation = ++gen;
                    WaitIdle(c);
                    t0 = Qpc::Now();
                    ip->Prepare(B, B2);
                    int64_t tp = Qpc::Now();
                    ip->Interpolate(B, B2, 0.5f, out);
                    int64_t ti = Qpc::Now();
                    WaitIdle(c);
                    t1 = Qpc::Now();
                    e2e.push_back(Qpc::ToMs(t1 - t0));
                    e2eP.push_back(Qpc::ToMs(tp - t0)); e2eI.push_back(Qpc::ToMs(ti - tp)); e2eW.push_back(Qpc::ToMs(t1 - ti));
                }
                printf("%-8s Prepare: CPU %.3f ms, GPU-ts %.3f ms, submit->done %.3f/%.3f ms | indep. work after Prepare %.3f ms | "
                       "Interpolate: GPU-ts %.3f/%.3f ms, CPU %.3f ms, submit->done %.3f ms | Prepare+Interpolate submit->done %.3f/%.3f ms\n",
                       ips[ii].name.c_str(), Median(prepCpu), Median(prepTs), Median(prepLat), Pct(prepLat, 0.95), Median(indepTs),
                       Median(interpTs), Pct(interpTs, 0.95), Median(interpCpu), Median(interpLat), Median(e2e), Pct(e2e, 0.95));
                printf("         e2e breakdown: Prepare call %.3f, Interpolate call (incl. CPU fence wait) %.3f, GPU drain %.3f ms\n",
                       Median(e2eP), Median(e2eI), Median(e2eW));
            }
            // Reference: cost of the independent work alone.
            std::vector<double> ref;
            for (int i = 0; i < 20; i++) {
                WaitIdle(c);
                ctx->Begin(tr.dj.Get()); ctx->End(tr.ts[0].Get());
                Render(c, scratch, Scene{});
                ctx->End(tr.ts[1].Get()); ctx->End(tr.dj.Get());
                std::vector<double> ms;
                if (tr.Get(ctx, ms, 2)) ref.push_back(ms[0]);
            }
            printf("reference: independent work alone %.3f ms\n", Median(ref));
        }
    } catch (const std::exception& e) {
        printf("exception: %s\n", e.what());
        return 1;
    }
    return 0;
}
