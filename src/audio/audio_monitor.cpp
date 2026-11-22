#include "audio_monitor.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace cameravfx {
namespace {

constexpr wchar_t kAudioThreadName[] = L"CameraVFX audio";

const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kPkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const DEVPROPKEY kDevpkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const DEVPROPKEY kDevpkeyInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
const GUID kBuiltInDevicesContainer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
const GUID kKsSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr double kPi = 3.14159265358979323846;

constexpr double kServoKpPpmPerMs = 400.0;
constexpr double kServoKiPpmPerMsSec = 60.0;
constexpr double kMaxTrimPpm = 5000.0;
constexpr double kMaxIntegralPpm = 4000.0;
constexpr double kTrimSlewPpmPerSec = 2000.0;
constexpr double kMinTrimChangePpm = 1.0;
constexpr double kPpmPerSyncMsPerSec = 1000.0;
constexpr double kServoIntervalSec = 0.05;
constexpr double kServoMaxStepSec = 0.5;
constexpr double kFillSmoothingSec = 0.5;
constexpr double kMaxRenderEventGapSec = 0.1;

constexpr double kExcessDropSec = 0.060;
constexpr double kPrimeExcessDropSec = 0.002;
constexpr uint32_t kUnderrunFadeFrames = 64;
constexpr double kUnderrunGuardStepMs = 3.0;
constexpr double kUnderrunGuardMaxMs = 30.0;
constexpr double kUnderrunGuardDecayMs = 1.0;
constexpr double kUnderrunGuardDecayIntervalSec = 30.0;

constexpr double kSyncSlewMsPerSec = 4.0;
constexpr double kSyncAccelMsPerSec2 = 1.5;
constexpr double kAutoSyncMaxMs = 250.0;
constexpr double kSyncHoldSec = 0.5;
constexpr double kSyncStepWindowSec = 10.0;
constexpr double kSyncTargetSmoothingSec = 2.0;
constexpr double kSyncSettledMs = 0.01;
constexpr double kSyncStepLogMinMs = 0.05;

constexpr double kCapturePllGain = 0.05;
constexpr double kCaptureTimestampSnapSec = 0.005;
constexpr double kCaptureTimestampMaxAheadSec = 0.005;
constexpr double kCaptureTimestampMaxAgeSec = 1.0;

constexpr REFERENCE_TIME kLegacyCaptureBufferHns = 1000000;
constexpr REFERENCE_TIME kLegacyRenderBufferHns = 500000;
constexpr double kRenderMarginMinSec = 0.0015;
constexpr double kRingHeadroomSec = 1.5;
constexpr size_t kChunkSlots = 8192;
static_assert((kChunkSlots & (kChunkSlots - 1)) == 0);
constexpr size_t kChunkMask = kChunkSlots - 1;

constexpr DWORD kWaitTimeoutMs = 100;
constexpr double kPublishIntervalSec = 0.1;
constexpr double kRenderSilenceTimeoutSec = 2.0;
constexpr double kRetrySec = 2.0;
constexpr double kStableSessionSec = 30.0;
constexpr int kFailuresLoggedFirst = 3;
constexpr int kFailureLogEvery = 30;
constexpr double kIncidentLogIntervalSec = 5.0;

constexpr double kGainRampSec = 0.010;
constexpr double kToneHz = 440.0;
constexpr float kToneAmplitude = 0.01585f;

struct CoMemFree { void operator()(void* p) const { CoTaskMemFree(p); } };
using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoMemFree>;

std::string GuidStr(const GUID& g) {
    wchar_t b[64] = {};
    StringFromGUID2(g, b, 64);
    return WideToUtf8(b);
}
double NowSec() { return Qpc::ToSec(Qpc::Now()); }

enum class SampleKind { Unknown, F32, F64, I16, I24, I32 };

struct WaveFormat {
    SampleKind kind = SampleKind::Unknown;
    uint32_t rate = 0, channels = 0, blockBytes = 0, bits = 0;
    DWORD channelMask = 0;
};

WORD TagOfKsSubtype(const GUID& g) {
    const bool isKsSubtype = g.Data2 == 0 && g.Data3 == 0x10 && memcmp(g.Data4, kKsSubtypeIeeeFloat.Data4, 8) == 0;
    return isKsSubtype ? (WORD)g.Data1 : 0;
}

WaveFormat ParseFormat(const WAVEFORMATEX* wf) {
    WaveFormat f;
    f.rate = wf->nSamplesPerSec;
    f.channels = wf->nChannels;
    f.blockBytes = wf->nBlockAlign;
    f.bits = wf->wBitsPerSample;
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.channelMask = x->dwChannelMask;
        tag = TagOfKsSubtype(x->SubFormat);
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT) f.kind = f.bits == 32 ? SampleKind::F32 : f.bits == 64 ? SampleKind::F64 : SampleKind::Unknown;
    else if (tag == WAVE_FORMAT_PCM) f.kind = f.bits == 16 ? SampleKind::I16 : f.bits == 24 ? SampleKind::I24 : f.bits == 32 ? SampleKind::I32 : SampleKind::Unknown;
    if (f.channels == 0 || f.blockBytes != f.channels * (f.bits / 8)) f.kind = SampleKind::Unknown;
    return f;
}

const char* SampleKindName(SampleKind k) {
    switch (k) {
    case SampleKind::F32: return "float32";
    case SampleKind::F64: return "float64";
    case SampleKind::I16: return "pcm16";
    case SampleKind::I24: return "pcm24";
    case SampleKind::I32: return "pcm32";
    case SampleKind::Unknown: break;
    }
    return "?";
}

std::string DescribeFormat(const WAVEFORMATEX* wf) {
    const WaveFormat f = ParseFormat(wf);
    char b[96];
    snprintf(b, sizeof(b), "%u Hz %u ch %s", f.rate, f.channels, SampleKindName(f.kind));
    return b;
}

