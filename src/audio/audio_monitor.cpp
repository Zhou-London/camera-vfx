// HDMI audio monitor: WASAPI shared mode capture -> float ring -> WASAPI shared mode render,
// one MMCSS ("Pro Audio") thread, event driven on both endpoints.
//
// Timing model. Every captured packet carries the QPC time of its first frame (u64QPCPosition).
// The capture timeline is smoothed with a tiny PLL and kept per chunk next to the ring, so the
// capture time of any ring frame is known. At each render event (right after an engine pass) the
// frame R0 we are about to write will be played at
//     now + padding / rate (+ the render stream latency)
// so the time it spends buffered is D = now - capture(R0) + padding / rate. D is the buffered amount
// (ring + render padding + the capture packet in flight) measured continuously, i.e. independent of
// the phase between the capture and render packet cadences. A PI servo on smoothed D trims the render
// rate (IAudioClockAdjustment) so D stays at the target; latency = D + render stream latency.
#include "audio_monitor.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <thread>
#include <vector>

namespace camss {
namespace {

// Property keys defined locally (no initguid.h / ksguid.lib dependency, no ODR clashes).
const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kPkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const DEVPROPKEY kDevpkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const DEVPROPKEY kDevpkeyInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
// {00000000-0000-0000-FFFF-FFFFFFFFFFFF}: the container of everything built into the PC.
const GUID kComputerContainer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr double kPi = 3.14159265358979323846;

// Servo / policy constants.
constexpr double kServoKp = 400.0;        // ppm per ms of buffer error (2 ms -> 0.08 %, ~1.4 cents)
constexpr double kServoKi = 60.0;         // ppm per ms per second (closed loop ~3 s, damping ~0.8)
constexpr double kMaxPpm = 5000.0;        // +-0.5 % rate trim
constexpr double kMaxIntegPpm = 4000.0;
constexpr double kPpmSlewPerSec = 2000.0; // rate trim slew limit
constexpr double kFillTauSec = 0.5;       // smoothing of the buffer fill
constexpr double kOverflowSec = 0.060;    // fill above target + this -> drop the excess at once
constexpr double kSyncSlewMsPerSec = 4.0; // auto-sync delay change rate (servo trims <= 5 ms/s)
constexpr double kSyncAccMsPerSec2 = 1.5; // its acceleration (= 1500 ppm/s, below the trim slew limit)
constexpr double kSyncMaxMs = 250.0;
constexpr double kGuardStepMs = 3.0, kGuardMaxMs = 30.0, kGuardDecaySec = 30.0;
constexpr double kSyncJumpWindowSec = 10.0; // the first auto-sync value may jump (start-up only)
constexpr double kRetrySec = 2.0;
constexpr float kToneAmp = 0.01585f;      // -36 dBFS

struct ComScope {
    bool uninit = false;
    ComScope() { uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() { if (uninit) CoUninitialize(); }
};

struct CoMemFree { void operator()(void* p) const { CoTaskMemFree(p); } };
using WaveFmtPtr = std::unique_ptr<WAVEFORMATEX, CoMemFree>;

std::wstring Upper(std::wstring s) { for (auto& c : s) c = (wchar_t)std::towupper(c); return s; }
bool ContainsI(const std::wstring& hay, const std::wstring& needle) {
    return !needle.empty() && Upper(hay).find(Upper(needle)) != std::wstring::npos;
}
std::string U8(const std::wstring& w) { return WideToUtf8(w.c_str()); }
std::string GuidStr(const GUID& g) {
    wchar_t b[64] = {};
    StringFromGUID2(g, b, 64);
    return U8(b);
}

// ---------------------------------------------------------------- formats
enum class SampleKind { Unknown, F32, F64, I16, I24, I32 };

struct WaveFmt {
    SampleKind kind = SampleKind::Unknown;
    uint32_t rate = 0, channels = 0, block = 0, bits = 0;
    DWORD mask = 0;
};

WaveFmt ParseFormat(const WAVEFORMATEX* wf) {
    WaveFmt f;
    f.rate = wf->nSamplesPerSec; f.channels = wf->nChannels; f.block = wf->nBlockAlign; f.bits = wf->wBitsPerSample;
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.mask = x->dwChannelMask;
        // KSDATAFORMAT_SUBTYPE_xxx = {format tag}-0000-0010-8000-00AA00389B71
        const GUID& g = x->SubFormat;
        tag = (g.Data2 == 0 && g.Data3 == 0x10 && memcmp(g.Data4, kSubtypeFloat.Data4, 8) == 0) ? (WORD)g.Data1 : 0;
    }
    // Samples narrower than the container (e.g. 24 in 32) are left justified: convert by container.
    if (tag == WAVE_FORMAT_IEEE_FLOAT) f.kind = f.bits == 32 ? SampleKind::F32 : f.bits == 64 ? SampleKind::F64 : SampleKind::Unknown;
    else if (tag == WAVE_FORMAT_PCM) f.kind = f.bits == 16 ? SampleKind::I16 : f.bits == 24 ? SampleKind::I24 : f.bits == 32 ? SampleKind::I32 : SampleKind::Unknown;
    if (f.channels == 0 || f.block != f.channels * (f.bits / 8)) f.kind = SampleKind::Unknown;
    return f;
}

const char* KindName(SampleKind k) {
    switch (k) {
    case SampleKind::F32: return "float32";
    case SampleKind::F64: return "float64";
    case SampleKind::I16: return "pcm16";
    case SampleKind::I24: return "pcm24";
    case SampleKind::I32: return "pcm32";
    default: return "unsupported";
    }
}

std::string FormatDesc(const WAVEFORMATEX* wf) {
    WaveFmt f = ParseFormat(wf);
    char b[96];
    snprintf(b, sizeof(b), "%u Hz %u ch %s", f.rate, f.channels, f.kind == SampleKind::Unknown ? "?" : KindName(f.kind));
    return b;
}

void ConvertToFloat(const BYTE* src, float* dst, size_t samples, SampleKind k) {
    switch (k) {
    case SampleKind::F32: memcpy(dst, src, samples * sizeof(float)); break;
    case SampleKind::F64: { const double* s = reinterpret_cast<const double*>(src); for (size_t i = 0; i < samples; i++) dst[i] = (float)s[i]; break; }
    case SampleKind::I16: { const int16_t* s = reinterpret_cast<const int16_t*>(src); for (size_t i = 0; i < samples; i++) dst[i] = s[i] * (1.0f / 32768.0f); break; }
    case SampleKind::I24:
        for (size_t i = 0; i < samples; i++, src += 3) {
            const int32_t v = (int32_t)(((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24));
            dst[i] = (float)v * (1.0f / 2147483648.0f);
        }
        break;
    case SampleKind::I32: { const int32_t* s = reinterpret_cast<const int32_t*>(src); for (size_t i = 0; i < samples; i++) dst[i] = (float)s[i] * (1.0f / 2147483648.0f); break; }
    default: memset(dst, 0, samples * sizeof(float)); break;
    }
}

DWORD DefaultChannelMask(uint32_t ch) {
    switch (ch) {
    case 1: return 0x4;   // FC
    case 2: return 0x3;   // FL FR
    case 3: return 0x7;   // FL FR FC
    case 4: return 0x33;  // FL FR BL BR
    case 5: return 0x37;  // FL FR FC BL BR
    case 6: return 0x3F;  // 5.1
    case 8: return 0x63F; // 7.1
    default: return 0;
    }
}

// ---------------------------------------------------------------- endpoints
struct Endpoint {
    std::wstring id, name;
    GUID container{};
    bool hasContainer = false;
    bool isDefault = false;
    std::string format;
};

std::vector<Endpoint> EnumEndpoints(IMMDeviceEnumerator* en, EDataFlow flow, bool withFormat) {
    std::vector<Endpoint> out;
    std::wstring defId;
    {
        ComPtr<IMMDevice> d;
        LPWSTR id = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d)) && SUCCEEDED(d->GetId(&id))) { defId = id; CoTaskMemFree(id); }
    }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        Endpoint e;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id))) continue;
        e.id = id; CoTaskMemFree(id);
        e.isDefault = e.id == defId;
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) e.name = v.pwszVal;
            PropVariantClear(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) { e.container = *v.puuid; e.hasContainer = true; }
            PropVariantClear(&v);
        }
        if (withFormat) {
            ComPtr<IAudioClient> ac;
            WAVEFORMATEX* wf = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac)) && SUCCEEDED(ac->GetMixFormat(&wf))) {
                e.format = FormatDesc(wf);
                CoTaskMemFree(wf);
            } else {
                e.format = "mix format unavailable";
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

// ContainerId of the physical device behind an MF video capture symbolic link.
bool VideoContainerId(const std::wstring& link, GUID& out, const char** how) {
    if (link.empty()) return false;
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(GUID);
    if (CM_Get_Device_Interface_PropertyW(link.c_str(), &kDevpkeyContainerId, &type, (PBYTE)&out, &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_GUID) {
        *how = "interface";
        return true;
    }
    // Interface stores do not always carry it: go to the device node that exposes the interface.
    wchar_t inst[MAX_DEVICE_ID_LEN + 1] = {};
    size = sizeof(inst); type = 0;
    if (CM_Get_Device_Interface_PropertyW(link.c_str(), &kDevpkeyInstanceId, &type, (PBYTE)inst, &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return false;
    DEVINST di = 0;
    if (CM_Locate_DevNodeW(&di, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    size = sizeof(GUID); type = 0;
    if (CM_Get_DevNode_PropertyW(di, &kDevpkeyContainerId, &type, (PBYTE)&out, &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_GUID)
        return false;
    *how = "devnode";
    return true;
}

bool UsableContainer(const GUID& g) { return g != GUID_NULL && g != kComputerContainer; }

// Capture endpoints that are a copy of some render output: playing them back would feed back.
bool IsLoopbackName(const std::wstring& name) {
    static const wchar_t* kNames[] = {L"Stereo Mix", L"What U Hear", L"Wave Out Mix", L"Loopback", L"Rec. Playback",
                                      L"立体声混音" /* Chinese "Stereo Mix" */};
    for (const wchar_t* n : kNames)
        if (ContainsI(name, n)) return true;
    return false;
}

double NowSec() { return Qpc::ToSec(Qpc::Now()); }

} // namespace

// ---------------------------------------------------------------- shared state
struct AudioImpl {
    AudioOptions opt;
    std::thread thread;
    HANDLE stopEvt = nullptr;

    // controls
    std::atomic<bool> muted{false};
    std::atomic<float> volume{1.0f};
    std::atomic<double> videoLatencyMs{0.0};

    // endpoint selection: written by Start() before the thread exists, then only by the thread
    std::wstring capId, renId;

    // published stats (written by the audio thread ~10x per second)
    std::atomic<bool> streaming{false}, playing{false}, rateAdjust{false};
    std::atomic<uint32_t> rate{0}, channels{0};
    std::atomic<double> latencyMs{0}, bufferMs{0}, targetMs{0}, syncMs{0}, ppm{0};
    std::atomic<double> capPeriodMs{0}, renPeriodMs{0}, renLatencyMs{0};
    std::atomic<uint64_t> underruns{0}, overflows{0}, discontinuities{0}, glitches{0}, reopens{0};
    std::atomic<float> peak{0};
    mutable std::mutex nameMtx;
    std::string capName, renName, matchMethod;

    bool Resolve(bool verbose);
    void ThreadMain();
};

bool AudioImpl::Resolve(bool verbose) {
    ComScope com;
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) {
        if (verbose) LOGW("audio: MMDeviceEnumerator unavailable: %s", HrToString(hr).c_str());
        return false;
    }
    const std::vector<Endpoint> caps = EnumEndpoints(en.Get(), eCapture, false);
    const std::vector<Endpoint> rens = EnumEndpoints(en.Get(), eRender, false);

    const Endpoint* cap = nullptr;
    std::string method;
    if (!opt.captureDevice.empty()) {
        // first match, preferring one that is not a loopback ("Realtek" may match "Stereo Mix (Realtek)" first)
        for (const auto& e : caps)
            if (ContainsI(e.name, opt.captureDevice) && (!cap || (IsLoopbackName(cap->name) && !IsLoopbackName(e.name)))) {
                cap = &e;
                method = "name (explicit)";
            }
        if (!cap) {
            if (verbose) LOGW("audio: no active capture endpoint matches \"%s\" (%zu capture endpoints; --list shows them)",
                              U8(opt.captureDevice).c_str(), caps.size());
            return false;
        }
    } else {
        GUID vc{};
        const char* how = "";
        if (VideoContainerId(opt.videoSymbolicLink, vc, &how) && UsableContainer(vc)) {
            for (const auto& e : caps) {
                if (!e.hasContainer || e.container != vc) continue;
                // several endpoints in the same box: prefer the one named after the video device
                if (!cap || (ContainsI(e.name, opt.videoDeviceName) && !ContainsI(cap->name, opt.videoDeviceName))) cap = &e;
            }
            if (cap) method = std::string("ContainerId ") + GuidStr(vc) + " via " + how;
        } else if (verbose && !opt.videoSymbolicLink.empty()) {
            LOGI("audio: no ContainerId for the video device, falling back to a name match");
        }
        if (!cap && !opt.videoDeviceName.empty()) {
            for (const auto& e : caps)
                if (ContainsI(e.name, opt.videoDeviceName)) { cap = &e; method = "name (video device name)"; break; }
        }
        if (!cap) {
            if (verbose) LOGW("audio: no capture endpoint belongs to video device \"%s\" (%zu capture endpoints checked)",
                              U8(opt.videoDeviceName).c_str(), caps.size());
            return false;
        }
    }
    if (IsLoopbackName(cap->name)) {
        if (verbose) LOGW("audio: refusing capture endpoint \"%s\": it is a loopback of a render output (would feed back)", U8(cap->name).c_str());
        return false;
    }

    // Render: never the capture box itself (feedback / HDMI loop).
    auto sameBox = [&](const Endpoint& r) {
        if (cap->hasContainer && r.hasContainer && UsableContainer(cap->container) && r.container == cap->container) return true;
        return !opt.videoDeviceName.empty() && ContainsI(r.name, opt.videoDeviceName);
    };
    const Endpoint* ren = nullptr;
    if (!opt.renderDevice.empty()) {
        // first match, preferring one that is not the capture box ("HDMI" may match both)
        for (const auto& e : rens)
            if (ContainsI(e.name, opt.renderDevice) && (!ren || (sameBox(*ren) && !sameBox(e)))) ren = &e;
        if (!ren) {
            if (verbose) LOGW("audio: no active render endpoint matches \"%s\"", U8(opt.renderDevice).c_str());
            return false;
        }
        if (sameBox(*ren)) {
            if (verbose) LOGW("audio: refusing render endpoint \"%s\": it belongs to the capture device (would feed back)", U8(ren->name).c_str());
            return false;
        }
    } else {
        for (const auto& e : rens)
            if (e.isDefault && !sameBox(e)) ren = &e;
        if (!ren) {
            for (const auto& e : rens)
                if (!sameBox(e)) { ren = &e; break; }
            if (ren && verbose) LOGW("audio: default render endpoint belongs to the capture device; using \"%s\"", U8(ren->name).c_str());
        }
        if (!ren) {
            if (verbose) LOGW("audio: no usable render endpoint");
            return false;
        }
    }

    capId = cap->id;
    renId = ren->id;
    {
        std::lock_guard<std::mutex> lk(nameMtx);
        capName = U8(cap->name);
        renName = U8(ren->name);
        matchMethod = method;
    }
    if (verbose) LOGI("audio: capture \"%s\" (matched by %s) -> render \"%s\"%s", U8(cap->name).c_str(), method.c_str(),
                      U8(ren->name).c_str(), ren->isDefault ? " (default)" : "");
    return true;
}

// ---------------------------------------------------------------- streaming engine (audio thread only)
namespace {

struct Chunk { uint64_t start; double t; };

class Engine {
public:
    explicit Engine(AudioImpl& a) : a_(a) {}
    void Run();

private:
    struct Session {
        ComPtr<IAudioClient> capClient, renClient;
        ComPtr<IAudioCaptureClient> cap;
        ComPtr<IAudioRenderClient> ren;
        ComPtr<IAudioClockAdjustment> clockAdj;
        HANDLE capEvt = nullptr, renEvt = nullptr;
        bool capStarted = false, renStarted = false;
        bool capLowLat = false, renLowLat = false;
        WaveFmt cf;
        uint32_t rate = 0, ch = 0;
        uint32_t capPeriod = 0, renPeriod = 0, renBuf = 0, padTarget = 0;
        double renLatSec = 0;
    };

    HRESULT Open();
    HRESULT OpenCapture(IMMDevice* dev);
    HRESULT OpenRender(IMMDevice* dev);
    void Close();
    HRESULT Stream();
    HRESULT DrainCapture(double now);
    HRESULT ServiceRender(double now);
    void Servo(double now);
    void UpdateSync(double now, double dt);
    void StepSync(double newSyncMs);
    void Publish(double now);

    double TargetSec() const {
        return (double)s_.capPeriod / s_.rate + (double)s_.padTarget / s_.rate + (a_.opt.bufferMs + a_.opt.delayMs + guardMs_ + sync_) / 1000.0;
    }
    uint64_t Avail() const { return wAbs_ - rAbs_; }
    void PushChunk(uint64_t start, double t);
    double CaptureTime(uint64_t abs, double now);
    void WriteRing(const BYTE* src, uint32_t n, bool silent);
    void WriteSeg(float* dst, const BYTE* src, uint32_t n, bool silent);
    void ReadRing(float* dst, uint32_t n);
    void Drop(uint64_t n) { rAbs_ += std::min<uint64_t>(n, Avail()); }
    uint64_t DropCapturedBefore(double tCut, uint64_t maxDrop, double now);
    void ApplyGain(float* p, uint32_t frames);
    bool LogAllowed(double now, double& last) { if (now - last < 5.0) return false; last = now; return true; }

    AudioImpl& a_;
    Session s_;

    // ring of float interleaved frames + capture timeline
    std::vector<float> ring_;
    uint64_t ringCap_ = 0, wAbs_ = 0, rAbs_ = 0;
    std::vector<Chunk> chunks_;
    size_t chHead_ = 0, chCount_ = 0;
    double capNextT_ = 0;
    bool capFirst_ = true;
    double tonePhase_ = 0;

    // render / servo state
    bool playing_ = false;
    bool everPlayed_ = false;
    double playStart_ = 0, firstPlay_ = 0;
    double lastRenT_ = 0, lastServoT_ = 0, lastRenEvt_ = 0;
    double dSmooth_ = -1;
    double integ_ = 0, ppm_ = 0, ppmSet_ = 0;
    bool rateAdj_ = false;
    bool correcting_ = false;
    double lastFix_ = 0;
    int64_t fixNet_ = 0;
    double fixPpm_ = 0;
    uint64_t pendingSilence_ = 0;
    double sync_ = 0, desiredEma_ = 0;
    // Extra buffer learned from underruns (capture packets arriving late, e.g. while the USB card also
    // streams video): +3 ms per underrun up to 30 ms, given back at 1 ms per 30 s without one.
    double guardMs_ = 0, lastGuardT_ = 0;
    double syncVel_ = 0;   // ms per second
    bool syncInit_ = false;
    float gain_ = 0, peakAcc_ = 0, peakPub_ = 0;
    double lastPub_ = 0;
    double lastUnderLog_ = -1e9, lastOverLog_ = -1e9, lastGlitchLog_ = -1e9;
    const char* failWhy_ = nullptr;   // reason of the last session end when it is not a plain HRESULT
    // debug diagnostics over 5 s windows: raw fill range and the smallest ring headroom after a write
    double diagDMin_ = 1e9, diagDMax_ = 0, lastDiag_ = 0;
    uint64_t diagHeadMin_ = UINT64_MAX;
    uint64_t underLogged_ = 0, overLogged_ = 0, glitchLogged_ = 0;
};

void Engine::Run() {
    int failures = 0;
    while (WaitForSingleObject(a_.stopEvt, 0) != WAIT_OBJECT_0) {
        failWhy_ = nullptr;
        HRESULT hr = Open();
        const bool opened = SUCCEEDED(hr);
        if (opened) {
            const double t0 = NowSec();
            hr = Stream();
            // only a session that streamed for a while resets the warning budget: a device that opens and
            // fails again right away (flapping) must not log on every retry
            if (NowSec() - t0 >= 30.0) failures = 0;
        }
        Close();
        if (WaitForSingleObject(a_.stopEvt, 0) == WAIT_OBJECT_0) break;
        failures++;
        if (failures <= 3 || failures % 30 == 0)
            LOGW("audio: %s: %s; retrying every %.0f s (failure %d)", opened ? "stream error" : "cannot open devices",
                 failWhy_ ? failWhy_ : HrToString(hr).c_str(), kRetrySec, failures);
        a_.reopens.fetch_add(1, std::memory_order_relaxed);
        if (WaitForSingleObject(a_.stopEvt, (DWORD)(kRetrySec * 1000)) == WAIT_OBJECT_0) break;
    }
}

HRESULT Engine::Open() {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;
    auto get = [&](const std::wstring& id, ComPtr<IMMDevice>& d) {
        d.Reset();
        DWORD st = 0;
        return !id.empty() && SUCCEEDED(en->GetDevice(id.c_str(), &d)) && SUCCEEDED(d->GetState(&st)) && st == DEVICE_STATE_ACTIVE;
    };
    ComPtr<IMMDevice> capDev, renDev;
    if (!get(a_.capId, capDev) || !get(a_.renId, renDev)) {
        // endpoint gone (unplugged, ids changed): pick again with the original criteria
        if (!a_.Resolve(false) || !get(a_.capId, capDev) || !get(a_.renId, renDev)) return AUDCLNT_E_DEVICE_INVALIDATED;
        LOGI("audio: endpoints re-resolved");
    }
    if (FAILED(hr = OpenCapture(capDev.Get()))) return hr;
    if (FAILED(hr = OpenRender(renDev.Get()))) return hr;

    // ring: worst case target (sync 250 ms + options) with plenty of headroom
    ringCap_ = (uint64_t)(s_.rate * (1.5 + (a_.opt.bufferMs + a_.opt.delayMs + kSyncMaxMs + kGuardMaxMs) / 1000.0));
    ring_.assign((size_t)(ringCap_ * s_.ch), 0.0f);
    chunks_.assign(8192, Chunk{0, 0});
    chHead_ = chCount_ = 0;
    wAbs_ = rAbs_ = 0;
    capNextT_ = 0; capFirst_ = true; tonePhase_ = 0;
    playing_ = false; everPlayed_ = false;
    lastRenT_ = lastServoT_ = 0; dSmooth_ = -1;
    integ_ = ppm_ = ppmSet_ = 0;
    correcting_ = false; fixNet_ = 0; fixPpm_ = 0;
    pendingSilence_ = 0;
    sync_ = 0; syncVel_ = 0; desiredEma_ = 0; syncInit_ = false;
    gain_ = 0; peakAcc_ = peakPub_ = 0;

    // prime the render buffer with silence, then start
    BYTE* buf = nullptr;
    if (FAILED(hr = s_.ren->GetBuffer(s_.padTarget, &buf))) return hr;
    if (FAILED(hr = s_.ren->ReleaseBuffer(s_.padTarget, AUDCLNT_BUFFERFLAGS_SILENT))) return hr;
    if (FAILED(hr = s_.renClient->Start())) return hr;
    s_.renStarted = true;
    if (FAILED(hr = s_.capClient->Start())) return hr;
    s_.capStarted = true;

    a_.rate = s_.rate;
    a_.channels = s_.ch;
    a_.rateAdjust = rateAdj_;
    a_.capPeriodMs = 1000.0 * s_.capPeriod / s_.rate;
    a_.renPeriodMs = 1000.0 * s_.renPeriod / s_.rate;
    a_.renLatencyMs = s_.renLatSec * 1000.0;
    LOGI("audio: streaming %u Hz %u ch (capture %s, period %.2f ms%s); render period %.2f ms%s, buffer %u, pad target %u, "
         "stream latency %.1f ms, rate adjust %s; buffer target %.1f ms",
         s_.rate, s_.ch, KindName(s_.cf.kind), 1000.0 * s_.capPeriod / s_.rate, s_.capLowLat ? " low-latency" : "",
         1000.0 * s_.renPeriod / s_.rate, s_.renLowLat ? " low-latency" : "", s_.renBuf, s_.padTarget, s_.renLatSec * 1000.0,
         rateAdj_ ? "yes" : "no (frame drop/insert)", TargetSec() * 1000.0);
    return S_OK;
}

HRESULT Engine::OpenCapture(IMMDevice* dev) {
    HRESULT hr;
    WaveFmtPtr mix;
    {
        ComPtr<IAudioClient3> c3;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3))) {
            WAVEFORMATEX* wf = nullptr;
            if (FAILED(hr = c3->GetMixFormat(&wf))) return hr;
            mix.reset(wf);
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix.get(), &def, &fund, &mn, &mx)) &&
                SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix.get(), nullptr))) {
                WAVEFORMATEX* ef = nullptr;
                UINT32 frames = 0;
                if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&ef, &frames)) && ef) {
                    s_.capPeriod = (uint32_t)std::llround((double)frames * mix->nSamplesPerSec / ef->nSamplesPerSec);
                    CoTaskMemFree(ef);
                } else {
                    s_.capPeriod = mn;
                }
                s_.capClient = c3;
                s_.capLowLat = true;
            }
        }
    }
    if (!s_.capClient) {
        ComPtr<IAudioClient> c;
        if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c))) return hr;
        if (!mix) {
            WAVEFORMATEX* wf = nullptr;
            if (FAILED(hr = c->GetMixFormat(&wf))) return hr;
            mix.reset(wf);
        }
        // shared mode: the buffer size is only a capacity for capture, it adds no latency
        if (FAILED(hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, mix.get(), nullptr))) return hr;
        REFERENCE_TIME def = 0;
        c->GetDevicePeriod(&def, nullptr);
        s_.capPeriod = (uint32_t)std::llround(def * 1e-7 * mix->nSamplesPerSec);
        s_.capClient = c;
    }
    s_.cf = ParseFormat(mix.get());
    if (s_.cf.kind == SampleKind::Unknown) {
        LOGW("audio: unsupported capture format (tag 0x%x, %u bits)", mix->wFormatTag, mix->wBitsPerSample);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    s_.rate = s_.cf.rate;
    s_.ch = s_.cf.channels;
    if (s_.capPeriod == 0) s_.capPeriod = s_.rate / 100;
    s_.capEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_.capEvt) return HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr = s_.capClient->SetEventHandle(s_.capEvt))) return hr;
    return s_.capClient->GetService(IID_PPV_ARGS(&s_.cap));
}

