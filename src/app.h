// camss application: wires capture -> convert -> pacing -> interpolation -> SR/HDR -> present.
#pragma once
#include "util/common.h"
#include "config.h"
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
#include "pacing/cadence.h"
#include "audio/audio_monitor.h"
#include <memory>
#include <deque>
#include <array>

namespace camss {

class VfxChain;

int RunSelfTest(const Options& o);

class App {
public:
    explicit App(const Options& o);
    ~App();
    bool Init(HINSTANCE inst);
    int Run();

private:
    static constexpr int kStore = 16;   // source frames kept (covers content periods up to 4 with the pacing lag)
    struct OutRecord {
        uint64_t idx = 0;
        UINT presentId = 0;
        int64_t renderStart = 0, presentCall = 0, predictedV = 0, actualV = 0;
        int64_t nA = 0, nB = 0; float t = 0;
        double delayMs = 0, slackMs = 0; bool late = false, hold = false;
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
    // Frame generation adapts to the content rate (repeat detection + cadence).
    void OnContentRateChanged();
    int ChooseMultiplier(double contentFps) const;
    // (Re)creates the Maxine chain with the current quality settings (run-time F / Q keys).
    bool BuildVfx();
    void RebuildVfx(int oldFgMode, int oldSrQuality);   // falls back to these on failure
    void WarmUp(bool initial);
    void PrintDetail();
    void StartAudio();
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
    CadenceDetector cadence_;
    AudioMonitor audio_;
    StatBook stats_;
    PreciseSleeper sleeper_;

    std::array<SourceFrame, kStore> store_;
    Device::Tex interpOut_;          // src res, RGBA8
    Device::Tex vpOut_;              // out res, BGRA8 or R10G10B10A2
    UINT srcW_ = 0, srcH_ = 0, outW_ = 0, outH_ = 0;
    UINT rtxInW_ = 0, rtxInH_ = 0;   // input size of the RTX video processor stage
    bool haveSlot_ = false;
    ComPtr<ID3D11Query> doneQ_;      // GPU completion of the last submitted frame
    uint64_t gpuBusyAtPresent_ = 0;          // swap-chain waitable consumed but not yet presented
    bool interpOn_ = true, srOn_ = true;
    bool hdrOut_ = false;

    int64_t synthNext_ = 0, synthN_ = 0, synthT0_ = 0;
    int64_t synthContent_ = -1;      // synthetic source: last content frame index rendered
    bool adaptive_ = false;          // repeat-aware frame generation active
    int64_t lastUniqueN_ = -1;       // newest source frame carrying new content
    uint64_t repeats_ = 0;           // repeated source frames skipped
    uint64_t lastSeq_ = 0;           // capture sequence of the last frame seen by the app
    bool contentLost_ = false;       // a new-content frame was lost: treat the next frames as new until one is stored
    int curK_ = 2;                   // output frames per content frame (VRR)
    uint64_t lastReportOut_ = 0;     // output count at the previous report (output rate)
    double lastOutFps_ = 0;
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
