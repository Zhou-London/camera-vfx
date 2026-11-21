// camss application: wires capture -> convert -> pacing -> interpolation -> SR/HDR -> present.
#pragma once
#include "util/common.h"
#include "util/stats.h"
#include "gfx/device.h"
#include "gfx/color_convert.h"
#include "gfx/blit.h"
#include "capture/mf_capture.h"
#include "present/presenter.h"
#include "pacing/clocks.h"
#include "pacing/pacer.h"
#include "interp/interpolator.h"
#include "scale/rtx_video.h"
#include <memory>
#include <deque>
#include <array>

namespace camss {

class VfxChain;

struct Options {
    // capture
    std::wstring device;
    int deviceIndex = -1;
    PixFmt format = PixFmt::YUY2;
    UINT capW = 1920, capH = 1080;
    double capFps = 60.0;
    bool synthetic = false;
    ColorOptions color;
    // pacing
    PaceMode mode = PaceMode::Fixed;
    int vrrK = 2;
    bool lateLatch = true;
    double leadMs = 3.0;            // initial render lead (late latch / VRR)
    double initialDelayMs = 0;      // 0 = estimate from mode
    double marginMs = 1.0;
    double snapEps = 0.06;
    int passSync = 1;               // passthrough: present sync interval
    // processing
    std::string interp = "none";    // none | blend | nvof | vfx
    int vfgMode = 0;                // VFX frame generation: 0 low, 1 medium, 2 high
    int nvofQuality = 1;
    std::string sr = "none";        // none | rtx | vfx
    int srQuality = 21;             // VFX VSR quality level (21 = streaming medium)
    std::string hdr = "none";       // none | rtx | vfx
    bool hdrBeforeSr = true;        // VFX chain: TrueHDR at source resolution, then VSR in RGB10A2
    UINT outW = 0, outH = 0;        // processing output size (0 = display size)
    float sdrWhiteNits = 203.f;
    // presentation
    bool windowed = false;
    int monitor = 0;
    // measurement
    double durationSec = 0;         // 0 = until ESC
    std::string csvPath;
    std::string logPath;
    bool timecode = false;          // draw loopback timecode band
    bool decodeTimecode = false;    // decode timecode band from capture (HDMI loopback)
    bool marker = false;            // flashing photodiode marker square
    double reportSec = 2.0;
    double warmupSec = 6.0;         // statistics are reset once after this
    bool autoMode = true;           // probe the display for VRR, then pick vrr or fixed
    bool debug = false;
    bool statsDebug = false;
    bool selftest = false;       // dump raw DXGI frame statistics for the first frames
};

bool ParseOptions(int argc, wchar_t** argv, Options& o);
int RunSelfTest(const Options& o);
void PrintUsage();

class App {
public:
    explicit App(const Options& o);
    ~App();
    bool Init(HINSTANCE inst);
    int Run();

private:
    static constexpr int kStore = 8;
    struct OutRecord {
        uint64_t idx = 0;
        UINT presentId = 0;
        int64_t renderStart = 0, presentCall = 0, predictedV = 0, actualV = 0;
        int64_t nA = 0, nB = 0; float t = 0;
        double delayMs = 0, slackMs = 0; bool late = false;
        int64_t newestArrival = 0, newestSample = 0, contentSample = 0, newestUploaded = 0;
        double renderCpuMs = 0;
        bool finalized = false;
    };

    void Ingest(int64_t now);
    void IngestCaptured(const RawFrame& f);
    SourceFrame* Frame(int64_t n);
    // presentAt != 0: hold the Present() until that QPC time (VRR: the flip happens at present).
    bool Render(const PaceDecision& d, int64_t renderStart, int64_t V, UINT sync, bool tearing, int64_t presentAt = 0);
    void PollPresentStats();
    void FinalizeRecord(OutRecord& r);
    void Report(bool final);
    void HandleKeys();
    void RunFixed();
    void RunVrr();
    void RunPassthrough(double untilSec = 0, int sync = -1);

    Options o_;
    Device dev_;
    MfCapture* cap_ = nullptr;
    ColorConverter conv_;
    SyntheticSource synth_;
    Presenter pres_;
    VsyncClock vsync_;
    Pacer pacer_;
    Blitter blit_;
    GpuProfiler prof_;
    std::unique_ptr<Interpolator> interp_;
    std::unique_ptr<RtxVideo> rtx_;
    std::unique_ptr<VfxChain> vfx_;
    StatBook stats_;
    PreciseSleeper sleeper_;

    std::array<SourceFrame, kStore> store_;
    Device::Tex interpOut_;          // src res, RGBA8
    Device::Tex vpOut_;              // out res, BGRA8 or R10G10B10A2
    UINT srcW_ = 0, srcH_ = 0, outW_ = 0, outH_ = 0;
    bool interpOn_ = true, srOn_ = true;
    bool hdrOut_ = false;

    int64_t synthNext_ = 0, synthN_ = 0, synthT0_ = 0;
    int64_t lastArrival_ = 0;
    int64_t lastPresentCall_ = 0;
    UINT lastStatsPresent_ = 0, lastStatsRefresh_ = 0;
    double vrrRatio_ = 3.0;
    bool vrrActive_ = false;
    double presentIntervalEma_ = 0;
    bool warmupDone_ = false;
    std::deque<OutRecord> records_;
    uint64_t outCount_ = 0;
    uint64_t missed_ = 0, finalizedCount_ = 0;
    double leadMs_ = 3.0;
    double lastVfxGpuMs_ = 0;
    RollingStat frameCost_{300};   // recent per-frame cost (ms) for the render-lead estimate
    int64_t lastMissQpc_ = 0, lastLeadAdjust_ = 0;
    FILE* csv_ = nullptr;
    int64_t startQpc_ = 0, lastReport_ = 0;
    uint64_t timecodeHits_ = 0;
};

} // namespace camss
