// Optical-flow frame interpolator built on the NVIDIA Optical Flow (NVOF) hardware engine.
//
// Prepare(a, b)  (once per consecutive pair, all GPU, no readback):
//   1. luma(a), luma(b) -> R8_UNORM (cached per frame, so frame b is reused as the next pair's a)
//   2. nvOFExecute(input=a, reference=b, PRED_DIRECTION_BOTH, cost) -> block flows F01 (a->b), F10 (b->a)
//      (D3D12 NVOF API on shared resources, fence-synchronized with the D3D11 queue; see class comment)
//   Nothing on the D3D11 queue consumes the flow in Prepare, so the OFA engine runs asynchronously
//   and the graphics queue is not blocked until the first Interpolate of this pair (GPU-side fence wait).
//
// Interpolate(a, b, t)  (cheap, any number of times per pair):
//   4. "tflow" CS at flow-grid resolution: for every cell x at time t search for the motion u of the
//      surface visible there. Candidates come from the flows around x and from fixed-point iterations
//      u <- F01(x - t u), u <- -F10(x + (1-t) u) (reverse projection). Each candidate is scored in three
//      visibility modes: visible in both frames (geometric consistency with F01 and F10 + photometric patch
//      match between a(x - t u) and b(x + (1-t) u)), visible only in a (being covered), visible only in b
//      (being uncovered). Best (u, mode, confidence) is stored per cell.
//   5. "compose" CS at full resolution: for each pixel take the 2x2 neighbouring cells' (u, mode), backward-warp
//      a and b with each, weight the candidates by bilinear position * photometric agreement (snaps motion
//      boundaries to pixel accuracy), blend the two warps with (1-t)/t * visibility, and fade towards a plain
//      cross-fade where confidence is low.
#include "interpolator.h"
#include "nvof_d3d12.h"
#include <vector>
#include <cmath>

namespace camss {

// ----------------------------------------------------------------------------------------------- shaders
static const char* kNvofCS = R"(
cbuffer CB : register(b0) {
    uint W, H, FW, FH;          // source size, flow grid size
    float G, invG, t, pad0;     // grid cell size in pixels
    float invW, invH, pad1, pad2;
    float alpha, pOcc, pEdge, pad3;         // tflow scoring
    float sGood, sBad, invSig2, pwOcc;      // confidence / compose weighting
};

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---------------------------------------------------------------- luma (R8 input for NVOF)
Texture2D<float4> LumaSrc : register(t0);
RWTexture2D<unorm float> LumaDst : register(u0);
[numthreads(16, 8, 1)]
void LumaCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    LumaDst[id.xy] = Luma(LumaSrc[id.xy].rgb);
}

// ---------------------------------------------------------------- tflow: motion at time t per grid cell
Texture2D<int2> F01 : register(t0);      // NVOF forward flow a->b on frame-a grid (S10.5)
Texture2D<int2> F10 : register(t1);      // NVOF backward flow b->a on frame-b grid (S10.5)
Texture2D<float4> ImA : register(t2);
Texture2D<float4> ImB : register(t3);
SamplerState LS : register(s0);
RWTexture2D<float4> TOut : register(u0);  // (u.xy, mode m in [-1,1], confidence)

float2 LoadM0(float2 p) { return float2(F01[clamp(int2(floor(p * invG)), int2(0, 0), int2(FW - 1, FH - 1))]) * (1.0 / 32.0); }
float2 LoadM1(float2 p) { return float2(F10[clamp(int2(floor(p * invG)), int2(0, 0), int2(FW - 1, FH - 1))]) * (1.0 / 32.0); }
bool Inside(float2 p) { return p.x >= -0.5 && p.y >= -0.5 && p.x <= W + 0.5 && p.y <= H + 0.5; }