void ConvertToFloat(const BYTE* src, float* dst, size_t samples, SampleKind k) {
    constexpr float kInt16Scale = 1.0f / 32768.0f;
    constexpr float kInt32Scale = 1.0f / 2147483648.0f;
    switch (k) {
    case SampleKind::F32: memcpy(dst, src, samples * sizeof(float)); break;
    case SampleKind::F64: { const double* s = reinterpret_cast<const double*>(src); for (size_t i = 0; i < samples; i++) dst[i] = (float)s[i]; break; }
    case SampleKind::I16: { const int16_t* s = reinterpret_cast<const int16_t*>(src); for (size_t i = 0; i < samples; i++) dst[i] = s[i] * kInt16Scale; break; }
    case SampleKind::I24:
        for (size_t i = 0; i < samples; i++, src += 3) {
            const int32_t v = (int32_t)(((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24));
            dst[i] = (float)v * kInt32Scale;
        }
        break;
    case SampleKind::I32: { const int32_t* s = reinterpret_cast<const int32_t*>(src); for (size_t i = 0; i < samples; i++) dst[i] = (float)s[i] * kInt32Scale; break; }
    case SampleKind::Unknown: break;
    }
}

DWORD DefaultChannelMask(uint32_t channels) {
    constexpr DWORD kStereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    constexpr DWORD kQuad = kStereo | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    constexpr DWORD kSurround51 = kQuad | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY;
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return kStereo;
    case 3: return kStereo | SPEAKER_FRONT_CENTER;
    case 4: return kQuad;
    case 5: return kQuad | SPEAKER_FRONT_CENTER;
    case 6: return kSurround51;
    case 8: return kSurround51 | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}

WAVEFORMATEXTENSIBLE FloatFormat(uint32_t rate, uint32_t channels, DWORD channelMask) {
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)channels;
    fmt.Format.nSamplesPerSec = rate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = (WORD)(sizeof(float) * channels);
    fmt.Format.nAvgBytesPerSec = rate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = channelMask ? channelMask : DefaultChannelMask(channels);
    fmt.SubFormat = kKsSubtypeIeeeFloat;
    return fmt;
}

ComPtr<IMMDeviceEnumerator> DeviceEnumerator() {
    ComPtr<IMMDeviceEnumerator> devices;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
    if (FAILED(hr)) LOGW("audio: MMDeviceEnumerator unavailable: %s", HrToString(hr).c_str());
    return devices;
}

struct Endpoint {
    std::wstring id, name;
    std::optional<GUID> container;
    bool isDefault = false;
    std::string format;
};

bool UsableContainer(const GUID& g) { return g != GUID_NULL && g != kBuiltInDevicesContainer; }

bool SharePhysicalDevice(const Endpoint& a, const Endpoint& b) {
    return a.container && UsableContainer(*a.container) && a.container == b.container;
}

std::wstring DefaultEndpointId(IMMDeviceEnumerator* devices, EDataFlow flow) {
    ComPtr<IMMDevice> d;
    LPWSTR id = nullptr;
    if (FAILED(devices->GetDefaultAudioEndpoint(flow, eConsole, &d)) || FAILED(d->GetId(&id))) return {};
    std::wstring s = id;
    CoTaskMemFree(id);
    return s;
}