HRESULT Engine::OpenRender(IMMDevice* dev) {
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)s_.ch;
    fmt.Format.nSamplesPerSec = s_.rate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = (WORD)(4 * s_.ch);
    fmt.Format.nAvgBytesPerSec = s_.rate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = s_.cf.mask ? s_.cf.mask : DefaultChannelMask(s_.ch);
    fmt.SubFormat = kSubtypeFloat;
    const WAVEFORMATEX* wf = &fmt.Format;

    const DWORD base = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    struct Attempt { bool v3, rateAdjust; };
    // rate adjust is worth more than the low-latency period: try it first on both paths
    const Attempt attempts[] = {{true, true}, {false, true}, {true, false}, {false, false}};
    HRESULT hr = E_FAIL;
    for (const Attempt& at : attempts) {
        if (at.rateAdjust && a_.opt.forceDropInsert) continue;
        const DWORD flags = base | (at.rateAdjust ? AUDCLNT_STREAMFLAGS_RATEADJUST : 0);
        auto failed = [&](const char* step) {
            LOGD("audio: render %s%s: %s failed: %s", at.v3 ? "IAudioClient3" : "IAudioClient", at.rateAdjust ? " +rateadjust" : "",
                 step, HrToString(hr).c_str());
        };
        if (at.v3) {
            ComPtr<IAudioClient3> c3;
            if (FAILED(hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3))) { failed("Activate"); continue; }
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (FAILED(hr = c3->GetSharedModeEnginePeriod(wf, &def, &fund, &mn, &mx))) { failed("GetSharedModeEnginePeriod"); continue; }
            LOGD("audio: render engine periods (frames): default %u fundamental %u min %u max %u", def, fund, mn, mx);
            if (FAILED(hr = c3->InitializeSharedAudioStream(flags, mn, wf, nullptr))) { failed("InitializeSharedAudioStream"); continue; }
            WAVEFORMATEX* ef = nullptr;
            UINT32 frames = 0;
            if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&ef, &frames)) && ef) {
                s_.renPeriod = (uint32_t)std::llround((double)frames * s_.rate / ef->nSamplesPerSec);
                CoTaskMemFree(ef);
            } else {
                s_.renPeriod = mn;
            }
            s_.renClient = c3;
            s_.renLowLat = true;
        } else {
            ComPtr<IAudioClient> c;
            if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c))) { failed("Activate"); continue; }
            // 50 ms capacity; we only keep ~1.5 periods queued in it
            if (FAILED(hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 500000, 0, wf, nullptr))) { failed("Initialize"); continue; }
            REFERENCE_TIME def = 0;
            c->GetDevicePeriod(&def, nullptr);
            s_.renPeriod = (uint32_t)std::llround(def * 1e-7 * s_.rate);
            s_.renClient = c;
        }
        rateAdj_ = at.rateAdjust;
        break;
    }
    if (!s_.renClient) return hr;
    if (rateAdj_) {
        if (FAILED(s_.renClient->GetService(IID_PPV_ARGS(&s_.clockAdj))) || FAILED(s_.clockAdj->SetSampleRate((float)s_.rate))) {
            s_.clockAdj.Reset();
            rateAdj_ = false;
        }
    }
    if (s_.renPeriod == 0) s_.renPeriod = s_.rate / 100;
    UINT32 bufFrames = 0;
    if (FAILED(hr = s_.renClient->GetBufferSize(&bufFrames))) return hr;
    s_.renBuf = bufFrames;
    REFERENCE_TIME lat = 0;
    s_.renClient->GetStreamLatency(&lat);
    // Shared mode streams often report 0 here although the engine always holds at least one
    // period between our buffer and the device: use that as the floor of the estimate.
    s_.renLatSec = std::max(lat * 1e-7, (double)s_.renPeriod / s_.rate);
    LOGD("audio: render GetStreamLatency %.2f ms, using %.2f ms", lat * 1e-4, s_.renLatSec * 1000.0);
    // keep ~1.5 periods queued: the engine pulls one period per pass; the rest is margin for SRC/rate trim
    const uint32_t margin = std::max<uint32_t>(s_.renPeriod / 2, (uint32_t)(s_.rate * 0.0015));
    s_.padTarget = std::min<uint32_t>(s_.renBuf, s_.renPeriod + margin);
    s_.renEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_.renEvt) return HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr = s_.renClient->SetEventHandle(s_.renEvt))) return hr;
    return s_.renClient->GetService(IID_PPV_ARGS(&s_.ren));
}