float4 PatchA(float2 p) {
    float2 s = float2(invW, invH);
    return float4(Luma(ImA.SampleLevel(LS, (p + float2(-1, -1)) * s, 0).rgb),
                  Luma(ImA.SampleLevel(LS, (p + float2( 1, -1)) * s, 0).rgb),
                  Luma(ImA.SampleLevel(LS, (p + float2(-1,  1)) * s, 0).rgb),
                  Luma(ImA.SampleLevel(LS, (p + float2( 1,  1)) * s, 0).rgb));
}
float4 PatchB(float2 p) {
    float2 s = float2(invW, invH);
    return float4(Luma(ImB.SampleLevel(LS, (p + float2(-1, -1)) * s, 0).rgb),
                  Luma(ImB.SampleLevel(LS, (p + float2( 1, -1)) * s, 0).rgb),
                  Luma(ImB.SampleLevel(LS, (p + float2(-1,  1)) * s, 0).rgb),
                  Luma(ImB.SampleLevel(LS, (p + float2( 1,  1)) * s, 0).rgb));
}

struct Best { float s; float2 u; float m; };

void Eval(float2 x, float2 u, inout Best b) {
    float2 p0 = x - t * u, p1 = x + (1.0 - t) * u;
    bool in0 = Inside(p0), in1 = Inside(p1);
    float g0 = length(LoadM0(p0) - u);
    float g1 = length(LoadM1(p1) + u);
    if (in0 && in1) {
        float4 d = abs(PatchA(p0) - PatchB(p1));
        float s = g0 + g1 + alpha * 0.25 * (d.x + d.y + d.z + d.w);
        if (s < b.s) { b.s = s; b.u = u; b.m = 0; }
    }
    if (in0) {
        float s = g0 + (in1 ? pOcc : pEdge);
        if (s < b.s) { b.s = s; b.u = u; b.m = -1; }
    }
    if (in1) {
        float s = g1 + (in0 ? pOcc : pEdge);
        if (s < b.s) { b.s = s; b.u = u; b.m = 1; }
    }
}

// Candidate s plus two fixed-point (reverse projection) steps through each flow field.
void EvalChain(float2 x, float2 s, inout Best b) {
    Eval(x, s, b);
    float2 a1 = LoadM0(x - t * s);            Eval(x, a1, b);
    float2 b1 = -LoadM1(x + (1.0 - t) * s);   Eval(x, b1, b);
    float2 a2 = LoadM0(x - t * a1);           Eval(x, a2, b);
    float2 b2 = -LoadM1(x + (1.0 - t) * b1);  Eval(x, b2, b);
}

[numthreads(16, 8, 1)]
void TFlowCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= FW || id.y >= FH) return;
    float2 x = (float2(id.xy) + 0.5) * G;
    Best b; b.s = 1e30; b.u = 0; b.m = 0;
    Eval(x, float2(0, 0), b);
    const float R = 2.0 * G;
    const float2 offs[5] = { float2(0, 0), float2(-R, 0), float2(R, 0), float2(0, -R), float2(0, R) };
    [unroll] for (int k = 0; k < 5; k++) {
        EvalChain(x, LoadM0(x + offs[k]), b);
        EvalChain(x, -LoadM1(x + offs[k]), b);
    }
    float conf = saturate((sBad - b.s) / (sBad - sGood));
    TOut[id.xy] = float4(b.u, b.m, conf);
}

// ---------------------------------------------------------------- compose: full-res warp + blend
Texture2D<float4> TIn : register(t0);
Texture2D<float4> CA : register(t1);
Texture2D<float4> CBt : register(t2);
RWTexture2D<unorm float4> Out : register(u0);

bool InsideImg(float2 p) { return p.x >= -0.5 && p.y >= -0.5 && p.x <= W + 0.5 && p.y <= H + 0.5; }