std::vector<Endpoint> EnumEndpoints(IMMDeviceEnumerator* devices, EDataFlow flow, bool withFormat) {
    std::vector<Endpoint> out;
    const std::wstring defaultId = DefaultEndpointId(devices, flow);
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(devices->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        LPWSTR id = nullptr;
        if (FAILED(col->Item(i, &d)) || FAILED(d->GetId(&id))) continue;
        Endpoint e;
        e.id = id;
        CoTaskMemFree(id);
        e.isDefault = e.id == defaultId;
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) e.name = v.pwszVal;
            PropVariantClear(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) e.container = *v.puuid;
            PropVariantClear(&v);
        }
        if (withFormat) {
            ComPtr<IAudioClient> client;
            WAVEFORMATEX* wf = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) && SUCCEEDED(client->GetMixFormat(&wf))) {
                e.format = DescribeFormat(wf);
                CoTaskMemFree(wf);
            } else {
                e.format = "mix format unavailable";
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<GUID> UsableContainerOf(const GUID& g) {
    if (!UsableContainer(g)) return std::nullopt;
    return g;
}

std::optional<GUID> VideoContainerId(const std::wstring& symbolicLink) {
    if (symbolicLink.empty()) return std::nullopt;
    GUID container{};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(container);
    if (CM_Get_Device_Interface_PropertyW(symbolicLink.c_str(), &kDevpkeyContainerId, &type, (PBYTE)&container, &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_GUID)
        return UsableContainerOf(container);
    wchar_t instanceId[MAX_DEVICE_ID_LEN + 1] = {};
    size = sizeof(instanceId);
    type = 0;
    if (CM_Get_Device_Interface_PropertyW(symbolicLink.c_str(), &kDevpkeyInstanceId, &type, (PBYTE)instanceId, &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return std::nullopt;
    DEVINST devNode = 0;
    if (CM_Locate_DevNodeW(&devNode, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return std::nullopt;
    size = sizeof(container);
    type = 0;
    if (CM_Get_DevNode_PropertyW(devNode, &kDevpkeyContainerId, &type, (PBYTE)&container, &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_GUID)
        return std::nullopt;
    return UsableContainerOf(container);
}

bool IsLoopbackName(const std::wstring& name) {
    static const wchar_t* kLoopbackNames[] = {L"Stereo Mix", L"What U Hear", L"Wave Out Mix", L"Loopback", L"Rec. Playback", L"立体声混音"};
    for (const wchar_t* n : kLoopbackNames)
        if (ContainsIgnoreCase(name, n)) return true;
    return false;
}

template <class Pred>
const Endpoint* FindFirst(const std::vector<Endpoint>& endpoints, Pred pred) {
    for (const Endpoint& e : endpoints)
        if (pred(e)) return &e;
    return nullptr;
}

const Endpoint* FindByName(const std::vector<Endpoint>& endpoints, const std::wstring& name) {
    return FindFirst(endpoints, [&](const Endpoint& e) { return ContainsIgnoreCase(e.name, name); });
}

}

struct AudioImpl {
    AudioOptions opt;
    ComPtr<IMMDeviceEnumerator> devices;
    std::thread thread;
    HANDLE stopEvent = nullptr;

    std::atomic<bool> muted{false};
    std::atomic<bool> paused{false};
    std::atomic<float> volume{1.0f};
    std::atomic<double> videoLatencyMs{0.0};

    std::wstring captureId, renderId;

    mutable std::mutex statsMutex;
    AudioStats stats;

    bool StopRequested() const { return WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0; }
    template <class F> void UpdateStats(F update) {
        std::lock_guard<std::mutex> lock(statsMutex);
        update(stats);
    }
    bool Resolve(bool verbose);
    void ThreadMain();
};

bool AudioImpl::Resolve(bool verbose) {
    std::vector<Endpoint> captures = EnumEndpoints(devices.Get(), eCapture, false);
    std::erase_if(captures, [](const Endpoint& e) { return IsLoopbackName(e.name); });

    const Endpoint* capture = nullptr;
    std::string method;
    if (!opt.captureDevice.empty()) {
        capture = FindByName(captures, opt.captureDevice);
        method = "name";
        if (!capture) {
            if (verbose) LOGW("audio: no usable capture endpoint matching \"%s\" (%zu candidates; --list shows them)",
                              WideToUtf8(opt.captureDevice.c_str()).c_str(), captures.size());
            return false;
        }
    } else {
        const std::optional<GUID> videoContainer = VideoContainerId(opt.videoSymbolicLink);
        const auto inVideoDevice = [&](const Endpoint& e) { return videoContainer && e.container == videoContainer; };
        capture = FindFirst(captures, [&](const Endpoint& e) { return inVideoDevice(e) && ContainsIgnoreCase(e.name, opt.videoDeviceName); });
        if (!capture) capture = FindFirst(captures, inVideoDevice);
        if (capture) {
            method = "ContainerId " + GuidStr(*videoContainer);
        } else {
            capture = FindByName(captures, opt.videoDeviceName);
            method = "video device name";
        }
        if (!capture) {
            if (verbose) LOGW("audio: no usable capture endpoint belongs to video device \"%s\" (%zu candidates)",
                              WideToUtf8(opt.videoDeviceName.c_str()).c_str(), captures.size());
            return false;
        }
    }

    std::vector<Endpoint> renders = EnumEndpoints(devices.Get(), eRender, false);
    std::erase_if(renders, [&](const Endpoint& r) { return SharePhysicalDevice(*capture, r) || ContainsIgnoreCase(r.name, opt.videoDeviceName); });
    const Endpoint* render = nullptr;
    if (!opt.renderDevice.empty()) {
        render = FindByName(renders, opt.renderDevice);
    } else {
        render = FindFirst(renders, [](const Endpoint& e) { return e.isDefault; });
        if (!render && !renders.empty()) render = &renders.front();
    }
    if (!render) {
        if (verbose) LOGW("audio: no usable render endpoint matching \"%s\" (the capture device's own outputs are excluded)",
                          opt.renderDevice.empty() ? "default" : WideToUtf8(opt.renderDevice.c_str()).c_str());
        return false;
    }

    captureId = capture->id;
    renderId = render->id;
    UpdateStats([&](AudioStats& s) {
        s.captureName = WideToUtf8(capture->name.c_str());
        s.renderName = WideToUtf8(render->name.c_str());
    });
    if (verbose) LOGI("audio: capture \"%s\" (matched by %s) -> render \"%s\"%s", WideToUtf8(capture->name.c_str()).c_str(), method.c_str(),
                      WideToUtf8(render->name.c_str()).c_str(), render->isDefault ? " (default)" : "");
    return true;
}

namespace {

struct Chunk { uint64_t start; double t; };

struct SharedStream {
    ComPtr<IAudioClient> client;
    uint32_t periodFrames = 0;
};

HRESULT InitShared(IMMDevice* dev, const WAVEFORMATEX* wf, DWORD flags, REFERENCE_TIME legacyBufferHns, SharedStream& out) {
    ComPtr<IAudioClient3> lowLatency;
    UINT32 defaultPeriod = 0, fundamentalPeriod = 0, minPeriod = 0, maxPeriod = 0;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &lowLatency)) &&
        SUCCEEDED(lowLatency->GetSharedModeEnginePeriod(wf, &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod)) &&
        SUCCEEDED(lowLatency->InitializeSharedAudioStream(flags, minPeriod, wf, nullptr))) {
        out.periodFrames = minPeriod;
        WAVEFORMATEX* engineFormat = nullptr;
        UINT32 enginePeriod = 0;
        if (SUCCEEDED(lowLatency->GetCurrentSharedModeEnginePeriod(&engineFormat, &enginePeriod))) {
            out.periodFrames = (uint32_t)std::llround((double)enginePeriod * wf->nSamplesPerSec / engineFormat->nSamplesPerSec);
            CoTaskMemFree(engineFormat);
        }
        out.client = lowLatency;
        return S_OK;
    }
    ComPtr<IAudioClient> legacy;
    HRESULT hr;
    if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &legacy))) return hr;
    if (FAILED(hr = legacy->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, legacyBufferHns, 0, wf, nullptr))) return hr;
    REFERENCE_TIME periodHns = 0;
    if (FAILED(hr = legacy->GetDevicePeriod(&periodHns, nullptr))) return hr;
    out.periodFrames = (uint32_t)std::llround(periodHns / kHnsPerSecond * wf->nSamplesPerSec);
    out.client = legacy;
    return S_OK;
}

struct Incident {
    uint64_t count = 0, logged = 0;
    double lastLogT = -kIncidentLogIntervalSec;
    bool Note(double now) {
        ++count;
        if (now - lastLogT < kIncidentLogIntervalSec) return false;
        lastLogT = now;
        return true;
    }
    unsigned long long Total() const { return count; }
    unsigned long long Fresh() { const uint64_t d = count - logged; logged = count; return d; }
};