void Engine::Close() {
    a_.streaming = false;
    a_.playing = false;
    if (s_.capStarted) s_.capClient->Stop();
    if (s_.renStarted) s_.renClient->Stop();
    HANDLE ce = s_.capEvt, re = s_.renEvt;
    s_ = Session{};
    if (ce) CloseHandle(ce);
    if (re) CloseHandle(re);
}

HRESULT Engine::Stream() {
    HANDLE hs[3] = {a_.stopEvt, s_.capEvt, s_.renEvt};
    lastRenEvt_ = NowSec();
    a_.streaming = true;
    for (;;) {
        const DWORD w = WaitForMultipleObjects(3, hs, FALSE, 100);
        if (w == WAIT_OBJECT_0) return S_OK;
        if (w == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
        const double now = NowSec();
        // drain capture on every wake-up: device errors surface even if its events stop
        HRESULT hr = DrainCapture(now);
        if (FAILED(hr)) return hr;
        bool renderDue = w == WAIT_OBJECT_0 + 2;
        // WaitForMultipleObjects reports the lowest signalled index: after a stall the capture event
        // wins, so poll the render event before concluding that the render endpoint went quiet
        if (!renderDue && now - lastRenEvt_ > 0.5) renderDue = WaitForSingleObject(s_.renEvt, 0) == WAIT_OBJECT_0;
        if (renderDue) {
            lastRenEvt_ = now;
            if (FAILED(hr = ServiceRender(now))) return hr;
            Servo(now);
        } else if (now - lastRenEvt_ > 2.0) {
            failWhy_ = "render endpoint stopped requesting data";   // logged (rate-limited) by Run()
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        if (now - lastPub_ >= 0.1) Publish(now);
    }
}

void Engine::PushChunk(uint64_t start, double t) {
    const size_t mask = chunks_.size() - 1;
    if (chCount_ == chunks_.size()) { chHead_ = (chHead_ + 1) & mask; chCount_--; }
    chunks_[(chHead_ + chCount_) & mask] = Chunk{start, t};
    chCount_++;
}

double Engine::CaptureTime(uint64_t abs, double now) {
    if (chCount_ == 0) return now;
    const size_t mask = chunks_.size() - 1;
    while (chCount_ > 1 && chunks_[(chHead_ + 1) & mask].start <= abs) { chHead_ = (chHead_ + 1) & mask; chCount_--; }
    const Chunk& c = chunks_[chHead_];
    return c.t + ((double)abs - (double)c.start) / s_.rate;
}

// Drops ring frames captured before tCut (at most maxDrop), walking the chunk timeline.
uint64_t Engine::DropCapturedBefore(double tCut, uint64_t maxDrop, double now) {
    const size_t mask = chunks_.size() - 1;
    uint64_t dropped = 0;
    while (dropped < maxDrop && Avail() > 0) {
        const double t = CaptureTime(rAbs_, now);   // also advances the chunk head to rAbs_
        if (t >= tCut) break;
        uint64_t n = (uint64_t)std::ceil((tCut - t) * s_.rate);
        if (chCount_ > 1) n = std::min<uint64_t>(n, chunks_[(chHead_ + 1) & mask].start - rAbs_);   // stop at the next chunk
        n = std::clamp<uint64_t>(n, 1, maxDrop - dropped);
        Drop(n);
        dropped += n;
    }
    return dropped;
}

void Engine::WriteSeg(float* dst, const BYTE* src, uint32_t n, bool silent) {
    const uint32_t ch = s_.ch;
    if (a_.opt.testTone) {
        const double step = 2.0 * kPi * 440.0 / s_.rate;
        for (uint32_t f = 0; f < n; f++) {
            const float v = kToneAmp * (float)std::sin(tonePhase_);
            tonePhase_ += step;
            if (tonePhase_ > 2.0 * kPi) tonePhase_ -= 2.0 * kPi;
            for (uint32_t c = 0; c < ch; c++) dst[(size_t)f * ch + c] = v;
        }
    } else if (silent || !src) {
        memset(dst, 0, (size_t)n * ch * sizeof(float));
    } else {
        ConvertToFloat(src, dst, (size_t)n * ch, s_.cf.kind);
    }
}

void Engine::WriteRing(const BYTE* src, uint32_t n, bool silent) {
    const uint64_t pos = wAbs_ % ringCap_;
    const uint32_t first = (uint32_t)std::min<uint64_t>(n, ringCap_ - pos);
    WriteSeg(&ring_[(size_t)(pos * s_.ch)], src, first, silent);
    if (n > first) WriteSeg(&ring_[0], src ? src + (size_t)first * s_.cf.block : nullptr, n - first, silent);
    wAbs_ += n;
}

void Engine::ReadRing(float* dst, uint32_t n) {
    const uint64_t pos = rAbs_ % ringCap_;
    const uint32_t first = (uint32_t)std::min<uint64_t>(n, ringCap_ - pos);
    memcpy(dst, &ring_[(size_t)(pos * s_.ch)], (size_t)first * s_.ch * sizeof(float));
    if (n > first) memcpy(dst + (size_t)first * s_.ch, &ring_[0], (size_t)(n - first) * s_.ch * sizeof(float));
    rAbs_ += n;
}

HRESULT Engine::DrainCapture(double now) {
    UINT32 next = 0;
    HRESULT hr = s_.cap->GetNextPacketSize(&next);
    if (FAILED(hr)) return hr;
    while (next > 0) {
        BYTE* data = nullptr;
        UINT32 n = 0;
        DWORD flags = 0;
        UINT64 devPos = 0, qpcPos = 0;
        hr = s_.cap->GetBuffer(&data, &n, &flags, &devPos, &qpcPos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) return hr;
        if (n > 0) {
            const bool disc = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            if (disc && !capFirst_) a_.discontinuities.fetch_add(1, std::memory_order_relaxed);
            // capture time of the packet's first frame (QPC, 100 ns units); sanity checked
            const double dur = (double)n / s_.rate;
            double t = (double)qpcPos * 1e-7;
            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpcPos == 0 || t > now + 0.005 || t < now - 1.0) t = now - dur;
            // smooth the USB/engine timestamp jitter with a light PLL on the nominal timeline
            if (!capFirst_ && !disc && std::fabs(t - capNextT_) < 0.005) t = capNextT_ + 0.05 * (t - capNextT_);
            capNextT_ = t + dur;
            capFirst_ = false;
            // ring full (render stalled): drop the oldest frames
            const uint64_t freeFrames = ringCap_ - Avail();
            if (n > freeFrames) {
                Drop(n - freeFrames);
                a_.overflows.fetch_add(1, std::memory_order_relaxed);
            }
            PushChunk(wAbs_, t);
            WriteRing(data, n, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        }
        if (FAILED(hr = s_.cap->ReleaseBuffer(n))) return hr;
        if (FAILED(hr = s_.cap->GetNextPacketSize(&next))) return hr;
    }
    return S_OK;
}

void Engine::ApplyGain(float* p, uint32_t frames) {
    if (frames == 0) return;
    const float tgt = a_.muted.load(std::memory_order_relaxed) ? 0.0f : std::clamp(a_.volume.load(std::memory_order_relaxed), 0.0f, 2.0f);
    const float g0 = gain_;
    const float maxStep = (float)frames / ((float)s_.rate * 0.010f); // slew: 1.0 of gain per 10 ms (click free mute)
    const float g1 = g0 + std::clamp(tgt - g0, -maxStep, maxStep);
    gain_ = g1;
    const uint32_t ch = s_.ch;
    float pk = peakAcc_;
    if (g0 == 1.0f && g1 == 1.0f) {
        for (size_t i = 0, n = (size_t)frames * ch; i < n; i++) pk = std::max(pk, std::fabs(p[i]));
    } else {
        const float dg = (g1 - g0) / (float)frames;
        for (uint32_t f = 0; f < frames; f++) {
            const float g = g0 + dg * (float)f;
            for (uint32_t c = 0; c < ch; c++) {
                float& x = p[(size_t)f * ch + c];
                x *= g;
                pk = std::max(pk, std::fabs(x));
            }
        }
    }
    peakAcc_ = pk;
}

HRESULT Engine::ServiceRender(double now) {
    UINT32 pad = 0;
    HRESULT hr = s_.renClient->GetCurrentPadding(&pad);
    if (FAILED(hr)) return hr;
    const double dtEvt = lastRenT_ > 0 ? std::min(now - lastRenT_, 0.1) : 0.0;
    lastRenT_ = now;
    if (playing_ && pad == 0) {
        const uint64_t g = a_.glitches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (LogAllowed(now, lastGlitchLog_)) {
            LOGW("audio: render buffer ran empty (%llu total, +%llu); thread woke late", (unsigned long long)g,
                 (unsigned long long)(g - glitchLogged_));
            glitchLogged_ = g;
        }
    }
    if (pad >= s_.padTarget) return S_OK;
    const UINT32 want = std::min(s_.padTarget - pad, s_.renBuf - pad);
    if (want == 0) return S_OK;
    BYTE* buf = nullptr;
    if (FAILED(hr = s_.ren->GetBuffer(want, &buf))) return hr;
    float* out = reinterpret_cast<float*>(buf);
    const uint32_t ch = s_.ch;
    const double rate = s_.rate;
    const double target = TargetSec();

    if (!playing_) {
        // (re)priming: play silence until the buffer holds the target amount
        bool start = false;
        uint64_t avail = Avail();
        if (avail > 0) {
            const double d = now - CaptureTime(rAbs_, now) + pad / rate;
            if (d >= target) {
                if (d > target + 0.002) DropCapturedBefore(now + pad / rate - target, avail, now); // e.g. a burst after a stall
                avail = Avail();
                start = avail >= want;
            }
        }
        if (!start) return s_.ren->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
        playing_ = true;
        playStart_ = now;
        if (!everPlayed_) { everPlayed_ = true; firstPlay_ = now; }
        gain_ = 0.0f;   // fade in
        dSmooth_ = -1;
    }

    UINT32 off = 0;
    if (pendingSilence_ > 0) {
        off = (UINT32)std::min<uint64_t>(pendingSilence_, want);
        memset(out, 0, (size_t)off * ch * sizeof(float));
        pendingSilence_ -= off;
    }
    const UINT32 need = want - off;
    if (need > 0) {
        uint64_t avail = Avail();
        if (avail < need) {
            // underrun: play what is left with a short fade, then silence, and re-prime
            const uint32_t n = (uint32_t)avail;
            ReadRing(out + (size_t)off * ch, n);
            const uint32_t fade = std::min<uint32_t>(n, 64);
            for (uint32_t k = 0; k < fade; k++) {
                const float g = (float)k / (float)fade;
                float* fr = out + (size_t)(off + n - 1 - k) * ch;
                for (uint32_t c = 0; c < ch; c++) fr[c] *= g;
            }
            memset(out + (size_t)(off + n) * ch, 0, (size_t)(need - n) * ch * sizeof(float));
            if (playing_) { guardMs_ = std::min(guardMs_ + kGuardStepMs, kGuardMaxMs); lastGuardT_ = now; }
            playing_ = false;
            const uint64_t u = a_.underruns.fetch_add(1, std::memory_order_relaxed) + 1;
            if (LogAllowed(now, lastUnderLog_)) {
                LOGW("audio: underrun (capture data late or missing; %llu total, +%llu), re-priming to %.1f ms", (unsigned long long)u,
                     (unsigned long long)(u - underLogged_), target * 1000.0);
                underLogged_ = u;
            }
        } else {
            double d = now - CaptureTime(rAbs_, now) + (pad + off) / rate;
            if (d > target + kOverflowSec) {
                // too much buffered (stall, burst): drop the excess at once, by capture time so that
                // a hole in the capture timeline (frames lost during the stall) is not over-dropped
                const uint64_t drop = DropCapturedBefore(now + (pad + off) / rate - target, avail - need, now);
                d = now - CaptureTime(rAbs_, now) + (pad + off) / rate;
                dSmooth_ = d;
                const uint64_t o = a_.overflows.fetch_add(1, std::memory_order_relaxed) + 1;
                if (LogAllowed(now, lastOverLog_)) {
                    LOGW("audio: overflow (%llu total, +%llu), dropped %.1f ms", (unsigned long long)o, (unsigned long long)(o - overLogged_),
                         1000.0 * drop / rate);
                    overLogged_ = o;
                }
            }
            if (dSmooth_ < 0) dSmooth_ = d;
            else dSmooth_ += (d - dSmooth_) * (1.0 - std::exp(-dtEvt / kFillTauSec));
            diagDMin_ = std::min(diagDMin_, d);
            diagDMax_ = std::max(diagDMax_, d);

            // no rate adjust: single-frame drop / repeat, at most one per render period (~2000 ppm at 10 ms)
            int fix = 0;
            if (!rateAdj_ && correcting_ && now - lastFix_ >= 0.008) {
                fix = dSmooth_ > target ? 1 : -1;
                lastFix_ = now;
            }
            if (fix > 0 && Avail() > need) {
                Drop(1);
                dSmooth_ -= 1.0 / rate;
                fixNet_++;
            }
            float* dst = out + (size_t)off * ch;
            if (fix < 0 && need >= 2) {
                ReadRing(dst, need - 1);
                memcpy(dst + (size_t)(need - 1) * ch, dst + (size_t)(need - 2) * ch, ch * sizeof(float));
                dSmooth_ += 1.0 / rate;
                fixNet_--;
            } else {
                ReadRing(dst, need);
            }
            diagHeadMin_ = std::min(diagHeadMin_, Avail());
        }
    }
    ApplyGain(out, want);
    return s_.ren->ReleaseBuffer(want, 0);
}

void Engine::UpdateSync(double now, double dt) {
    const double v = a_.videoLatencyMs.load(std::memory_order_relaxed);
    double desired = 0;
    if (a_.opt.autoSync && v > 0) {
        // steady-state audio latency without the sync delay (model, not measurement: no feedback loop)
        const double baseMs = (TargetSec() * 1000.0 - sync_) + s_.renLatSec * 1000.0;
        desired = std::clamp(v - baseMs, 0.0, kSyncMaxMs);
        if (!syncInit_) {
            if (!playing_ || now - playStart_ <= 0.5) return;   // hold until the start-up decision
            syncInit_ = true;
            desiredEma_ = desired;
            if (now - firstPlay_ < kSyncJumpWindowSec) {   // start-up: jump instead of a slow minute-long glide
                StepSync(desired);
                return;
            }
        }
    }
    desiredEma_ += (desired - desiredEma_) * (1.0 - std::exp(-dt / 2.0));
    // Glide with a trapezoidal velocity profile: the velocity is the rate trim the servo must apply
    // (1 ms/s = 1000 ppm), so its acceleration stays below the trim slew limit -> no lag, no overshoot.
    // The drop/insert fallback corrects at most ~2 ms/s: glide slower there.
    const double vmax = rateAdj_ ? kSyncSlewMsPerSec : 1.5;
    const double acc = kSyncAccMsPerSec2;
    const double err = desiredEma_ - sync_;
    if (std::fabs(err) < 0.01 && std::fabs(syncVel_) < acc * dt) { syncVel_ = 0; return; }
    const double vWant = std::clamp(std::copysign(std::sqrt(2.0 * acc * std::fabs(err)), err), -vmax, vmax);
    syncVel_ += std::clamp(vWant - syncVel_, -acc * dt, acc * dt);
    sync_ = std::clamp(sync_ + syncVel_ * dt, 0.0, kSyncMaxMs);
}

void Engine::StepSync(double newSyncMs) {
    const double delta = newSyncMs - sync_;
    sync_ = newSyncMs;
    syncVel_ = 0;
    if (delta > 0) {
        pendingSilence_ += (uint64_t)(delta / 1000.0 * s_.rate);
        if (dSmooth_ >= 0) dSmooth_ += delta / 1000.0;
    } else if (delta < 0) {
        const uint64_t want = (uint64_t)(-delta / 1000.0 * s_.rate);
        const uint64_t drop = std::min<uint64_t>(want, Avail() > s_.padTarget ? Avail() - s_.padTarget : 0);
        Drop(drop);
        if (dSmooth_ >= 0) dSmooth_ -= (double)drop / s_.rate;
    }
    if (std::fabs(delta) > 0.05) LOGI("audio: auto-sync delay %.1f ms (start-up step)", sync_);
    LOGD("audio: step: smoothed buffer now %.2f ms, target %.2f ms", dSmooth_ * 1000.0, TargetSec() * 1000.0);
}

void Engine::Servo(double now) {
    if (lastServoT_ == 0) { lastServoT_ = now; return; }
    double dt = now - lastServoT_;
    if (dt < 0.05) return;
    lastServoT_ = now;
    dt = std::min(dt, 0.5);
    UpdateSync(now, dt);
    // feed-forward of a gliding sync delay: a target rising 1 ms/s needs the render side 1000 ppm slower
    const double ffPpm = -syncVel_ * 1000.0;
    if (!playing_ || dSmooth_ < 0) return;
    const double eMs = (dSmooth_ - TargetSec()) * 1000.0;
    if (rateAdj_) {
        // fill above target -> declare a higher stream rate -> the engine consumes faster
        const double pi = kServoKp * eMs + integ_ + ffPpm;
        // conditional integration (anti-windup): hold the integrator while saturated in the error's direction
        if (!((pi >= kMaxPpm && eMs > 0) || (pi <= -kMaxPpm && eMs < 0)))
            integ_ = std::clamp(integ_ + kServoKi * eMs * dt, -kMaxIntegPpm, kMaxIntegPpm);
        const double want = std::clamp(kServoKp * eMs + integ_ + ffPpm, -kMaxPpm, kMaxPpm);
        const double step = kPpmSlewPerSec * dt;
        ppm_ += std::clamp(want - ppm_, -step, step);
        if (std::fabs(ppm_ - ppmSet_) >= 1.0) {
            const HRESULT hr = s_.clockAdj->SetSampleRate((float)(s_.rate * (1.0 + ppm_ * 1e-6)));
            if (FAILED(hr)) {
                LOGW("audio: rate adjust failed (%s), using frame drop/insert", HrToString(hr).c_str());
                rateAdj_ = false;
                ppm_ = ppmSet_ = 0;
                a_.rateAdjust = false;
            } else {
                ppmSet_ = ppm_;
            }
        }
    } else {
        if (!correcting_ && std::fabs(eMs) > 3.0) correcting_ = true;
        else if (correcting_ && std::fabs(eMs) < 0.5) correcting_ = false;
    }
}

void Engine::Publish(double now) {
    const double dt = lastPub_ > 0 ? now - lastPub_ : 0.1;
    lastPub_ = now;
    if (guardMs_ > 0 && now - lastGuardT_ > kGuardDecaySec) { guardMs_ = std::max(0.0, guardMs_ - 1.0); lastGuardT_ = now; }
    if (!rateAdj_) {
        // effective trim of the drop/insert fallback, smoothed over ~1 s
        const double inst = (double)fixNet_ / (s_.rate * dt) * 1e6;
        fixNet_ = 0;
        fixPpm_ += (inst - fixPpm_) * std::min(1.0, dt);
    }
    if (now - lastDiag_ >= 5.0) {
        if (lastDiag_ > 0 && diagHeadMin_ != UINT64_MAX)
            LOGD("audio: last 5 s: raw buffer %.2f..%.2f ms (target %.2f), min ring headroom after write %.2f ms, trim %+.1f ppm",
                 diagDMin_ * 1000.0, diagDMax_ * 1000.0, TargetSec() * 1000.0, 1000.0 * diagHeadMin_ / s_.rate, rateAdj_ ? ppm_ : fixPpm_);
        lastDiag_ = now;
        diagDMin_ = 1e9; diagDMax_ = 0; diagHeadMin_ = UINT64_MAX;
    }
    peakPub_ = std::max(peakAcc_, peakPub_ * 0.7f);
    if (peakPub_ < 1e-6f) peakPub_ = 0.0f;   // -120 dBFS floor, no denormals
    peakAcc_ = 0;
    a_.playing = playing_;
    if (dSmooth_ >= 0) {
        a_.bufferMs = dSmooth_ * 1000.0;
        a_.latencyMs = (dSmooth_ + s_.renLatSec) * 1000.0;
    }
    a_.targetMs = TargetSec() * 1000.0;
    a_.syncMs = sync_;
    a_.ppm = rateAdj_ ? ppm_ : fixPpm_;
    a_.peak = peakPub_;
}

} // namespace

void AudioImpl::ThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"camss audio");
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
    else LOGW("audio: MMCSS registration failed (%lu)", GetLastError());
    {
        Engine e(*this);
        e.Run();
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// ---------------------------------------------------------------- public API
AudioMonitor::AudioMonitor() : p_(std::make_unique<AudioImpl>()) {}
AudioMonitor::~AudioMonitor() { Stop(); }

void AudioMonitor::ListDevices() {
    ComScope com;
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) { LOGW("audio: MMDeviceEnumerator unavailable: %s", HrToString(hr).c_str()); return; }
    for (EDataFlow flow : {eCapture, eRender}) {
        const auto eps = EnumEndpoints(en.Get(), flow, true);
        LOGI("audio %s endpoints: %zu", flow == eCapture ? "capture" : "render", eps.size());
        for (const auto& e : eps)
            LOGI("  %s %-40s %-22s container %s", e.isDefault ? "*" : " ", U8(e.name).c_str(), e.format.c_str(),
                 e.hasContainer ? GuidStr(e.container).c_str() : "?");
    }
    LOGI("  (* = default)");
}