[numthreads(16, 8, 1)]
void ComposeCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float2 xp = float2(id.xy) + 0.5;
    float2 c = xp * invG - 0.5;
    int2 i0 = int2(floor(c));
    float2 f = c - float2(i0);
    float2 s = float2(invW, invH);
    float3 cross = lerp(CA[id.xy].rgb, CBt[id.xy].rgb, t);
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; j++) {
        [unroll] for (int i = 0; i < 2; i++) {
            int2 ci = clamp(i0 + int2(i, j), int2(0, 0), int2(FW - 1, FH - 1));
            float4 tv = TIn[ci];
            float bw = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y);
            float2 u = tv; 
            float2 q0 = xp - t * u, q1 = xp + (1.0 - t) * u;
            float vis0 = saturate(1.0 - tv.z), vis1 = saturate(1.0 + tv.z);
            if (!InsideImg(q0)) vis0 = 0;
            if (!InsideImg(q1)) vis1 = 0;
            if (vis0 + vis1 <= 0) { vis0 = 1; vis1 = 1; }
            float3 ca = CA.SampleLevel(LS, q0 * s, 0).rgb;
            float3 cb = CBt.SampleLevel(LS, q1 * s, 0).rgb;
            float wa = (1.0 - t) * vis0, wb = t * vis1;
            float3 col = (wa * ca + wb * cb) / (wa + wb);
            float3 d = ca - cb;
            float e = dot(d, d) * (1.0 / 3.0);
            float pw = lerp(pwOcc, exp(-e * invSig2), vis0 * vis1);
            float w = bw * (pw + 1e-4);
            acc += w * lerp(cross, col, tv.w);
            wsum += w;
        }
    }
    Out[id.xy] = float4(acc / wsum, 1.0);
}
)";

// ----------------------------------------------------------------------------------------------- NVOF loader
namespace {

typedef NV_OF_STATUS(NVOFAPI* PFN_NvOFAPICreateInstanceD3D12)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice_)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

struct NvofApi {
    bool tried = false, ok = false;
    NV_OF_D3D12_API_FUNCTION_LIST fn{};
    PFN_D3D12CreateDevice_ createDevice = nullptr;
    bool Load() {
        if (tried) return ok;
        tried = true;
        HMODULE of = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!of) { LOGE("nvof: nvofapi64.dll not found (no NVIDIA driver?)"); return false; }
        auto create = (PFN_NvOFAPICreateInstanceD3D12)GetProcAddress(of, "NvOFAPICreateInstanceD3D12");
        if (!create) { LOGE("nvof: NvOFAPICreateInstanceD3D12 missing"); return false; }
        NV_OF_STATUS s = create(NV_OF_API_VERSION, &fn);
        if (s != NV_OF_SUCCESS) { LOGE("nvof: NvOFAPICreateInstanceD3D12 failed (%d)", (int)s); return false; }
        HMODULE d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        createDevice = d12 ? (PFN_D3D12CreateDevice_)GetProcAddress(d12, "D3D12CreateDevice") : nullptr;
        if (!createDevice) { LOGE("nvof: d3d12.dll unavailable"); return false; }
        ok = true;
        return true;
    }
};
NvofApi& Api() { static NvofApi a; return a; }

struct CBData {
    UINT W, H, FW, FH;
    float G, invG, t, pad0;
    float invW, invH, pad1, pad2;
    float alpha, pOcc, pEdge, pad3;
    float sGood, sBad, invSig2, pwOcc;
};

} // namespace

// ----------------------------------------------------------------------------------------------- interpolator
// Synchronization: the D3D11 flavour of the NVOF API does not order OFA writes against later D3D11
// work (measured: compute shaders, CopyResource and even event queries issued after nvOFExecute see
// stale/partial flow; results appear ~1.3 ms later). So NVOF runs through its D3D12 API on a D3D12
// device on the same adapter, on resources shared with our D3D11 device, synchronized with two
// shared fences, all GPU-side:
//   fenceIn_  : D3D11 Signal after writing the luma inputs  -> NVOF waits before reading them
//   fenceOut_ : NVOF signals when the flow is written        -> D3D11 Wait before the first read
class NvofInterpolator : public Interpolator {
public:
    explicit NvofInterpolator(int quality) : quality_(std::clamp(quality, 0, 2)) {}
    ~NvofInterpolator() override { Release(); }

    const char* Name() const override { return "nvof"; }

