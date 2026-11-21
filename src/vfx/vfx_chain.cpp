#include "vfx_chain.h"
#include "nvVideoEffects.h"
#include "nvCVImage.h"
#include "nvTransferD3D11.h"
#include <array>
#include <filesystem>

// Used by NVVideoEffectsProxy.cpp to locate the runtime.
char* g_nvVFXSDKPath = nullptr;

namespace camss {

// ---- selectors / parameters of VFX SDK 1.x effects (headers for these ship only in
// the login-gated SDK core; values taken from the SDK samples and the runtime itself).
static const char* kFxFrameGen = "VideoFrameGeneration";
static const char* kFxVideoSuperRes = "VideoSuperRes";
static const char* kFxTrueHdr = "TrueHDR";
static const char* kInputWidth = "InputWidth";
static const char* kInputHeight = "InputHeight";
static const char* kQualityLevel = "QualityLevel";
static const char* kImageEncodingMode = "ImageEncodingMode";
static const char* kFgMode = "Mode";
static const char* kFgFrameMultiplier = "FrameMultiplier";
static const char* kFgTimestep = "Timestep";
static const char* kFgShotChange = "ShotChange";
static const char* kFgAutoShotChange = "AutomaticShotChangeDetectionEnabled";
static const NvCVImage_PixelFormat kNVCV_RGB10A2 = (NvCVImage_PixelFormat)13;
static const NvCVImage_ComponentType kNVCV_P32 = (NvCVImage_ComponentType)11;
static const unsigned kEncRGB8 = 0, kEncRGB10A2 = 1;

#define VFX_CHECK(expr) do { NvCV_Status _s = (expr); if (_s != NVCV_SUCCESS) { \
    LOGE("%s failed: %d (%s)", #expr, (int)_s, NvCV_GetErrorStringFromCode(_s)); return false; } } while (0)

// Minimal CUDA runtime event API, loaded from the VFX runtime's cudart64_12.dll so
// that we share its context without linking the CUDA toolkit.
struct CudaRt {
    typedef int (*EventCreate)(void**);
    typedef int (*EventRecord)(void*, void*);
    typedef int (*EventQuery)(void*);
    typedef int (*EventElapsed)(float*, void*, void*);
    typedef int (*EventDestroy)(void*);
    EventCreate create = nullptr; EventRecord record = nullptr; EventQuery query = nullptr;
    EventElapsed elapsed = nullptr; EventDestroy destroy = nullptr;
    bool Load(const std::wstring& dir) {
        HMODULE m = LoadLibraryW((dir + L"\\cudart64_12.dll").c_str());
        if (!m) return false;
        create = (EventCreate)GetProcAddress(m, "cudaEventCreate");
        record = (EventRecord)GetProcAddress(m, "cudaEventRecord");
        query = (EventQuery)GetProcAddress(m, "cudaEventQuery");
        elapsed = (EventElapsed)GetProcAddress(m, "cudaEventElapsedTime");
        destroy = (EventDestroy)GetProcAddress(m, "cudaEventDestroy");
        return create && record && query && elapsed && destroy;
    }
};

struct Effect {
    NvVFX_Handle h = nullptr;
    ~Effect() { if (h) NvVFX_DestroyEffect(h); }
};

struct VfxImpl {
    Device* dev = nullptr;
    VfxChain::Config cfg;
    CUstream stream = nullptr;
    SourceFrame* store = nullptr;
    int storeCount = 0;
    // D3D views of the source store (registered once) and their CUDA copies.
    std::vector<NvCVImage> d3dSrc;
    std::vector<NvCVImage> gpuSrc;
    std::vector<int64_t> gpuN;
    NvCVImage tmp;                      // staging for Transfer
    // Effects and their buffers.
    Effect fg, hdr, sr;
    NvCVImage fgOut, hdrOut, srOut;     // GPU
    NvCVImage* lastSrIn = nullptr;
    NvCVImage* lastFgA = nullptr; NvCVImage* lastFgB = nullptr;
    int64_t lastPairA = -1, lastPairB = -1;
    // Output D3D textures (one per possible final format/size) + interop images.
    Device::Tex outTex[2];              // [0] = out res, [1] = src res (SR toggled off)
    NvCVImage outD3d[2];
    Device::Tex packedTex[2];           // R32_UINT interop targets for RGB10A2 output
    ComPtr<ID3D11ComputeShader> unpackCS;
    ComPtr<ID3D11Buffer> unpackCB;
    bool outPq = false;
    // Timing.
    CudaRt rt;
    static constexpr int kEv = 6, kRing = 4;
    void* ev[kRing][kEv] = {};
    bool evPending[kRing] = {};
    const char* evName[kEv] = {"vfx.start", "vfx.fg", "vfx.hdr", "vfx.sr", "vfx.out", "vfx.end"};
    int evFrame = 0;
    std::vector<std::pair<const char*, double>> timings;

    ~VfxImpl() {
        for (Effect* e : {&fg, &hdr, &sr}) if (e->h) { NvVFX_DestroyEffect(e->h); e->h = nullptr; }
        if (stream) NvVFX_CudaStreamDestroy(stream);
    }

    void Record(int stage) {
        if (!cfg.profile || !ev[evFrame][stage]) return;
        rt.record(ev[evFrame][stage], stream);
    }
    void CollectTimings() {
        for (int f = 0; f < kRing; f++) {
            if (!evPending[f]) continue;
            if (rt.query(ev[f][kEv - 1]) != 0) continue;   // not done
            evPending[f] = false;
            float total = 0;
            for (int s = 1; s < kEv; s++) {
                float ms = 0;
                if (rt.elapsed(&ms, ev[f][s - 1], ev[f][s]) == 0) { timings.push_back({evName[s], ms}); total += ms; }
            }
            timings.push_back({"vfx.gpu_total", total});
        }
    }
};

static std::wstring ResolveRuntimeDir(const std::wstring& configured) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (!configured.empty()) candidates.push_back(configured);
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"CAMSS_VFX_RUNTIME", env, MAX_PATH)) candidates.push_back(env);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path exeDir = fs::path(exe).parent_path();
    candidates.push_back(exeDir / L"nvvfx_runtime");