class Engine {
public:
    explicit Engine(AudioImpl& a) : a_(a) {}
    void Run();

private:
    struct Session {
        ComPtr<IAudioClient> captureClient, renderClient;
        ComPtr<IAudioCaptureClient> capture;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioClockAdjustment> clockAdjust;
        HANDLE captureEvent = nullptr, renderEvent = nullptr;
        bool captureStarted = false, renderStarted = false;
        WaveFormat captureFormat;
        uint32_t rate = 0, channels = 0;
        uint32_t capturePeriod = 0, renderPeriod = 0, renderPadTarget = 0;
        double renderLatencySec = 0;

        std::vector<float> ring;
        uint64_t ringFrames = 0, writePos = 0, readPos = 0;
        std::vector<Chunk> chunks;
        size_t chunkHead = 0, chunkCount = 0;
        double nextCaptureT = 0;
        bool firstPacket = true;
        double tonePhase = 0;

        bool playing = false;
        double playStart = 0, firstPlay = -1;
        double lastRenderT = 0, lastRenderEventT = 0, lastServoT = 0;
        double smoothedBufferSec = -1;
        double integralPpm = 0, trimPpm = 0, appliedPpm = 0;
        uint64_t pendingSilence = 0;
        double syncMs = 0, syncVelocity = 0, desiredSyncMs = 0;
        bool syncStarted = false;
        float gain = 0;
    };

    HRESULT Open();
    HRESULT OpenCapture(IMMDevice* dev);
    HRESULT OpenRender(IMMDevice* dev);
    void Close();
    HRESULT Stream();
    HRESULT DrainCapture(double now);
    HRESULT ServiceRender(double now);
    bool StartPlaybackIfPrimed(double now, UINT32 padding, UINT32 want);
    UINT32 WritePendingSilence(float* out, UINT32 want);
    void FillAfterUnderrun(float* dst, UINT32 need, double now);
    void FillSteady(float* dst, UINT32 need, uint64_t queued, double now, double eventGapSec);
    HRESULT Servo(double now);
    void UpdateSync(double now, double dt);
    void StepSync(double newSyncMs);
    void DecayUnderrunGuard(double now);
    void Publish(double now);

    double MaxSyncMs() const { return kAutoSyncMaxMs + a_.opt.offsetMs; }
    double TargetSec() const {
        return (double)(s_.capturePeriod + s_.renderPadTarget) / s_.rate + (a_.opt.bufferMs + guardMs_ + s_.syncMs) / 1000.0;
    }
    double BufferedSec(double now, uint64_t queuedFrames) { return now - CaptureTime(s_.readPos, now) + (double)queuedFrames / s_.rate; }
    uint64_t Buffered() const { return s_.writePos - s_.readPos; }
    void DropFrames(uint64_t n) { s_.readPos += std::min<uint64_t>(n, Buffered()); }
    void PushChunk(uint64_t start, double t);
    double CaptureTime(uint64_t pos, double now);
    void WriteSegment(float* dst, const BYTE* src, uint32_t n, bool silent);
    void WriteRing(const BYTE* src, uint32_t n, bool silent);
    void ReadRing(float* dst, uint32_t n);
    uint64_t DropCapturedBefore(double cutT, uint64_t maxDrop, double now);
    void FadeOut(float* frames, uint32_t n);
    void RampGain(float* p, uint32_t frames);

    AudioImpl& a_;
    Session s_;
    double guardMs_ = 0, lastGuardT_ = 0;
    double lastPublishT_ = 0;
    const char* failReason_ = nullptr;
    Incident underruns_, renderStarved_, excessDrops_, ringOverflows_;
};

void Engine::Run() {
    int failures = 0;
    while (!a_.StopRequested()) {
        failReason_ = nullptr;
        HRESULT hr = Open();
        const bool opened = SUCCEEDED(hr);
        if (opened) {
            const double start = NowSec();
            hr = Stream();
            if (NowSec() - start >= kStableSessionSec) failures = 0;
        }
        Close();
        if (a_.StopRequested()) break;
        failures++;
        if (failures <= kFailuresLoggedFirst || failures % kFailureLogEvery == 0)
            LOGW("audio: %s: %s; retrying every %.0f s (failure %d)", opened ? "stream error" : "cannot open devices",
                 failReason_ ? failReason_ : HrToString(hr).c_str(), kRetrySec, failures);
        if (WaitForSingleObject(a_.stopEvent, (DWORD)(kRetrySec * 1000)) == WAIT_OBJECT_0) break;
    }
}

