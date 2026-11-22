#include "vfx_chain.h"
#include "nvVideoEffects.h"
#include "nvCVImage.h"
#include "nvTransferD3D11.h"
#include "videofx_version.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

char* g_nvVFXSDKPath = nullptr;

namespace cameravfx {

namespace {

constexpr const char* kEffectFrameGeneration = "VideoFrameGeneration";
constexpr const char* kEffectSuperRes = "VideoSuperRes";
constexpr const char* kEffectTrueHdr = "TrueHDR";
constexpr const char* kParamQualityLevel = "QualityLevel";
constexpr const char* kParamImageEncodingMode = "ImageEncodingMode";
constexpr const char* kParamFrameMultiplier = "FrameMultiplier";
constexpr const char* kParamTimestep = "Timestep";
constexpr const char* kParamShotChange = "ShotChange";
constexpr const char* kParamAutoShotChange = "AutomaticShotChangeDetectionEnabled";
constexpr const char* kParamContrast = "Contrast";
constexpr const char* kParamSaturation = "Saturation";
constexpr const char* kParamMiddleGray = "MiddleGray";
constexpr const char* kParamPeakLuminance = "Luminance";

constexpr unsigned kSrEncodingRgba8 = 0, kSrEncodingRgb10a2 = 1;
constexpr unsigned kFgExplicitTimestepMultiplier = 0;
constexpr float kSrStrength = 1.0f;
constexpr unsigned kHdrContrast = 100, kHdrSaturation = 100, kHdrMiddleGray = 50, kHdrPeakNits = 650;
constexpr float kMinTimestep = 0.001f, kMaxTimestep = 0.999f;
constexpr unsigned kGpuRowAlignment = 32;
constexpr UINT kUnpackGroupWidth = 16, kUnpackGroupHeight = 8;
constexpr double kFailureLogIntervalMs = 2000;
constexpr int kCudaSuccess = 0;

constexpr const wchar_t* kRuntimeDirEnvVar = L"CAMERAVFX_VFX_RUNTIME";
constexpr const wchar_t* kRuntimeDirNextToExe = L"nvvfx_runtime";
constexpr const wchar_t* kCudaRuntimeDll = L"cudart64_12.dll";

constexpr const char* kUnpackRgb10a2Cs = R"(
Texture2D<uint> Src : register(t0);
RWTexture2D<unorm float4> Dst : register(u0);
cbuffer CB : register(b0) { uint w; uint h; uint2 padding; };
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint v = Src[id.xy];
    Dst[id.xy] = float4(float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u), 1023.0) / 1023.0;
}
)";