bool AudioMonitor::Start(const AudioOptions& o) {
    Stop();
    if (!o.enabled) return false;
    AudioImpl& a = *p_;
    a.opt = o;
    a.opt.volume = std::clamp(o.volume, 0.0f, 2.0f);
    a.opt.bufferMs = std::clamp(o.bufferMs, 0.0, 500.0);
    a.opt.delayMs = std::clamp(o.delayMs, 0.0, 1000.0);
    a.volume = a.opt.volume;
    if (!a.Resolve(true)) return false;
    a.streaming = false; a.playing = false; a.rateAdjust = false;
    a.rate = 0; a.channels = 0;
    a.latencyMs = 0; a.bufferMs = 0; a.targetMs = 0; a.syncMs = 0; a.ppm = 0;
    a.underruns = 0; a.overflows = 0; a.discontinuities = 0; a.glitches = 0; a.reopens = 0;
    a.peak = 0;
    a.stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!a.stopEvt) return false;
    AudioImpl* impl = p_.get();
    a.thread = std::thread([impl] { impl->ThreadMain(); });
    return true;
}

void AudioMonitor::Stop() {
    AudioImpl& a = *p_;
    if (a.thread.joinable()) {
        SetEvent(a.stopEvt);
        a.thread.join();
    }
    if (a.stopEvt) { CloseHandle(a.stopEvt); a.stopEvt = nullptr; }
    a.streaming = false;
    a.playing = false;
}