HRESULT Engine::Open() {
    auto activeDevice = [&](const std::wstring& id, ComPtr<IMMDevice>& d) {
        d.Reset();
        DWORD state = 0;
        return SUCCEEDED(a_.devices->GetDevice(id.c_str(), &d)) && SUCCEEDED(d->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
    };
    ComPtr<IMMDevice> captureDev, renderDev;
    if (!activeDevice(a_.captureId, captureDev) || !activeDevice(a_.renderId, renderDev)) {
        if (!a_.Resolve(false) || !activeDevice(a_.captureId, captureDev) || !activeDevice(a_.renderId, renderDev))
            return AUDCLNT_E_DEVICE_INVALIDATED;
        LOGI("audio: endpoints re-resolved");
    }
    HRESULT hr;
    if (FAILED(hr = OpenCapture(captureDev.Get()))) return hr;
    if (FAILED(hr = OpenRender(renderDev.Get()))) return hr;

    s_.ringFrames = (uint64_t)(s_.rate * (kRingHeadroomSec + (a_.opt.bufferMs + MaxSyncMs() + kUnderrunGuardMaxMs) / 1000.0));
    s_.ring.assign((size_t)(s_.ringFrames * s_.channels), 0.0f);
    s_.chunks.assign(kChunkSlots, Chunk{0, 0});

    BYTE* buf = nullptr;
    if (FAILED(hr = s_.render->GetBuffer(s_.renderPadTarget, &buf))) return hr;
    if (FAILED(hr = s_.render->ReleaseBuffer(s_.renderPadTarget, AUDCLNT_BUFFERFLAGS_SILENT))) return hr;
    if (FAILED(hr = s_.renderClient->Start())) return hr;
    s_.renderStarted = true;
    if (FAILED(hr = s_.captureClient->Start())) return hr;
    s_.captureStarted = true;

    LOGI("audio: streaming %u Hz %u ch (capture %s, period %.2f ms); render period %.2f ms, pad target %u, "
         "stream latency %.1f ms; buffer target %.1f ms",
         s_.rate, s_.channels, SampleKindName(s_.captureFormat.kind), 1000.0 * s_.capturePeriod / s_.rate,
         1000.0 * s_.renderPeriod / s_.rate, s_.renderPadTarget, s_.renderLatencySec * 1000.0, TargetSec() * 1000.0);
    return S_OK;
}

HRESULT Engine::OpenCapture(IMMDevice* dev) {
    HRESULT hr;
    WaveFormatPtr mix;
    {
        ComPtr<IAudioClient> probe;
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe))) return hr;
        if (FAILED(hr = probe->GetMixFormat(&wf))) return hr;
        mix.reset(wf);
    }
    s_.captureFormat = ParseFormat(mix.get());
    if (s_.captureFormat.kind == SampleKind::Unknown) {
        LOGW("audio: unsupported capture format (tag 0x%x, %u bits)", mix->wFormatTag, mix->wBitsPerSample);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    s_.rate = s_.captureFormat.rate;
    s_.channels = s_.captureFormat.channels;
    SharedStream stream;
    if (FAILED(hr = InitShared(dev, mix.get(), AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kLegacyCaptureBufferHns, stream))) return hr;
    s_.captureClient = stream.client;
    s_.capturePeriod = stream.periodFrames;
    s_.captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_.captureEvent) return HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr = s_.captureClient->SetEventHandle(s_.captureEvent))) return hr;
    return s_.captureClient->GetService(IID_PPV_ARGS(&s_.capture));
}

HRESULT Engine::OpenRender(IMMDevice* dev) {
    const WAVEFORMATEXTENSIBLE fmt = FloatFormat(s_.rate, s_.channels, s_.captureFormat.channelMask);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_RATEADJUST;
    HRESULT hr;
    SharedStream stream;
    if (FAILED(hr = InitShared(dev, &fmt.Format, flags, kLegacyRenderBufferHns, stream))) return hr;
    s_.renderClient = stream.client;
    s_.renderPeriod = stream.periodFrames;
    if (FAILED(hr = s_.renderClient->GetService(IID_PPV_ARGS(&s_.clockAdjust)))) return hr;
    if (FAILED(hr = s_.clockAdjust->SetSampleRate((float)s_.rate))) return hr;
    UINT32 bufferFrames = 0;
    if (FAILED(hr = s_.renderClient->GetBufferSize(&bufferFrames))) return hr;
    REFERENCE_TIME reportedLatencyHns = 0;
    s_.renderClient->GetStreamLatency(&reportedLatencyHns);
    const double enginePeriodSec = (double)s_.renderPeriod / s_.rate;
    s_.renderLatencySec = std::max(reportedLatencyHns / kHnsPerSecond, enginePeriodSec);
    const uint32_t margin = std::max<uint32_t>(s_.renderPeriod / 2, (uint32_t)(s_.rate * kRenderMarginMinSec));
    s_.renderPadTarget = std::min<uint32_t>(bufferFrames, s_.renderPeriod + margin);
    s_.renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_.renderEvent) return HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr = s_.renderClient->SetEventHandle(s_.renderEvent))) return hr;
    return s_.renderClient->GetService(IID_PPV_ARGS(&s_.render));
}

void Engine::Close() {
    a_.UpdateStats([](AudioStats& s) { s.streaming = false; s.playing = false; });
    if (s_.captureStarted) s_.captureClient->Stop();
    if (s_.renderStarted) s_.renderClient->Stop();
    const HANDLE captureEvent = s_.captureEvent, renderEvent = s_.renderEvent;
    s_ = Session{};
    if (captureEvent) CloseHandle(captureEvent);
    if (renderEvent) CloseHandle(renderEvent);
}