#define VFX_CHECK(expr) do { NvCV_Status _s = (expr); if (_s != NVCV_SUCCESS) { \
    LOGE("%s failed: %d (%s)", #expr, (int)_s, NvCV_GetErrorStringFromCode(_s)); return false; } } while (0)

enum class Failure { SourceMap, SourceTransfer, FgRun, HdrRun, SrRun, OutputMap, OutputTransfer, Count };
constexpr const char* kFailureNames[] = {"source map", "source transfer", "VideoFrameGeneration run", "TrueHDR run",
                                         "VideoSuperRes run", "output map", "output transfer"};
static_assert(std::size(kFailureNames) == (size_t)Failure::Count);

void LogFailure(Failure what, NvCV_Status s) {
    static std::array<std::atomic<int64_t>, (size_t)Failure::Count> lastLogged{};
    if (LogEnabled(LogLevel::Error) && detail::LogRateOk(lastLogged[(size_t)what], kFailureLogIntervalMs))
        LOGE("VFX %s failed: %d (%s)", kFailureNames[(size_t)what], (int)s, NvCV_GetErrorStringFromCode(s));
}

struct CudaEvents {
    using Create = int (*)(void**);
    using Record = int (*)(void*, void*);
    using Query = int (*)(void*);
    using Elapsed = int (*)(float*, void*, void*);
    using Destroy = int (*)(void*);
    Create create = nullptr;
    Record record = nullptr;
    Query query = nullptr;
    Elapsed elapsed = nullptr;
    Destroy destroy = nullptr;

    static std::optional<CudaEvents> Load(const std::filesystem::path& runtimeDir) {
        HMODULE cudart = LoadLibraryW((runtimeDir / kCudaRuntimeDll).c_str());
        if (!cudart) return std::nullopt;
        CudaEvents e;
        e.create = (Create)GetProcAddress(cudart, "cudaEventCreate");
        e.record = (Record)GetProcAddress(cudart, "cudaEventRecord");
        e.query = (Query)GetProcAddress(cudart, "cudaEventQuery");
        e.elapsed = (Elapsed)GetProcAddress(cudart, "cudaEventElapsedTime");
        e.destroy = (Destroy)GetProcAddress(cudart, "cudaEventDestroy");
        if (!e.create || !e.record || !e.query || !e.elapsed || !e.destroy) return std::nullopt;
        return e;
    }
};

struct VfxRuntime {
    std::string dirUtf8;
    std::optional<CudaEvents> cudaEvents;
};

std::filesystem::path RuntimeDirFromEnvironment() {
    wchar_t buf[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(kRuntimeDirEnvVar, buf, MAX_PATH);
    return len > 0 && len < MAX_PATH ? std::filesystem::path(buf) : std::filesystem::path();
}

std::filesystem::path RuntimeDirNextToExe() { return std::filesystem::path(ExeDir()) / kRuntimeDirNextToExe; }

std::filesystem::path FindRuntimeDir() {
    namespace fs = std::filesystem;
    for (const fs::path& c : {RuntimeDirFromEnvironment(), RuntimeDirNextToExe()}) {
        if (c.empty() || !fs::exists(c / L"NVVideoEffects.dll") || !fs::exists(c / L"nvVFXVideoFrameGeneration.dll")) continue;
        fs::path dir = fs::absolute(c).lexically_normal();
        dir.make_preferred();
        return dir.has_filename() ? dir : dir.parent_path();
    }
    return {};
}

void ForwardSdkLog(void*, const char* msg) {
    std::string_view text = msg ? msg : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    LOGW("VFX SDK: %.*s", (int)text.size(), text.data());
}

std::unique_ptr<VfxRuntime> LoadRuntime() {
    const std::filesystem::path dir = FindRuntimeDir();
    if (dir.empty()) {
        const std::string envVar = WideToUtf8(kRuntimeDirEnvVar);
        const std::filesystem::path fromEnvironment = RuntimeDirFromEnvironment();
        LOGE("VFX runtime (NVVideoEffects 1.3 + feature DLLs) not found in %%%s%% (%s) or %s; fetch it with tools/get-vfx-runtime.ps1 and rebuild, "
             "or set %s to its folder",
             envVar.c_str(), fromEnvironment.empty() ? "not set" : WideToUtf8(fromEnvironment.c_str()).c_str(),
             WideToUtf8(RuntimeDirNextToExe().c_str()).c_str(), envVar.c_str());
        return nullptr;
    }
    auto runtime = std::make_unique<VfxRuntime>();
    runtime->dirUtf8 = WideToUtf8(dir.c_str());
    SetDllDirectoryW(dir.c_str());
    for (const wchar_t* dll : {L"NVCVImage.dll", L"NVVideoEffects.dll"}) {
        if (LoadLibraryExW((dir / dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) continue;
        const DWORD err = GetLastError();
        LOGE("failed to load %s (err %lu)", WideToUtf8((dir / dll).c_str()).c_str(), err);
        return nullptr;
    }
    wchar_t boundImageDll[MAX_PATH] = L"?";
    GetModuleFileNameW(GetModuleHandleW(L"NVCVImage.dll"), boundImageDll, MAX_PATH);
    if (_wcsicmp(boundImageDll, (dir / L"NVCVImage.dll").c_str()) != 0) {
        LOGE("wrong NVCVImage.dll bound (%s); the VFX 1.3 runtime needs its own copy", WideToUtf8(boundImageDll).c_str());
        return nullptr;
    }
    g_nvVFXSDKPath = runtime->dirUtf8.data();
    unsigned ver = 0;
    NvVFX_GetVersion(&ver);
    const unsigned major = ver >> 24, minor = (ver >> 16) & 0xff;
    LOGI("VFX runtime %u.%u.%u.%u from %s (built with SDK headers %s)", major, minor, (ver >> 8) & 0xff, ver & 0xff,
         runtime->dirUtf8.c_str(), NVIDIA_VIDEOEFFECTS_SDK_VERSION_STRING);
    if (major != NVIDIA_VIDEOEFFECTS_SDK_VERSION_MAJOR || minor != NVIDIA_VIDEOEFFECTS_SDK_VERSION_MINOR)
        LOGW("VFX runtime %u.%u does not match the SDK headers %s; effects may fail to load", major, minor,
             NVIDIA_VIDEOEFFECTS_SDK_VERSION_STRING_MAJOR_MINOR);
    NvVFX_ConfigureLogger(NVCV_LOG_WARNING, nullptr, ForwardSdkLog, nullptr);
    runtime->cudaEvents = CudaEvents::Load(dir);
    if (!runtime->cudaEvents) LOGW("cudart events unavailable; VFX GPU timing disabled");
    return runtime;
}

const VfxRuntime* Runtime() {
    static const std::unique_ptr<VfxRuntime> runtime = LoadRuntime();
    return runtime.get();
}

struct CudaStream {
    CUstream handle = nullptr;
    CudaStream() = default;
    CudaStream(const CudaStream&) = delete;
    CudaStream& operator=(const CudaStream&) = delete;
    ~CudaStream() {
        if (handle) NvVFX_CudaStreamDestroy(handle);
    }
};

struct Effect {
    NvVFX_Handle handle = nullptr;
    Effect() = default;
    Effect(Effect&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    Effect& operator=(Effect&& other) noexcept {
        std::swap(handle, other.handle);
        return *this;
    }
    ~Effect() {
        if (handle) NvVFX_DestroyEffect(handle);
    }
};

bool CreateEffect(const char* selector, Effect& effect) {
    const NvCV_Status s = NvVFX_CreateEffect(selector, &effect.handle);
    if (s == NVCV_SUCCESS) return true;
    LOGE("NvVFX_CreateEffect(%s) failed: %d (%s)", selector, (int)s, NvCV_GetErrorStringFromCode(s));
    effect.handle = nullptr;
    return false;
}

enum class GpuFormat { Rgba8, Rgb10a2 };

NvCV_Status AllocGpuImage(NvCVImage& image, UINT w, UINT h, GpuFormat format) {
    return format == GpuFormat::Rgb10a2 ? NvCVImage_Alloc(&image, w, h, NVCV_RGB10A2, NVCV_P32, NVCV_CHUNKY, NVCV_GPU, kGpuRowAlignment)
                                        : NvCVImage_Alloc(&image, w, h, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, kGpuRowAlignment);
}

NvCV_Status ReinterpretAsRgb10a2(NvCVImage& image) {
    NvCVImage layout;
    const NvCV_Status s = NvCVImage_Init(&layout, 1, 1, 4, nullptr, NVCV_RGB10A2, NVCV_P32, NVCV_CHUNKY, NVCV_GPU);
    image.pixelFormat = layout.pixelFormat;
    image.componentType = layout.componentType;
    image.pixelBytes = layout.pixelBytes;
    image.componentBytes = layout.componentBytes;
    image.numComponents = layout.numComponents;
    return s;
}

enum class Stage { Start, Fg, Hdr, Sr, Out };

class StageTimer {
public:
    explicit StageTimer(const CudaEvents* events) : events_(events) {}
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
    ~StageTimer() {
        if (!events_) return;
        for (Frame& frame : frames_)
            for (void* e : frame.events)
                if (e) events_->destroy(e);
    }

    void Start(CUstream stream) {
        stream_ = stream;
        if (!events_) return;
        enabled_ = true;
        for (Frame& frame : frames_)
            for (void*& e : frame.events) enabled_ = enabled_ && events_->create(&e) == kCudaSuccess;
    }
    void Begin() {
        recording_ = enabled_ && !frames_[current_].pending;
        frames_[current_].marks = 0;
        MarkAfter(Stage::Start);
    }
    void MarkAfter(Stage stage) {
        if (!recording_) return;
        Frame& frame = frames_[current_];
        events_->record(frame.events[frame.marks], stream_);
        frame.stages[frame.marks++] = stage;
    }
    void End() {
        if (!recording_) return;
        MarkAfter(Stage::Out);
        frames_[current_].pending = true;
        current_ = (current_ + 1) % kFramesInFlight;
        recording_ = false;
    }
    void Abandon() { recording_ = false; }
    void Collect(std::vector<VfxTimings>& timings) {
        if (!enabled_) return;
        for (Frame& frame : frames_) {
            if (!frame.pending || events_->query(frame.events[frame.marks - 1]) != kCudaSuccess) continue;
            frame.pending = false;
            VfxTimings t;
            for (int i = 1; i < frame.marks; i++) {
                float ms = 0;
                if (events_->elapsed(&ms, frame.events[i - 1], frame.events[i]) != kCudaSuccess) continue;
                StageMs(t, frame.stages[i]) = ms;
                t.totalMs += ms;
            }
            timings.push_back(t);
        }
    }

private:
    static constexpr int kFramesInFlight = 4;
    static constexpr int kMaxMarks = 5;
    struct Frame {
        std::array<void*, kMaxMarks> events{};
        std::array<Stage, kMaxMarks> stages{};
        int marks = 0;
        bool pending = false;
    };
    static std::optional<double>& StageMs(VfxTimings& t, Stage stage) {
        switch (stage) {
        case Stage::Fg: return t.fgMs;
        case Stage::Hdr: return t.hdrMs;
        case Stage::Sr: return t.srMs;
        case Stage::Start:
        case Stage::Out: break;
        }
        return t.outMs;
    }
    const CudaEvents* events_;
    CUstream stream_ = nullptr;
    bool enabled_ = false, recording_ = false;
    std::array<Frame, kFramesInFlight> frames_{};
    int current_ = 0;
};

struct Output {
    Device::Tex tex;
    Device::Tex packed;
    NvCVImage interop;
    Encoding encoding = Encoding::SdrGamma;
};

}

struct VfxImpl {
    VfxImpl(Device& device, const VfxChain::Config& config, std::span<SourceFrame> frames, const VfxRuntime& runtime)
        : dev(device), cfg(config), hdrAtSource(config.hdr && !(config.sr && config.hdrAfterSr)), store(frames),
          timer(runtime.cudaEvents ? &*runtime.cudaEvents : nullptr), fgQuality(config.fgQuality), srQuality(config.srQuality) {}

    bool Init();
    bool LoadFg(int quality);
    bool LoadSr(int quality);
    bool LoadHdr();
    bool ReloadFg(int quality);
    bool ReloadSr(int quality);
    bool MakeOutput(Output& output, UINT w, UINT h, Encoding encoding);
    NvCV_Status RunHdr(NvCVImage* input);
    void UnpackRgb10a2(const Output& output);
    VfxOutput Fail(Failure what, NvCV_Status s);
    int Slot(const SourceFrame& frame) const { return (int)(&frame - store.data()); }
    NvCVImage* SrLoadInput() { return hdrAtSource ? &hdrOut : &srcCuda[0]; }

    Device& dev;
    const VfxChain::Config cfg;
    const bool hdrAtSource;
    std::span<SourceFrame> store;
    CudaStream stream;
    StageTimer timer;
    std::vector<NvCVImage> srcInterop;
    std::vector<NvCVImage> srcCuda;
    std::vector<int64_t> srcCudaFrameN;
    NvCVImage fgOut, hdrOut, srOut, transferStaging;
    Output out, srOffOut;
    ComPtr<ID3D11ComputeShader> unpackCs;
    ComPtr<ID3D11Buffer> unpackCb;
    Effect fg, hdr, sr;
    int fgQuality, srQuality;
    NvCVImage* srBoundInput = nullptr;
    static constexpr int kUnbound = -1;
    int fgBoundSlotA = kUnbound, fgBoundSlotB = kUnbound;
    double fgLoadMs = 0, srLoadMs = 0;
    std::vector<VfxTimings> timings;
};

bool VfxImpl::LoadFg(int quality) {
    if (!CreateEffect(kEffectFrameGeneration, fg)) return false;
    VFX_CHECK(NvVFX_SetCudaStream(fg.handle, NVVFX_CUDA_STREAM, stream.handle));
    VFX_CHECK(NvVFX_SetU32(fg.handle, NVVFX_INPUT_WIDTH, cfg.srcW));
    VFX_CHECK(NvVFX_SetU32(fg.handle, NVVFX_INPUT_HEIGHT, cfg.srcH));
    VFX_CHECK(NvVFX_SetU32(fg.handle, NVVFX_MODE, (unsigned)quality));
    VFX_CHECK(NvVFX_SetU32(fg.handle, kParamAutoShotChange, 1u));
    const int64_t t0 = Qpc::Now();
    VFX_CHECK(NvVFX_Load(fg.handle));
    fgLoadMs = Qpc::ToMs(Qpc::Now() - t0);
    LOGI("VFX VideoFrameGeneration loaded (quality %d) in %.0f ms", quality, fgLoadMs);
    VFX_CHECK(NvVFX_SetImage(fg.handle, NVVFX_OUTPUT_IMAGE, &fgOut));
    VFX_CHECK(NvVFX_SetU32(fg.handle, kParamFrameMultiplier, kFgExplicitTimestepMultiplier));
    fgQuality = quality;
    return true;
}

bool VfxImpl::LoadSr(int quality) {
    if (!CreateEffect(kEffectSuperRes, sr)) return false;
    VFX_CHECK(NvVFX_SetCudaStream(sr.handle, NVVFX_CUDA_STREAM, stream.handle));
    VFX_CHECK(NvVFX_SetU32(sr.handle, kParamImageEncodingMode, hdrAtSource ? kSrEncodingRgb10a2 : kSrEncodingRgba8));
    VFX_CHECK(NvVFX_SetImage(sr.handle, NVVFX_INPUT_IMAGE, SrLoadInput()));
    VFX_CHECK(NvVFX_SetImage(sr.handle, NVVFX_OUTPUT_IMAGE, &srOut));
    VFX_CHECK(NvVFX_SetU32(sr.handle, kParamQualityLevel, (unsigned)quality));
    VFX_CHECK(NvVFX_SetF32(sr.handle, NVVFX_STRENGTH, kSrStrength));
    const int64_t t0 = Qpc::Now();
    VFX_CHECK(NvVFX_Load(sr.handle));
    srLoadMs = Qpc::ToMs(Qpc::Now() - t0);
    LOGI("VFX VideoSuperRes loaded (%ux%u -> %ux%u, quality %d, %s) in %.0f ms", cfg.srcW, cfg.srcH, cfg.outW, cfg.outH, quality,
         hdrAtSource ? "RGB10A2" : "RGBA8", srLoadMs);
    srBoundInput = SrLoadInput();
    srQuality = quality;
    return true;
}

bool VfxImpl::LoadHdr() {
    const UINT w = hdrAtSource ? cfg.srcW : cfg.outW, h = hdrAtSource ? cfg.srcH : cfg.outH;
    if (!CreateEffect(kEffectTrueHdr, hdr)) return false;
    VFX_CHECK(NvVFX_SetCudaStream(hdr.handle, NVVFX_CUDA_STREAM, stream.handle));
    VFX_CHECK(NvVFX_SetU32(hdr.handle, kParamContrast, kHdrContrast));
    VFX_CHECK(NvVFX_SetU32(hdr.handle, kParamSaturation, kHdrSaturation));
    VFX_CHECK(NvVFX_SetU32(hdr.handle, kParamMiddleGray, kHdrMiddleGray));
    VFX_CHECK(NvVFX_SetU32(hdr.handle, kParamPeakLuminance, kHdrPeakNits));
    VFX_CHECK(AllocGpuImage(hdrOut, w, h, GpuFormat::Rgb10a2));
    VFX_CHECK(NvVFX_SetImage(hdr.handle, NVVFX_INPUT_IMAGE, hdrAtSource ? &srcCuda[0] : &srOut));
    VFX_CHECK(NvVFX_SetImage(hdr.handle, NVVFX_OUTPUT_IMAGE, &hdrOut));
    const int64_t t0 = Qpc::Now();
    VFX_CHECK(NvVFX_Load(hdr.handle));
    LOGI("VFX TrueHDR loaded (%ux%u, peak %u nits) in %.0f ms", w, h, kHdrPeakNits, Qpc::ToMs(Qpc::Now() - t0));
    return true;
}

bool VfxImpl::ReloadFg(int quality) {
    const int previous = fgQuality;
    fg = {};
    fgBoundSlotA = fgBoundSlotB = kUnbound;
    if (LoadFg(quality)) return true;
    fg = {};
    if (!LoadFg(previous)) fg = {};
    return false;
}

bool VfxImpl::ReloadSr(int quality) {
    const int previous = srQuality;
    sr = {};
    if (LoadSr(quality)) return true;
    sr = {};
    if (!LoadSr(previous)) sr = {};
    return false;
}

bool VfxImpl::MakeOutput(Output& output, UINT w, UINT h, Encoding encoding) {
    const bool pq = encoding == Encoding::Pq;
    output.encoding = encoding;
    output.tex = dev.CreateTex(w, h, pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    if (!pq) {
        VFX_CHECK(NvCVImage_InitFromD3D11Texture(&output.interop, output.tex.tex.Get()));
        return true;
    }
    output.packed = dev.CreateTex(w, h, DXGI_FORMAT_R32_UINT, true, false);
    VFX_CHECK(NvCVImage_InitFromD3D11Texture(&output.interop, output.packed.tex.Get()));
    VFX_CHECK(ReinterpretAsRgb10a2(output.interop));
    return true;
}

bool VfxImpl::Init() {
    VFX_CHECK(NvVFX_CudaStreamCreate(&stream.handle));
    timer.Start(stream.handle);

    srcInterop.resize(store.size());
    srcCuda.resize(store.size());
    srcCudaFrameN.assign(store.size(), -1);
    for (size_t i = 0; i < store.size(); i++) {
        VFX_CHECK(NvCVImage_InitFromD3D11Texture(&srcInterop[i], store[i].rgb.tex.Get()));
        VFX_CHECK(AllocGpuImage(srcCuda[i], cfg.srcW, cfg.srcH, GpuFormat::Rgba8));
    }

    const int effectCount = (int)cfg.fg + (int)cfg.hdr + (int)cfg.sr;
    int effectsLoaded = 0;
    auto reportLoaded = [&] {
        if (cfg.onLoadProgress) cfg.onLoadProgress((float)++effectsLoaded / (float)effectCount);
    };
    if (cfg.fg) {
        VFX_CHECK(AllocGpuImage(fgOut, cfg.srcW, cfg.srcH, GpuFormat::Rgba8));
        if (!LoadFg(cfg.fgQuality)) return false;
        reportLoaded();
    }
    if (cfg.sr) VFX_CHECK(AllocGpuImage(srOut, cfg.outW, cfg.outH, hdrAtSource ? GpuFormat::Rgb10a2 : GpuFormat::Rgba8));
    if (cfg.hdr) {
        if (!LoadHdr()) return false;
        reportLoaded();
    }
    if (cfg.sr) {
        if (!LoadSr(cfg.srQuality)) return false;
        reportLoaded();
    }

    if (cfg.hdr) {
        unpackCs = dev.CompileCS(kUnpackRgb10a2Cs, "main", "unpack_rgb10a2_cs");
        unpackCb = dev.CreateConstantBuffer(16);
    }
    if (!MakeOutput(out, cfg.outW, cfg.outH, cfg.hdr ? Encoding::Pq : Encoding::SdrGamma)) return false;
    if (cfg.sr && !MakeOutput(srOffOut, cfg.srcW, cfg.srcH, hdrAtSource ? Encoding::Pq : Encoding::SdrGamma)) return false;
    VFX_CHECK(AllocGpuImage(transferStaging, cfg.outW, cfg.outH, GpuFormat::Rgba8));
    return true;
}

NvCV_Status VfxImpl::RunHdr(NvCVImage* input) {
    NvVFX_SetImage(hdr.handle, NVVFX_INPUT_IMAGE, input);
    return NvVFX_Run(hdr.handle, 0);
}

void VfxImpl::UnpackRgb10a2(const Output& output) {
    ID3D11DeviceContext* ctx = dev.Context();
    const struct { UINT w, h, padding[2]; } constants{output.tex.w, output.tex.h, {0, 0}};
    dev.UpdateCB(unpackCb.Get(), &constants, sizeof(constants));
    ctx->CSSetShader(unpackCs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, output.packed.srv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 1, output.tex.uav.GetAddressOf(), nullptr);
    ctx->CSSetConstantBuffers(0, 1, unpackCb.GetAddressOf());
    ctx->Dispatch((output.tex.w + kUnpackGroupWidth - 1) / kUnpackGroupWidth, (output.tex.h + kUnpackGroupHeight - 1) / kUnpackGroupHeight, 1);
    dev.ClearCS();
}

VfxOutput VfxImpl::Fail(Failure what, NvCV_Status s) {
    timer.Abandon();
    LogFailure(what, s);
    return {};
}

void RecordVfxTimings(StatBook& stats, const VfxTimings& timings) {
    auto add = [&](std::string_view key, const std::optional<double>& ms) {
        if (ms) stats.Add(key, *ms);
    };
    add(stat::kVfxFg, timings.fgMs);
    add(stat::kVfxHdr, timings.hdrMs);
    add(stat::kVfxSr, timings.srMs);
    add(stat::kVfxOut, timings.outMs);
    stats.Add(stat::kVfxGpuTotal, timings.totalMs);
}

std::unique_ptr<VfxChain> VfxChain::Create(Device& dev, const Config& cfg, std::span<SourceFrame> store) {
    const VfxRuntime* runtime = Runtime();
    if (!runtime) return nullptr;
    auto impl = std::make_unique<VfxImpl>(dev, cfg, store, *runtime);
    if (!impl->Init()) return nullptr;
    return std::unique_ptr<VfxChain>(new VfxChain(std::move(impl)));
}

VfxChain::VfxChain(std::unique_ptr<VfxImpl> impl) : p_(std::move(impl)) {}
VfxChain::~VfxChain() = default;
bool VfxChain::Loaded() const { return (!p_->cfg.fg || p_->fg.handle) && (!p_->cfg.sr || p_->sr.handle); }
int VfxChain::FgQuality() const { return p_->fgQuality; }
int VfxChain::SrQuality() const { return p_->srQuality; }
double VfxChain::FgLoadMs() const { return p_->fgLoadMs; }
double VfxChain::SrLoadMs() const { return p_->srLoadMs; }

bool VfxChain::Reload(int fgQuality, int srQuality) {
    VfxImpl& p = *p_;
    NvVFX_CudaStreamSynchronize(p.stream.handle);
    const bool fgOk = !p.cfg.fg || fgQuality == p.fgQuality || p.ReloadFg(fgQuality);
    const bool srOk = !p.cfg.sr || srQuality == p.srQuality || p.ReloadSr(srQuality);
    return fgOk && srOk;
}

void VfxChain::Ingest(const SourceFrame& frame) {
    VfxImpl& p = *p_;
    const int slot = p.Slot(frame);
    p.dev.Context()->Flush();
    NvCV_Status s = NvCVImage_MapResource(&p.srcInterop[slot], p.stream.handle);
    if (s != NVCV_SUCCESS) {
        LogFailure(Failure::SourceMap, s);
        return;
    }
    s = NvCVImage_Transfer(&p.srcInterop[slot], &p.srcCuda[slot], 1.f, p.stream.handle, nullptr);
    NvCVImage_UnmapResource(&p.srcInterop[slot], p.stream.handle);
    if (s != NVCV_SUCCESS) {
        LogFailure(Failure::SourceTransfer, s);
        return;
    }
    p.srcCudaFrameN[slot] = frame.n;
    if (slot == p.fgBoundSlotA || slot == p.fgBoundSlotB) p.fgBoundSlotA = p.fgBoundSlotB = VfxImpl::kUnbound;
}

VfxOutput VfxChain::Process(const SourceFrame& a, const SourceFrame* b, float t, bool srOn) {
    VfxImpl& p = *p_;
    p.timer.Collect(p.timings);
    const int slotA = p.Slot(a);
    if (p.srcCudaFrameN[slotA] != a.n) return {};
    NvCVImage* cur = &p.srcCuda[slotA];
    p.timer.Begin();

    bool generated = false;
    const int slotB = b ? p.Slot(*b) : VfxImpl::kUnbound;
    if (b && p.cfg.fg && p.srcCudaFrameN[slotB] == b->n) {
        if (slotA != p.fgBoundSlotA || slotB != p.fgBoundSlotB) {
            NvVFX_SetImage(p.fg.handle, NVVFX_INPUT_IMAGE_0, cur);
            NvVFX_SetImage(p.fg.handle, NVVFX_INPUT_IMAGE_1, &p.srcCuda[slotB]);
            NvVFX_SetU32(p.fg.handle, kParamShotChange, 0u);
            p.fgBoundSlotA = slotA;
            p.fgBoundSlotB = slotB;
        }
        NvVFX_SetF32(p.fg.handle, kParamTimestep, std::clamp(t, kMinTimestep, kMaxTimestep));
        if (NvCV_Status s = NvVFX_Run(p.fg.handle, 0); s != NVCV_SUCCESS) return p.Fail(Failure::FgRun, s);
        cur = &p.fgOut;
        generated = true;
        p.timer.MarkAfter(Stage::Fg);
    }

    if (p.hdrAtSource) {
        if (NvCV_Status s = p.RunHdr(cur); s != NVCV_SUCCESS) return p.Fail(Failure::HdrRun, s);
        cur = &p.hdrOut;
        p.timer.MarkAfter(Stage::Hdr);
    }

    const bool scaled = p.cfg.sr && srOn;
    if (scaled) {
        if (cur != p.srBoundInput) {
            NvVFX_SetImage(p.sr.handle, NVVFX_INPUT_IMAGE, cur);
            p.srBoundInput = cur;
        }
        if (NvCV_Status s = NvVFX_Run(p.sr.handle, 0); s != NVCV_SUCCESS) return p.Fail(Failure::SrRun, s);
        cur = &p.srOut;
        p.timer.MarkAfter(Stage::Sr);
        if (p.cfg.hdr && !p.hdrAtSource) {
            if (NvCV_Status s = p.RunHdr(cur); s != NVCV_SUCCESS) return p.Fail(Failure::HdrRun, s);
            cur = &p.hdrOut;
            p.timer.MarkAfter(Stage::Hdr);
        }
    }

    Output& out = p.cfg.sr && !scaled ? p.srOffOut : p.out;
    if (NvCV_Status s = NvCVImage_MapResource(&out.interop, p.stream.handle); s != NVCV_SUCCESS) return p.Fail(Failure::OutputMap, s);
    const NvCV_Status transfer = NvCVImage_Transfer(cur, &out.interop, 1.f, p.stream.handle, &p.transferStaging);
    NvCVImage_UnmapResource(&out.interop, p.stream.handle);
    p.timer.End();
    if (transfer != NVCV_SUCCESS) return p.Fail(Failure::OutputTransfer, transfer);
    if (out.encoding == Encoding::Pq) p.UnpackRgb10a2(out);
    return {&out.tex, out.encoding, generated};
}

std::vector<VfxTimings> VfxChain::PopTimings() { return std::exchange(p_->timings, {}); }

}