    bool Init(Device& dev, UINT w, UINT h) override {
        dev_ = &dev; w_ = w; h_ = h;
        if (!Api().Load()) return false;
        auto& F = Api().fn;
        switch (quality_) {
        case 0: grid_ = 4; perf_ = NV_OF_PERF_LEVEL_FAST; break;
        case 1: grid_ = 4; perf_ = NV_OF_PERF_LEVEL_MEDIUM; break;
        default: grid_ = 2; perf_ = NV_OF_PERF_LEVEL_FAST; break;
        }
        if (quality_ == 2 && getenv("NVOF_Q2")) { int g = 4, pf = 10; sscanf(getenv("NVOF_Q2"), "%d,%d", &g, &pf); grid_ = (UINT)g; perf_ = (NV_OF_PERF_LEVEL)pf; }
        fw_ = (w + grid_ - 1) / grid_;
        fh_ = (h + grid_ - 1) / grid_;
        try {
            ctx4_ = dev.ctx();
            HRESULT hr = Api().createDevice(dev.adapter(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12_));
            if (FAILED(hr)) { LOGE("nvof: D3D12CreateDevice failed: %s", HrToString(hr).c_str()); return false; }
            if (!CreateSharedFence(fenceIn12_, fenceIn11_) || !CreateSharedFence(fenceOut12_, fenceOut11_)) return false;

            NV_OF_STATUS s = F.nvCreateOpticalFlowD3D12(dev12_.Get(), &of_);
            if (s != NV_OF_SUCCESS) { LOGE("nvof: nvCreateOpticalFlowD3D12 failed (%d)", (int)s); of_ = nullptr; return false; }
            NV_OF_INIT_PARAMS ip{};
            ip.width = w; ip.height = h;
            ip.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)grid_;
            ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
            ip.mode = NV_OF_MODE_OPTICALFLOW;
            ip.perfLevel = perf_;
            ip.enableExternalHints = NV_OF_FALSE;
            ip.enableOutputCost = NV_OF_TRUE;
            ip.predDirection = NV_OF_PRED_DIRECTION_BOTH;
            ip.enableGlobalFlow = NV_OF_FALSE;
            ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
            s = F.nvOFInit(of_, &ip);
            if (s != NV_OF_SUCCESS) { LOGE("nvof: nvOFInit failed (%d): %s", (int)s, LastError().c_str()); return false; }

            for (auto& l : luma_)
                if (!CreateShared(w, h, DXGI_FORMAT_R8_UNORM, true, l.tex)) return false;
            for (auto& p : pairs_) {
                if (!CreateShared(fw_, fh_, DXGI_FORMAT_R16G16_SINT, false, p.flowF) ||
                    !CreateShared(fw_, fh_, DXGI_FORMAT_R16G16_SINT, false, p.flowB) ||
                    !CreateShared(fw_, fh_, DXGI_FORMAT_R8_UINT, false, p.costF) ||
                    !CreateShared(fw_, fh_, DXGI_FORMAT_R8_UINT, false, p.costB)) return false;
            }
            WaitCpu(fenceOut12_.Get(), outVal_);   // registrations complete
            tflow_ = dev.CreateTex(fw_, fh_, DXGI_FORMAT_R32G32B32A32_FLOAT, true, false);
            csLuma_ = dev.CompileCS(kNvofCS, "LumaCS", "nvof_luma");
            csTFlow_ = dev.CompileCS(kNvofCS, "TFlowCS", "nvof_tflow");
            csCompose_ = dev.CompileCS(kNvofCS, "ComposeCS", "nvof_compose");
            cb_ = dev.CreateConstantBuffer(sizeof(CBData));
        } catch (const std::exception& e) {
            LOGE("nvof: init failed: %s", e.what());
            return false;
        }
        LOGI("nvof: %ux%u, NVOF (D3D12 interop) grid %u perf %s, flow %ux%u", w, h, grid_,
             perf_ == NV_OF_PERF_LEVEL_FAST ? "fast" : perf_ == NV_OF_PERF_LEVEL_MEDIUM ? "medium" : "slow", fw_, fh_);
        return true;
    }

    void Prepare(const SourceFrame& a, const SourceFrame& b) override {
        std::lock_guard<std::mutex> lk(mu_);
        FindOrPrepare(a, b);
    }

    void Interpolate(const SourceFrame& a, const SourceFrame& b, float t, const Device::Tex& out) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto* ctx = dev_->ctx();
        Pair* p = FindOrPrepare(a, b);
        ID3D11SamplerState* ls = dev_->linearClamp();
        SetCB(t);
        if (!p) {
            // No flow available: plain cross-fade (confidence 0 everywhere).
            const float zero[4] = {0, 0, 0, 0};
            ctx->ClearUnorderedAccessViewFloat(tflow_.uav.Get(), zero);
        } else {
            WaitFlow(p->done);
            ID3D11ShaderResourceView* srvs[4] = {p->flowF.t11.srv.Get(), p->flowB.t11.srv.Get(), a.rgb.srv.Get(), b.rgb.srv.Get()};
            ctx->CSSetShader(csTFlow_.Get(), nullptr, 0);
            ctx->CSSetShaderResources(0, 4, srvs);
            ctx->CSSetUnorderedAccessViews(0, 1, tflow_.uav.GetAddressOf(), nullptr);
            ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
            ctx->CSSetSamplers(0, 1, &ls);
            ctx->Dispatch((fw_ + 15) / 16, (fh_ + 7) / 8, 1);
            dev_->ClearCS();
        }
        ID3D11ShaderResourceView* srvs[3] = {tflow_.srv.Get(), a.rgb.srv.Get(), b.rgb.srv.Get()};
        ctx->CSSetShader(csCompose_.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, out.uav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->CSSetSamplers(0, 1, &ls);
        ctx->Dispatch((w_ + 15) / 16, (h_ + 7) / 8, 1);
        dev_->ClearCS();
    }