HRESULT Engine::Stream() {
    const HANDLE handles[] = {a_.stopEvent, s_.renderEvent, s_.captureEvent};
    s_.lastRenderEventT = NowSec();
    a_.UpdateStats([](AudioStats& s) { s.streaming = true; });
    for (;;) {
        const DWORD w = WaitForMultipleObjects((DWORD)std::size(handles), handles, FALSE, kWaitTimeoutMs);
        if (w == WAIT_OBJECT_0) return S_OK;
        if (w == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
        const double now = NowSec();
        HRESULT hr = DrainCapture(now);
        if (FAILED(hr)) return hr;
        if (w == WAIT_OBJECT_0 + 1) {
            s_.lastRenderEventT = now;
            if (FAILED(hr = ServiceRender(now))) return hr;
            if (FAILED(hr = Servo(now))) return hr;
        } else if (now - s_.lastRenderEventT > kRenderSilenceTimeoutSec) {
            failReason_ = "render endpoint stopped requesting data";
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        if (now - lastPublishT_ >= kPublishIntervalSec) {
            DecayUnderrunGuard(now);
            Publish(now);
        }
    }
}

void Engine::PushChunk(uint64_t start, double t) {
    if (s_.chunkCount == kChunkSlots) { s_.chunkHead = (s_.chunkHead + 1) & kChunkMask; s_.chunkCount--; }
    s_.chunks[(s_.chunkHead + s_.chunkCount) & kChunkMask] = Chunk{start, t};
    s_.chunkCount++;
}

double Engine::CaptureTime(uint64_t pos, double now) {
    if (s_.chunkCount == 0) return now;
    while (s_.chunkCount > 1 && s_.chunks[(s_.chunkHead + 1) & kChunkMask].start <= pos) {
        s_.chunkHead = (s_.chunkHead + 1) & kChunkMask;
        s_.chunkCount--;
    }
    const Chunk& c = s_.chunks[s_.chunkHead];
    return c.t + ((double)pos - (double)c.start) / s_.rate;
}

uint64_t Engine::DropCapturedBefore(double cutT, uint64_t maxDrop, double now) {
    uint64_t dropped = 0;
    while (dropped < maxDrop && Buffered() > 0) {
        const double t = CaptureTime(s_.readPos, now);
        if (t >= cutT) break;
        uint64_t n = (uint64_t)std::ceil((cutT - t) * s_.rate);
        if (s_.chunkCount > 1) n = std::min<uint64_t>(n, s_.chunks[(s_.chunkHead + 1) & kChunkMask].start - s_.readPos);
        n = std::clamp<uint64_t>(n, 1, maxDrop - dropped);
        DropFrames(n);
        dropped += n;
    }
    return dropped;
}

void Engine::WriteSegment(float* dst, const BYTE* src, uint32_t n, bool silent) {
    const uint32_t ch = s_.channels;
    if (a_.opt.testTone) {
        const double step = 2.0 * kPi * kToneHz / s_.rate;
        for (uint32_t f = 0; f < n; f++) {
            const float v = kToneAmplitude * (float)std::sin(s_.tonePhase);
            s_.tonePhase += step;
            if (s_.tonePhase > 2.0 * kPi) s_.tonePhase -= 2.0 * kPi;
            for (uint32_t c = 0; c < ch; c++) dst[(size_t)f * ch + c] = v;
        }
    } else if (silent) {
        memset(dst, 0, (size_t)n * ch * sizeof(float));
    } else {
        ConvertToFloat(src, dst, (size_t)n * ch, s_.captureFormat.kind);
    }
}

void Engine::WriteRing(const BYTE* src, uint32_t n, bool silent) {
    const uint64_t pos = s_.writePos % s_.ringFrames;
    const uint32_t first = (uint32_t)std::min<uint64_t>(n, s_.ringFrames - pos);
    WriteSegment(&s_.ring[(size_t)(pos * s_.channels)], src, first, silent);
    if (n > first) WriteSegment(&s_.ring[0], src + (size_t)first * s_.captureFormat.blockBytes, n - first, silent);
    s_.writePos += n;
}

void Engine::ReadRing(float* dst, uint32_t n) {
    const uint64_t pos = s_.readPos % s_.ringFrames;
    const uint32_t first = (uint32_t)std::min<uint64_t>(n, s_.ringFrames - pos);
    memcpy(dst, &s_.ring[(size_t)(pos * s_.channels)], (size_t)first * s_.channels * sizeof(float));
    if (n > first) memcpy(dst + (size_t)first * s_.channels, &s_.ring[0], (size_t)(n - first) * s_.channels * sizeof(float));
    s_.readPos += n;
}

HRESULT Engine::DrainCapture(double now) {
    UINT32 next = 0;
    HRESULT hr = s_.capture->GetNextPacketSize(&next);
    if (FAILED(hr)) return hr;
    while (next > 0) {
        BYTE* data = nullptr;
        UINT32 n = 0;
        DWORD flags = 0;
        UINT64 devicePos = 0, qpcPos = 0;
        hr = s_.capture->GetBuffer(&data, &n, &flags, &devicePos, &qpcPos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) return hr;
        if (n > 0) {
            const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            const double packetSec = (double)n / s_.rate;
            double t = (double)qpcPos / kHnsPerSecond;
            const bool timestampUsable = !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpcPos != 0 &&
                                         t <= now + kCaptureTimestampMaxAheadSec && t >= now - kCaptureTimestampMaxAgeSec;
            if (!timestampUsable) t = now - packetSec;
            if (!s_.firstPacket && !discontinuity && std::fabs(t - s_.nextCaptureT) < kCaptureTimestampSnapSec)
                t = s_.nextCaptureT + kCapturePllGain * (t - s_.nextCaptureT);
            s_.nextCaptureT = t + packetSec;
            s_.firstPacket = false;
            const uint64_t freeFrames = s_.ringFrames - Buffered();
            if (n > freeFrames) {
                DropFrames(n - freeFrames);
                if (ringOverflows_.Note(now))
                    LOGW("audio: capture ring full, dropped the oldest audio (%llu total, +%llu)", ringOverflows_.Total(), ringOverflows_.Fresh());
            }
            PushChunk(s_.writePos, t);
            WriteRing(data, n, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        }
        if (FAILED(hr = s_.capture->ReleaseBuffer(n))) return hr;
        if (FAILED(hr = s_.capture->GetNextPacketSize(&next))) return hr;
    }
    return S_OK;
}

void Engine::FadeOut(float* frames, uint32_t n) {
    const uint32_t fade = std::min(n, kUnderrunFadeFrames);
    for (uint32_t k = 0; k < fade; k++) {
        const float g = (float)k / (float)fade;
        float* frame = frames + (size_t)(n - 1 - k) * s_.channels;
        for (uint32_t c = 0; c < s_.channels; c++) frame[c] *= g;
    }
}

void Engine::RampGain(float* p, uint32_t frames) {
    const bool silenced = a_.muted.load(std::memory_order_relaxed) || a_.paused.load(std::memory_order_relaxed);
    const float target = silenced ? 0.0f : a_.volume.load(std::memory_order_relaxed);
    const float from = s_.gain;
    const float maxStep = (float)frames / ((float)s_.rate * (float)kGainRampSec);
    const float to = from + std::clamp(target - from, -maxStep, maxStep);
    s_.gain = to;
    if (from == 1.0f && to == 1.0f) return;
    const uint32_t ch = s_.channels;
    const float step = (to - from) / (float)frames;
    for (uint32_t f = 0; f < frames; f++) {
        const float g = from + step * (float)f;
        for (uint32_t c = 0; c < ch; c++) p[(size_t)f * ch + c] *= g;
    }
}

HRESULT Engine::ServiceRender(double now) {
    UINT32 padding = 0;
    HRESULT hr = s_.renderClient->GetCurrentPadding(&padding);
    if (FAILED(hr)) return hr;
    const double eventGapSec = s_.lastRenderT > 0 ? std::min(now - s_.lastRenderT, kMaxRenderEventGapSec) : 0.0;
    s_.lastRenderT = now;
    if (s_.playing && padding == 0 && renderStarved_.Note(now))
        LOGW("audio: render buffer ran empty (%llu total, +%llu); thread woke late", renderStarved_.Total(), renderStarved_.Fresh());
    if (padding >= s_.renderPadTarget) return S_OK;
    const UINT32 want = s_.renderPadTarget - padding;
    BYTE* buf = nullptr;
    if (FAILED(hr = s_.render->GetBuffer(want, &buf))) return hr;
    float* out = reinterpret_cast<float*>(buf);
    if (!s_.playing && !StartPlaybackIfPrimed(now, padding, want)) return s_.render->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
    const UINT32 silence = WritePendingSilence(out, want);
    const UINT32 need = want - silence;
    float* dst = out + (size_t)silence * s_.channels;
    if (need > 0) {
        if (Buffered() < need) FillAfterUnderrun(dst, need, now);
        else FillSteady(dst, need, padding + silence, now, eventGapSec);
    }
    RampGain(out, want);
    return s_.render->ReleaseBuffer(want, 0);
}

bool Engine::StartPlaybackIfPrimed(double now, UINT32 padding, UINT32 want) {
    if (Buffered() == 0) return false;
    const double target = TargetSec();
    const double buffered = BufferedSec(now, padding);
    if (buffered < target) return false;
    if (buffered > target + kPrimeExcessDropSec) DropCapturedBefore(now + (double)padding / s_.rate - target, Buffered(), now);
    if (Buffered() < want) return false;
    s_.playing = true;
    s_.playStart = now;
    if (s_.firstPlay < 0) s_.firstPlay = now;
    s_.gain = 0.0f;
    s_.smoothedBufferSec = -1;
    return true;
}

UINT32 Engine::WritePendingSilence(float* out, UINT32 want) {
    const UINT32 silence = (UINT32)std::min<uint64_t>(s_.pendingSilence, want);
    memset(out, 0, (size_t)silence * s_.channels * sizeof(float));
    s_.pendingSilence -= silence;
    return silence;
}

void Engine::FillAfterUnderrun(float* dst, UINT32 need, double now) {
    const uint32_t n = (uint32_t)Buffered();
    ReadRing(dst, n);
    FadeOut(dst, n);
    memset(dst + (size_t)n * s_.channels, 0, (size_t)(need - n) * s_.channels * sizeof(float));
    guardMs_ = std::min(guardMs_ + kUnderrunGuardStepMs, kUnderrunGuardMaxMs);
    lastGuardT_ = now;
    s_.playing = false;
    if (underruns_.Note(now))
        LOGW("audio: underrun (capture data late or missing; %llu total, +%llu), re-priming to %.1f ms", underruns_.Total(), underruns_.Fresh(),
             TargetSec() * 1000.0);
}

void Engine::FillSteady(float* dst, UINT32 need, uint64_t queued, double now, double eventGapSec) {
    const double target = TargetSec();
    double buffered = BufferedSec(now, queued);
    if (buffered > target + kExcessDropSec) {
        const uint64_t dropped = DropCapturedBefore(now + (double)queued / s_.rate - target, Buffered() - need, now);
        buffered = BufferedSec(now, queued);
        s_.smoothedBufferSec = buffered;
        if (excessDrops_.Note(now))
            LOGW("audio: overflow (%llu total, +%llu), dropped %.1f ms", excessDrops_.Total(), excessDrops_.Fresh(), 1000.0 * dropped / s_.rate);
    }
    if (s_.smoothedBufferSec < 0) s_.smoothedBufferSec = buffered;
    else s_.smoothedBufferSec += (buffered - s_.smoothedBufferSec) * (1.0 - std::exp(-eventGapSec / kFillSmoothingSec));
    ReadRing(dst, need);
}

void Engine::UpdateSync(double now, double dt) {
    const double videoMs = a_.videoLatencyMs.load(std::memory_order_relaxed);
    const bool awaitsVideo = a_.opt.autoSync && videoMs <= 0;
    double desired = a_.opt.offsetMs;
    if (a_.opt.autoSync && videoMs > 0) {
        const double baseLatencyMs = TargetSec() * 1000.0 - s_.syncMs + s_.renderLatencySec * 1000.0;
        desired += videoMs - baseLatencyMs;
    }
    desired = std::clamp(desired, 0.0, MaxSyncMs());
    if (!s_.syncStarted && !awaitsVideo) {
        if (!s_.playing || now - s_.playStart <= kSyncHoldSec) return;
        s_.syncStarted = true;
        s_.desiredSyncMs = desired;
        if (now - s_.firstPlay < kSyncStepWindowSec) {
            StepSync(desired);
            return;
        }
    }
    s_.desiredSyncMs += (desired - s_.desiredSyncMs) * (1.0 - std::exp(-dt / kSyncTargetSmoothingSec));
    const double error = s_.desiredSyncMs - s_.syncMs;
    const double maxVelocityStep = kSyncAccelMsPerSec2 * dt;
    if (std::fabs(error) < kSyncSettledMs && std::fabs(s_.syncVelocity) < maxVelocityStep) { s_.syncVelocity = 0; return; }
    const double wantVelocity = std::clamp(std::copysign(std::sqrt(2.0 * kSyncAccelMsPerSec2 * std::fabs(error)), error),
                                           -kSyncSlewMsPerSec, kSyncSlewMsPerSec);
    s_.syncVelocity += std::clamp(wantVelocity - s_.syncVelocity, -maxVelocityStep, maxVelocityStep);
    s_.syncMs = std::clamp(s_.syncMs + s_.syncVelocity * dt, 0.0, MaxSyncMs());
}

void Engine::StepSync(double newSyncMs) {
    const double deltaMs = newSyncMs - s_.syncMs;
    s_.syncMs = newSyncMs;
    s_.syncVelocity = 0;
    if (deltaMs > 0) {
        s_.pendingSilence += (uint64_t)(deltaMs / 1000.0 * s_.rate);
        if (s_.smoothedBufferSec >= 0) s_.smoothedBufferSec += deltaMs / 1000.0;
    } else if (deltaMs < 0) {
        const uint64_t want = (uint64_t)(-deltaMs / 1000.0 * s_.rate);
        const uint64_t spare = Buffered() > s_.renderPadTarget ? Buffered() - s_.renderPadTarget : 0;
        const uint64_t drop = std::min(want, spare);
        DropFrames(drop);
        if (s_.smoothedBufferSec >= 0) s_.smoothedBufferSec -= (double)drop / s_.rate;
    }
    if (std::fabs(deltaMs) > kSyncStepLogMinMs) LOGI("audio: sync delay %.1f ms (start-up step)", s_.syncMs);
}

HRESULT Engine::Servo(double now) {
    if (s_.lastServoT == 0) { s_.lastServoT = now; return S_OK; }
    const double elapsed = now - s_.lastServoT;
    if (elapsed < kServoIntervalSec) return S_OK;
    s_.lastServoT = now;
    const double dt = std::min(elapsed, kServoMaxStepSec);
    UpdateSync(now, dt);
    if (!s_.playing || s_.smoothedBufferSec < 0) return S_OK;
    const double feedForwardPpm = -s_.syncVelocity * kPpmPerSyncMsPerSec;
    const double errorMs = (s_.smoothedBufferSec - TargetSec()) * 1000.0;
    const double unclampedPpm = kServoKpPpmPerMs * errorMs + s_.integralPpm + feedForwardPpm;
    const bool saturatedTowardError = (unclampedPpm >= kMaxTrimPpm && errorMs > 0) || (unclampedPpm <= -kMaxTrimPpm && errorMs < 0);
    if (!saturatedTowardError)
        s_.integralPpm = std::clamp(s_.integralPpm + kServoKiPpmPerMsSec * errorMs * dt, -kMaxIntegralPpm, kMaxIntegralPpm);
    const double wantPpm = std::clamp(kServoKpPpmPerMs * errorMs + s_.integralPpm + feedForwardPpm, -kMaxTrimPpm, kMaxTrimPpm);
    const double maxStepPpm = kTrimSlewPpmPerSec * dt;
    s_.trimPpm += std::clamp(wantPpm - s_.trimPpm, -maxStepPpm, maxStepPpm);
    if (std::fabs(s_.trimPpm - s_.appliedPpm) < kMinTrimChangePpm) return S_OK;
    s_.appliedPpm = s_.trimPpm;
    return s_.clockAdjust->SetSampleRate((float)(s_.rate * (1.0 + s_.trimPpm * 1e-6)));
}

void Engine::DecayUnderrunGuard(double now) {
    if (guardMs_ <= 0 || now - lastGuardT_ <= kUnderrunGuardDecayIntervalSec) return;
    guardMs_ = std::max(0.0, guardMs_ - kUnderrunGuardDecayMs);
    lastGuardT_ = now;
}

void Engine::Publish(double now) {
    lastPublishT_ = now;
    a_.UpdateStats([&](AudioStats& s) {
        s.playing = s_.playing;
        if (s_.smoothedBufferSec >= 0) {
            s.bufferMs = s_.smoothedBufferSec * 1000.0;
            s.latencyMs = (s_.smoothedBufferSec + s_.renderLatencySec) * 1000.0;
        }
        s.syncDelayMs = s_.syncMs;
        s.underruns = underruns_.count;
    });
}

}

void AudioImpl::ThreadMain() {
    SetThreadDescription(GetCurrentThread(), kAudioThreadName);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    else LOGW("audio: MMCSS registration failed (%lu)", GetLastError());
    Engine(*this).Run();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(comInit)) CoUninitialize();
}

AudioMonitor::AudioMonitor() : p_(std::make_unique<AudioImpl>()) {}
AudioMonitor::~AudioMonitor() { Stop(); }

void AudioMonitor::ListDevices() {
    const ComPtr<IMMDeviceEnumerator> devices = DeviceEnumerator();
    if (!devices) return;
    for (EDataFlow flow : {eCapture, eRender}) {
        const std::vector<Endpoint> endpoints = EnumEndpoints(devices.Get(), flow, true);
        LOGI("audio %s endpoints: %zu", flow == eCapture ? "capture" : "render", endpoints.size());
        for (const Endpoint& e : endpoints)
            LOGI("  %s %-40s %-22s container %s", e.isDefault ? "*" : " ", WideToUtf8(e.name.c_str()).c_str(), e.format.c_str(),
                 e.container ? GuidStr(*e.container).c_str() : "?");
    }
    LOGI("  (* = default)");
}

bool AudioMonitor::Start(const AudioOptions& o) {
    Stop();
    AudioImpl& a = *p_;
    a.opt = o;
    a.volume = o.volume;
    a.UpdateStats([](AudioStats& s) { s = {}; });
    a.devices = DeviceEnumerator();
    if (!a.devices || !a.Resolve(true)) return false;
    a.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!a.stopEvent) return false;
    a.thread = std::thread([&a] { a.ThreadMain(); });
    return true;
}

void AudioMonitor::Stop() {
    AudioImpl& a = *p_;
    if (a.thread.joinable()) {
        SetEvent(a.stopEvent);
        a.thread.join();
    }
    if (a.stopEvent) { CloseHandle(a.stopEvent); a.stopEvent = nullptr; }
}

bool AudioMonitor::Running() const { return p_->thread.joinable(); }
void AudioMonitor::SetMuted(bool muted) { p_->muted = muted; }
bool AudioMonitor::Muted() const { return p_->muted; }
void AudioMonitor::SetPaused(bool paused) { p_->paused = paused; }
void AudioMonitor::SetVolume(float volume) { p_->volume = volume; }
float AudioMonitor::Volume() const { return p_->volume; }
void AudioMonitor::SetVideoLatencyMs(double ms) { p_->videoLatencyMs = ms; }

AudioStats AudioMonitor::Stats() const {
    std::lock_guard<std::mutex> lock(p_->statsMutex);
    return p_->stats;
}

}