bool AudioMonitor::Running() const { return p_->thread.joinable(); }
void AudioMonitor::SetMuted(bool m) { p_->muted = m; }
bool AudioMonitor::Muted() const { return p_->muted; }
void AudioMonitor::SetVolume(float v) { p_->volume = std::clamp(v, 0.0f, 2.0f); }
float AudioMonitor::Volume() const { return p_->volume; }
void AudioMonitor::SetVideoLatencyMs(double ms) { p_->videoLatencyMs = ms; }

AudioStats AudioMonitor::Stats() const {
    const AudioImpl& a = *p_;
    AudioStats s;
    s.running = a.streaming;
    {
        std::lock_guard<std::mutex> lk(a.nameMtx);
        s.captureName = a.capName;
        s.renderName = a.renName;
        s.matchMethod = a.matchMethod;
    }
    s.sampleRate = a.rate;
    s.channels = a.channels;
    s.latencyMs = a.latencyMs;
    s.bufferMs = a.bufferMs;
    s.targetBufferMs = a.targetMs;
    s.syncDelayMs = a.syncMs;
    s.rateAdjustPpm = a.ppm;
    s.underruns = a.underruns;
    s.overflows = a.overflows;
    s.discontinuities = a.discontinuities;
    s.peak = a.peak;
    s.playing = a.playing;
    s.rateAdjust = a.rateAdjust;
    s.capturePeriodMs = a.capPeriodMs;
    s.renderPeriodMs = a.renPeriodMs;
    s.renderLatencyMs = a.renLatencyMs;
    s.glitches = a.glitches;
    s.reopens = a.reopens;
    return s;
}

} // namespace camss