private:
    struct FrameKey {
        int64_t n = -1; uint64_t gen = 0; const void* tex = nullptr;
        bool operator==(const FrameKey& o) const { return n == o.n && gen == o.gen && tex == o.tex; }
    };
    static FrameKey KeyOf(const SourceFrame& f) { return {f.n, f.generation, f.rgb.tex.Get()}; }

    struct Shared {
        ComPtr<ID3D12Resource> res12;
        Device::Tex t11;
        NvOFGPUBufferHandle h = nullptr;
    };
    struct Luma {
        Shared tex;
        FrameKey key;
        bool valid = false;
        uint64_t lastUse = 0;
        uint64_t readDone = 0;   // fenceOut value after which NVOF no longer reads it
    };
    struct Pair {
        Shared flowF, flowB, costF, costB;
        FrameKey ka, kb;
        bool valid = false;
        uint64_t lastUse = 0;
        uint64_t done = 0;       // fenceOut value signalled when the flow is written
    };

    std::string LastError() {
        char buf[512] = {};
        uint32_t n = sizeof(buf);
        if (of_) Api().fn.nvOFGetLastError(of_, buf, &n);
        return buf;
    }

    bool CreateSharedFence(ComPtr<ID3D12Fence>& f12, ComPtr<ID3D11Fence>& f11) {
        HRESULT hr = dev12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f12));
        if (FAILED(hr)) { LOGE("nvof: CreateFence failed: %s", HrToString(hr).c_str()); return false; }
        HANDLE sh = nullptr;
        hr = dev12_->CreateSharedHandle(f12.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
        if (FAILED(hr)) { LOGE("nvof: fence CreateSharedHandle failed: %s", HrToString(hr).c_str()); return false; }
        hr = dev_->dev()->OpenSharedFence(sh, IID_PPV_ARGS(&f11));
        CloseHandle(sh);
        if (FAILED(hr)) { LOGE("nvof: OpenSharedFence failed: %s", HrToString(hr).c_str()); return false; }
        return true;
    }

    // Texture allocated on the D3D11 device as an NT-handle shared resource, opened on the D3D12 device and
    // registered with NVOF. (The opposite direction, D3D12-created -> OpenSharedResource1, fails with
    // E_INVALIDARG on this driver for every format tried.)
    bool CreateShared(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, Shared& s) {
        s.t11 = dev_->CreateTex(w, h, fmt, uav, false, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
        ComPtr<IDXGIResource1> dr;
        HR_CHECK(s.t11.tex.As(&dr));
        HANDLE sh = nullptr;
        HRESULT hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sh);
        if (FAILED(hr)) { LOGE("nvof: CreateSharedHandle(fmt %d) failed: %s", (int)fmt, HrToString(hr).c_str()); return false; }
        hr = dev12_->OpenSharedHandle(sh, IID_PPV_ARGS(&s.res12));
        CloseHandle(sh);
        if (FAILED(hr)) { LOGE("nvof: D3D12 OpenSharedHandle(fmt %d) failed: %s", (int)fmt, HrToString(hr).c_str()); return false; }

        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp{};
        rp.resource = s.res12.Get();
        rp.inputFencePoint = {fenceOut12_.Get(), outVal_};
        rp.hOFGpuBuffer = &s.h;
        rp.outputFencePoint = {fenceOut12_.Get(), ++outVal_};
        NV_OF_STATUS st = Api().fn.nvOFRegisterResourceD3D12(of_, &rp);
        if (st != NV_OF_SUCCESS) { LOGE("nvof: register resource failed (%d): %s", (int)st, LastError().c_str()); s.h = nullptr; return false; }
        return true;
    }

    static void WaitCpu(ID3D12Fence* f, uint64_t v) {
        if (!f || f->GetCompletedValue() >= v) return;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(f->SetEventOnCompletion(v, ev))) WaitForSingleObject(ev, 2000);
        CloseHandle(ev);
    }

    // Makes subsequent D3D11 work safe w.r.t. NVOF job `v` (its flow is written / its inputs are no longer
    // read). Already finished (checked with a fence read): nothing to do. Otherwise a GPU-side queue wait:
    // no CPU stall; measured end-to-end latency is the same as spinning on the CPU for the fence.
    void WaitFlow(uint64_t v) {
        if (v <= waitedOut_) return;
        if (fenceOut12_->GetCompletedValue() < v) ctx4_->Wait(fenceOut11_.Get(), v);
        waitedOut_ = v;
    }

    void Release() {
        if (of_) {
            WaitCpu(fenceOut12_.Get(), outVal_);
            auto& F = Api().fn;
            auto unreg = [&](Shared& s) {
                if (!s.h) return;
                NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 up{s.h};
                F.nvOFUnregisterResourceD3D12(&up);
                s.h = nullptr;
            };
            for (auto& l : luma_) unreg(l.tex);
            for (auto& p : pairs_) { unreg(p.flowF); unreg(p.flowB); unreg(p.costF); unreg(p.costB); }
            // The D3D12 resources must be released before nvOFDestroy (releasing the input textures
            // afterwards crashes inside the driver).
            for (auto& l : luma_) l.tex.res12.Reset();
            for (auto& p : pairs_) { p.flowF.res12.Reset(); p.flowB.res12.Reset(); p.costF.res12.Reset(); p.costB.res12.Reset(); }
            F.nvOFDestroy(of_);
            of_ = nullptr;
        }
    }

    void SetCB(float t) {
        CBData cb{};
        cb.W = w_; cb.H = h_; cb.FW = fw_; cb.FH = fh_;
        cb.G = (float)grid_; cb.invG = 1.0f / (float)grid_; cb.t = t;
        cb.invW = 1.0f / (float)w_; cb.invH = 1.0f / (float)h_;
        cb.alpha = 30.0f;      // px of geometric error per unit mean-abs luma difference
        cb.pOcc = 3.0f;        // penalty (px) for a one-sided (occlusion) explanation
        cb.pEdge = 0.5f;       // penalty when the other side falls outside the image
        cb.sGood = 4.0f; cb.sBad = 16.0f;   // score -> confidence ramp
        cb.invSig2 = 1.0f / 0.004f;         // photometric weight in compose (mean sq RGB diff)
        cb.pwOcc = 0.3f;                    // weight of one-sided candidates in compose
        dev_->UpdateCB(cb_.Get(), &cb, sizeof(cb));
    }

    Luma* GetLuma(const SourceFrame& f, const Luma* exclude) {
        FrameKey k = KeyOf(f);
        for (auto& l : luma_) if (l.valid && l.key == k) { l.lastUse = ++useClock_; return &l; }
        Luma* v = nullptr;
        for (auto& l : luma_) {
            if (&l == exclude) continue;
            if (!l.valid) { v = &l; break; }
            if (!v || l.lastUse < v->lastUse) v = &l;
        }
        WaitFlow(v->readDone);   // WAR: NVOF must be done reading the old contents
        auto* ctx = dev_->ctx();
        SetCB(0.f);
        ctx->CSSetShader(csLuma_.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, f.rgb.srv.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, v->tex.t11.uav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->Dispatch((w_ + 15) / 16, (h_ + 7) / 8, 1);
        dev_->ClearCS();
        v->key = k; v->valid = true; v->lastUse = ++useClock_;
        return v;
    }

    Pair* FindOrPrepare(const SourceFrame& a, const SourceFrame& b) {
        if (!of_ || !a.rgb || !b.rgb) return nullptr;
        FrameKey ka = KeyOf(a), kb = KeyOf(b);
        for (auto& p : pairs_) if (p.valid && p.ka == ka && p.kb == kb) { p.lastUse = ++useClock_; return &p; }
        Pair* p = &pairs_[0];
        for (auto& q : pairs_) {
            if (!q.valid) { p = &q; break; }
            if (q.lastUse < p->lastUse) p = &q;
        }
        p->valid = false;
        Luma* la = GetLuma(a, nullptr);
        Luma* lb = GetLuma(b, la);

        // D3D11 -> NVOF: inputs written (and all earlier D3D11 reads of the output slot done).
        const uint64_t inV = ++inVal_;
        ctx4_->Signal(fenceIn11_.Get(), inV);
        dev_->ctx()->Flush();

        NV_OF_FENCE_POINT inFp{fenceIn12_.Get(), inV};
        NV_OF_FENCE_POINT outFp{fenceOut12_.Get(), outVal_ + 1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
        in.inputFrame = la->tex.h;
        in.referenceFrame = lb->tex.h;
        // Temporal hints only help when this pair continues the previous one.
        in.disableTemporalHints = (haveLast_ && lastB_ == ka) ? NV_OF_FALSE : NV_OF_TRUE;
        in.numFencePoints = 1;
        in.fencePoint = &inFp;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
        out.outputBuffer = p->flowF.h;
        out.outputCostBuffer = p->costF.h;
        out.bwdOutputBuffer = p->flowB.h;
        out.bwdOutputCostBuffer = p->costB.h;
        out.fencePoint = &outFp;
        NV_OF_STATUS s = Api().fn.nvOFExecuteD3D12(of_, &in, &out);
        if (s != NV_OF_SUCCESS) {
            if (execErrors_++ < 5) LOGE("nvof: nvOFExecuteD3D12 failed (%d): %s", (int)s, LastError().c_str());
            haveLast_ = false;
            return nullptr;
        }
        outVal_++;
        haveLast_ = true; lastB_ = kb;
        la->readDone = lb->readDone = outVal_;
        p->done = outVal_;
        p->ka = ka; p->kb = kb; p->valid = true; p->lastUse = ++useClock_;
        return p;
    }

    int quality_;
    Device* dev_ = nullptr;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    ComPtr<ID3D12Device> dev12_;
    ComPtr<ID3D12Fence> fenceIn12_, fenceOut12_;
    ComPtr<ID3D11Fence> fenceIn11_, fenceOut11_;
    uint64_t inVal_ = 0, outVal_ = 0, waitedOut_ = 0;
    UINT w_ = 0, h_ = 0, fw_ = 0, fh_ = 0, grid_ = 4;
    NV_OF_PERF_LEVEL perf_ = NV_OF_PERF_LEVEL_FAST;
    NvOFHandle of_ = nullptr;
    std::mutex mu_;
    Luma luma_[4];
    Pair pairs_[3];
    Device::Tex tflow_;
    ComPtr<ID3D11ComputeShader> csLuma_, csTFlow_, csCompose_;
    ComPtr<ID3D11Buffer> cb_;
    uint64_t useClock_ = 0;
    bool haveLast_ = false;
    FrameKey lastB_;
    int execErrors_ = 0;
};

std::unique_ptr<Interpolator> CreateFlowInterpolator(int quality) { return std::make_unique<NvofInterpolator>(quality); }
std::unique_ptr<Interpolator> CreateVfxInterpolator() { return nullptr; }

} // namespace camss