#ifdef CAMSS_VFX_RUNTIME_DIR
    candidates.push_back(fs::path(CAMSS_VFX_RUNTIME_DIR));
#endif
    for (auto& c : candidates)
        if (fs::exists(c / L"NVVideoEffects.dll") && fs::exists(c / L"nvVFXVideoFrameGeneration.dll")) {
            // Backslashes + absolute: LOAD_WITH_ALTERED_SEARCH_PATH misbehaves with forward slashes.
            fs::path p = fs::absolute(c).lexically_normal();
            p.make_preferred();
            std::wstring w = p.wstring();
            while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
            return w;
        }
    return {};
}

// Loads the VFX 1.3 runtime exactly once, before any NvCVImage object exists: the
// NvCVImage default constructor already calls into NVCVImage.dll through the proxy,
// which would otherwise bind whatever NVCVImage.dll is first on PATH (e.g. an older
// 0.7.x install in Program Files that lacks RGB10A2 and the 1.x effects).
static std::wstring g_runtimeDir;
static bool LoadVfxRuntime() {
    static int state = 0;   // 0 = not tried, 1 = ok, -1 = failed
    if (state) return state > 0;
    state = -1;
    std::wstring dir = ResolveRuntimeDir(L"");
    if (dir.empty()) { LOGE("VFX runtime (NVVideoEffects 1.3 + feature DLLs) not found; set CAMSS_VFX_RUNTIME"); return false; }
    if (HMODULE pre = GetModuleHandleW(L"NVCVImage.dll")) {
        wchar_t m[MAX_PATH] = L"?"; GetModuleFileNameW(pre, m, MAX_PATH);
        LOGW("an NVCVImage.dll is already loaded (%s)", WideToUtf8(m).c_str());
    }
    SetDllDirectoryW(dir.c_str());     // feature DLLs (nvVFX*.dll, nvngx_*.dll, cudart) resolve from here
    AddDllDirectory(dir.c_str());
    HMODULE hImg = LoadLibraryExW((dir + L"\\NVCVImage.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hImg) { LOGE("failed to load %s\\NVCVImage.dll (err %lu)", WideToUtf8(dir.c_str()).c_str(), GetLastError()); return false; }
    HMODULE hVfx = LoadLibraryExW((dir + L"\\NVVideoEffects.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hVfx) { LOGE("failed to load %s\\NVVideoEffects.dll (err %lu)", WideToUtf8(dir.c_str()).c_str(), GetLastError()); return false; }
    static std::string dirA = WideToUtf8(dir.c_str());
    g_nvVFXSDKPath = dirA.data();      // used by NVVideoEffectsProxy.cpp
    g_runtimeDir = dir;
    // The proxies resolve "NVCVImage" / "NVVideoEffects" by module name: verify they bind ours.
    wchar_t m1[MAX_PATH] = L"?";
    GetModuleFileNameW(GetModuleHandleW(L"NVCVImage.dll"), m1, MAX_PATH);
    if (_wcsicmp(m1, (dir + L"\\NVCVImage.dll").c_str()) != 0) {
        LOGE("wrong NVCVImage.dll bound (%s); the VFX 1.3 runtime needs its own copy", WideToUtf8(m1).c_str());
        return false;
    }
    unsigned ver = 0;
    NvVFX_GetVersion(&ver);
    LOGI("VFX runtime %u.%u.%u.%u from %s", ver >> 24, (ver >> 16) & 0xff, (ver >> 8) & 0xff, ver & 0xff, dirA.c_str());
    state = 1;
    return true;
}

VfxChain::VfxChain() { LoadVfxRuntime(); p_ = std::make_unique<VfxImpl>(); }
VfxChain::~VfxChain() = default;
bool VfxChain::HasFg() const { return p_->fg.h != nullptr; }
bool VfxChain::OutputIsPq() const { return p_->outPq; }

static bool CreateEffect(const char* sel, Effect& e) {
    NvCV_Status s = NvVFX_CreateEffect(sel, &e.h);
    if (s != NVCV_SUCCESS) { LOGE("NvVFX_CreateEffect(%s) failed: %d (%s)", sel, (int)s, NvCV_GetErrorStringFromCode(s)); e.h = nullptr; return false; }
    return true;
}

bool VfxChain::Init(Device& dev, const Config& cfg, SourceFrame* store, int storeCount) {
    VfxImpl& p = *p_;
    p.dev = &dev; p.cfg = cfg; p.store = store; p.storeCount = storeCount;

    if (!LoadVfxRuntime()) return false;
    const std::wstring& dir = g_runtimeDir;
    if (cfg.profile && !p.rt.Load(dir)) { LOGW("cudart events unavailable; VFX GPU timing disabled"); p.cfg.profile = false; }

    VFX_CHECK(NvVFX_CudaStreamCreate(&p.stream));
    const UINT sw = cfg.srcW, sh = cfg.srcH;

    // Source store: register every D3D texture once; allocate CUDA copies.
    p.d3dSrc.resize(storeCount);
    p.gpuSrc.resize(storeCount);
    p.gpuN.assign(storeCount, -1);
    for (int i = 0; i < storeCount; i++) {
        VFX_CHECK(NvCVImage_InitFromD3D11Texture(&p.d3dSrc[i], store[i].rgb.tex.Get()));
        VFX_CHECK(NvCVImage_Alloc(&p.gpuSrc[i], sw, sh, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32));
    }

    // ---- Video Frame Generation
    if (cfg.fg) {
        if (!CreateEffect(kFxFrameGen, p.fg)) return false;
        VFX_CHECK(NvVFX_SetCudaStream(p.fg.h, NVVFX_CUDA_STREAM, p.stream));
        VFX_CHECK(NvVFX_SetU32(p.fg.h, kInputWidth, sw));
        VFX_CHECK(NvVFX_SetU32(p.fg.h, kInputHeight, sh));
        VFX_CHECK(NvVFX_SetU32(p.fg.h, kFgMode, (unsigned)std::clamp(cfg.fgMode, 0, 2)));
        VFX_CHECK(NvVFX_SetU32(p.fg.h, kFgAutoShotChange, 1u));
        int64_t t0 = Qpc::Now();
        VFX_CHECK(NvVFX_Load(p.fg.h));
        LOGI("VFX VideoFrameGeneration loaded (mode %d) in %.0f ms", cfg.fgMode, Qpc::ToMs(Qpc::Now() - t0));
        VFX_CHECK(NvCVImage_Alloc(&p.fgOut, sw, sh, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32));
        VFX_CHECK(NvVFX_SetImage(p.fg.h, NVVFX_OUTPUT_IMAGE, &p.fgOut));
        VFX_CHECK(NvVFX_SetU32(p.fg.h, kFgFrameMultiplier, 0u));   // explicit timestep mode
    }

    // ---- TrueHDR (input RGBA8, output RGB10A2 PQ, same size)
    const bool hdrAtSrc = cfg.hdr && (cfg.hdrBeforeSr || !cfg.sr);
    if (cfg.hdr) {
        UINT hw = hdrAtSrc ? sw : cfg.outW, hh = hdrAtSrc ? sh : cfg.outH;
        if (!CreateEffect(kFxTrueHdr, p.hdr)) return false;
        VFX_CHECK(NvVFX_SetCudaStream(p.hdr.h, NVVFX_CUDA_STREAM, p.stream));
        VFX_CHECK(NvVFX_SetU32(p.hdr.h, "Contrast", cfg.hdrContrast));
        VFX_CHECK(NvVFX_SetU32(p.hdr.h, "Saturation", cfg.hdrSaturation));
        VFX_CHECK(NvVFX_SetU32(p.hdr.h, "MiddleGray", cfg.hdrMiddleGray));
        VFX_CHECK(NvVFX_SetU32(p.hdr.h, "Luminance", cfg.hdrPeakNits));
        VFX_CHECK(NvCVImage_Alloc(&p.hdrOut, hw, hh, kNVCV_RGB10A2, kNVCV_P32, NVCV_CHUNKY, NVCV_GPU, 32));
        // TrueHDR needs a representative input bound before Load.
        NvCVImage* in0 = hdrAtSrc ? &p.gpuSrc[0] : nullptr;
        static NvCVImage dummyOut;
        if (!in0) { VFX_CHECK(NvCVImage_Alloc(&dummyOut, hw, hh, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32)); in0 = &dummyOut; }
        VFX_CHECK(NvVFX_SetImage(p.hdr.h, NVVFX_INPUT_IMAGE, in0));
        VFX_CHECK(NvVFX_SetImage(p.hdr.h, NVVFX_OUTPUT_IMAGE, &p.hdrOut));
        int64_t t0 = Qpc::Now();
        VFX_CHECK(NvVFX_Load(p.hdr.h));
        LOGI("VFX TrueHDR loaded (%ux%u, peak %u nits) in %.0f ms", hw, hh, cfg.hdrPeakNits, Qpc::ToMs(Qpc::Now() - t0));
    }

    // ---- Video Super Resolution
    if (cfg.sr) {
        const bool tenBit = cfg.hdr && hdrAtSrc;
        if (!CreateEffect(kFxVideoSuperRes, p.sr)) return false;
        VFX_CHECK(NvVFX_SetCudaStream(p.sr.h, NVVFX_CUDA_STREAM, p.stream));
        VFX_CHECK(NvVFX_SetU32(p.sr.h, kImageEncodingMode, tenBit ? kEncRGB10A2 : kEncRGB8));
        if (tenBit) VFX_CHECK(NvCVImage_Alloc(&p.srOut, cfg.outW, cfg.outH, kNVCV_RGB10A2, kNVCV_P32, NVCV_CHUNKY, NVCV_GPU, 32));
        else VFX_CHECK(NvCVImage_Alloc(&p.srOut, cfg.outW, cfg.outH, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32));
        NvCVImage* in0 = tenBit ? &p.hdrOut : &p.gpuSrc[0];
        VFX_CHECK(NvVFX_SetImage(p.sr.h, NVVFX_INPUT_IMAGE, in0));
        VFX_CHECK(NvVFX_SetImage(p.sr.h, NVVFX_OUTPUT_IMAGE, &p.srOut));
        VFX_CHECK(NvVFX_SetU32(p.sr.h, kQualityLevel, (unsigned)cfg.srQuality));
        VFX_CHECK(NvVFX_SetF32(p.sr.h, NVVFX_STRENGTH, cfg.srStrength));
        int64_t t0 = Qpc::Now();
        VFX_CHECK(NvVFX_Load(p.sr.h));
        p.lastSrIn = in0;
        LOGI("VFX VideoSuperRes loaded (%ux%u -> %ux%u, quality %d, %s) in %.0f ms", sw, sh, cfg.outW, cfg.outH, cfg.srQuality,
             tenBit ? "RGB10A2" : "RGBA8", Qpc::ToMs(Qpc::Now() - t0));
    }

    // ---- Output textures (D3D) the chain writes into.
    p.outPq = cfg.hdr;
    DXGI_FORMAT of = p.outPq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT ow = cfg.sr ? cfg.outW : sw, oh = cfg.sr ? cfg.outH : sh;
    // CUDA interop cannot register R10G10B10A2 textures, so HDR output goes through an
    // R32_UINT texture (identical 32-bit packing, reinterpreted as RGB10A2 for the copy)
    // and a tiny D3D compute pass unpacks it into R10G10B10A2_UNORM.
    NvCVImage ref10;
    if (p.outPq) {
        VFX_CHECK(NvCVImage_Alloc(&ref10, 16, 16, kNVCV_RGB10A2, kNVCV_P32, NVCV_CHUNKY, NVCV_GPU, 0));
        static const char* kUnpackCS = R"(
Texture2D<uint> Src : register(t0);
RWTexture2D<unorm float4> Dst : register(u0);
cbuffer CB : register(b0) { uint w; uint h; uint2 pad; };
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint v = Src[id.xy];
    Dst[id.xy] = float4(float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u), 1023.0) / 1023.0;
}
)";
        p.unpackCS = dev.CompileCS(kUnpackCS, "main", "unpack_rgb10a2_cs");
        p.unpackCB = dev.CreateConstantBuffer(16);
    }
    auto makeOut = [&](int i, UINT w, UINT h) -> bool {
        p.outTex[i] = dev.CreateTex(w, h, of, true, true);
        if (!p.outPq) { VFX_CHECK(NvCVImage_InitFromD3D11Texture(&p.outD3d[i], p.outTex[i].tex.Get())); return true; }
        p.packedTex[i] = dev.CreateTex(w, h, DXGI_FORMAT_R32_UINT, true, false);
        VFX_CHECK(NvCVImage_InitFromD3D11Texture(&p.outD3d[i], p.packedTex[i].tex.Get()));
        p.outD3d[i].pixelFormat = ref10.pixelFormat;
        p.outD3d[i].componentType = ref10.componentType;
        p.outD3d[i].pixelBytes = ref10.pixelBytes;
        p.outD3d[i].componentBytes = ref10.componentBytes;
        p.outD3d[i].numComponents = ref10.numComponents;
        return true;
    };
    if (!makeOut(0, ow, oh)) return false;
    if (cfg.sr && !makeOut(1, sw, sh)) return false;
    VFX_CHECK(NvCVImage_Alloc(&p.tmp, ow, oh, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32));

    if (p.cfg.profile)
        for (auto& f : p.ev) for (auto& e : f) p.rt.create(&e);
    return true;
}

void VfxChain::Ingest(const SourceFrame& f) {
    VfxImpl& p = *p_;
    int k = (int)(f.n % p.storeCount);
    p.dev->ctx()->Flush();                       // make the colour conversion visible to CUDA
    if (NvCVImage_MapResource(&p.d3dSrc[k], p.stream) != NVCV_SUCCESS) { LOGE("VFX map src failed"); return; }
    NvCV_Status s = NvCVImage_Transfer(&p.d3dSrc[k], &p.gpuSrc[k], 1.f, p.stream, nullptr);
    NvCVImage_UnmapResource(&p.d3dSrc[k], p.stream);
    if (s != NVCV_SUCCESS) { LOGE("VFX ingest transfer failed: %d", (int)s); return; }
    p.gpuN[k] = f.n;
}

const Device::Tex* VfxChain::Process(const SourceFrame& A, const SourceFrame* B, float t, bool srOn) {
    VfxImpl& p = *p_;
    if (p.cfg.profile) p.CollectTimings();
    int ka = (int)(A.n % p.storeCount);
    if (p.gpuN[ka] != A.n) return nullptr;
    NvCVImage* cur = &p.gpuSrc[ka];
    bool started = false;
    auto startEv = [&] {
        if (!started && p.cfg.profile) {
            if (p.evPending[p.evFrame]) { started = false; return; }
            p.Record(0);
            started = true;
        }
    };
    startEv();

    // 1) frame generation at source resolution
    if (p.fg.h && B && t > 0.f) {
        int kb = (int)(B->n % p.storeCount);
        if (p.gpuN[kb] == B->n) {
            if (A.n != p.lastPairA || B->n != p.lastPairB) {
                // New pair: both inputs must be (re)bound at the start of every pair.
                NvVFX_SetImage(p.fg.h, NVVFX_INPUT_IMAGE_0, &p.gpuSrc[ka]);
                NvVFX_SetImage(p.fg.h, NVVFX_INPUT_IMAGE_1, &p.gpuSrc[kb]);
                NvVFX_SetU32(p.fg.h, kFgShotChange, 0u);
                p.lastPairA = A.n; p.lastPairB = B->n;
            }
            NvVFX_SetF32(p.fg.h, kFgTimestep, std::clamp(t, 0.001f, 0.999f));
            NvCV_Status s = NvVFX_Run(p.fg.h, 0);
            if (s == NVCV_SUCCESS) cur = &p.fgOut;
            else LOGW("VideoFrameGeneration run failed: %d (%s)", (int)s, NvCV_GetErrorStringFromCode(s));
        }
    }
    if (started) p.Record(1);

    const bool hdrAtSrc = p.cfg.hdr && (p.cfg.hdrBeforeSr || !p.cfg.sr);
    // 2) TrueHDR at source resolution
    if (p.hdr.h && hdrAtSrc) {
        NvVFX_SetImage(p.hdr.h, NVVFX_INPUT_IMAGE, cur);
        if (NvVFX_Run(p.hdr.h, 0) == NVCV_SUCCESS) cur = &p.hdrOut;
    }
    if (started) p.Record(2);

    // 3) super resolution
    bool scaled = false;
    if (p.sr.h && srOn) {
        if (cur != p.lastSrIn) { NvVFX_SetImage(p.sr.h, NVVFX_INPUT_IMAGE, cur); p.lastSrIn = cur; }
        if (NvVFX_Run(p.sr.h, 0) == NVCV_SUCCESS) { cur = &p.srOut; scaled = true; }
        // TrueHDR after SR (at output resolution)
        if (p.hdr.h && !hdrAtSrc) {
            NvVFX_SetImage(p.hdr.h, NVVFX_INPUT_IMAGE, cur);
            if (NvVFX_Run(p.hdr.h, 0) == NVCV_SUCCESS) cur = &p.hdrOut;
        }
    }
    if (started) p.Record(3);

    // 4) hand the result to D3D
    int oi = (p.cfg.sr && !scaled) ? 1 : 0;
    // HDR enabled but SR toggled off with HDR-after-SR: output isn't PQ; keep it simple and require consistency.
    NvCVImage& dst = p.outD3d[oi];
    if (NvCVImage_MapResource(&dst, p.stream) != NVCV_SUCCESS) { LOGE("VFX map out failed"); return nullptr; }
    NvCV_Status s = NvCVImage_Transfer(cur, &dst, 1.f, p.stream, &p.tmp);
    if (started) p.Record(4);
    NvCVImage_UnmapResource(&dst, p.stream);
    if (started) { p.Record(5); p.evPending[p.evFrame] = true; p.evFrame = (p.evFrame + 1) % VfxImpl::kRing; }
    if (s != NVCV_SUCCESS) {
        static int warned = 0;
        if (warned++ < 3) LOGE("VFX output transfer failed: %d (%s)", (int)s, NvCV_GetErrorStringFromCode(s));
        return nullptr;
    }
    if (p.outPq) {
        // Unpack the R32_UINT staging texture into R10G10B10A2_UNORM (D3D side, after unmap).
        auto* ctx = p.dev->ctx();
        const Device::Tex& o = p.outTex[oi];
        struct { UINT w, h, pad[2]; } cb{o.w, o.h, {0, 0}};
        p.dev->UpdateCB(p.unpackCB.Get(), &cb, sizeof(cb));
        ctx->CSSetShader(p.unpackCS.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, p.packedTex[oi].srv.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, o.uav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, p.unpackCB.GetAddressOf());
        ctx->Dispatch((o.w + 15) / 16, (o.h + 7) / 8, 1);
        p.dev->ClearCS();
    }
    return &p.outTex[oi];
}

std::vector<std::pair<const char*, double>> VfxChain::PopTimings() {
    std::vector<std::pair<const char*, double>> out;
    out.swap(p_->timings);
    return out;
}

} // namespace camss
