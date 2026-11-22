#pragma once
#include "presenter.h"
#include "../ui/telemetry.h"
#include "../util/stats.h"
#include <deque>
#include <optional>
#include <string>

namespace cameravfx {

struct PresentedFrame {
    UINT presentId = 0;
    UINT syncInterval = 0;
    int64_t renderStart = 0, presentCall = 0, predictedVblank = 0;
    int64_t frameA = 0, frameB = 0;
    float t = 0;
    double delayMs = 0, minSlackMs = 0;
    bool late = false, hold = false;
    int64_t newestSample = 0, newestArrival = 0, newestUploaded = 0, contentSample = 0;
    double renderCpuMs = 0;
    float gpuMs = 0;
};

struct PresentEvents {
    bool missedVblank = false;
    bool becameComposed = false;
};

class PresentMonitor {
public:
    PresentMonitor(Presenter& presenter, StatBook& stats, Telemetry& telemetry) : presenter_(presenter), stats_(stats), telemetry_(telemetry) {}
    ~PresentMonitor();
    PresentMonitor(const PresentMonitor&) = delete;
    PresentMonitor& operator=(const PresentMonitor&) = delete;

    void OpenCsv(const std::string& path, int64_t startQpc);
    void OnPresented(const PresentedFrame& frame);
    PresentEvents Poll(int64_t now, bool vrrPacing);
    void FinalizeAll();
    void Discard() { records_.clear(); }

    uint64_t Presented() const { return presented_; }
    uint64_t MissedVblanks() const { return missedVblanks_; }
    bool VrrActive() const { return vrrActive_; }
    double RefreshesPerPresent() const { return refreshesPerPresent_; }
    bool Composed() const { return presentationMode_ == DXGI_FRAME_PRESENTATION_MODE_COMPOSED; }
    const char* ShownAt() const { return shownAtScanout_ ? "scanout" : "flip"; }

private:
    struct Record {
        uint64_t index = 0;
        PresentedFrame frame;
        int64_t scanout = 0;
    };
    bool FlipTimed(const Record& r) const { return r.frame.syncInterval == 0 || vrrActive_; }
    void TrackVrr(const DXGI_FRAME_STATISTICS& st, bool vrrPacing);
    void PollPresentationMode(int64_t now, PresentEvents& events);
    bool Finalize(const Record& r);
    void WriteCsv(const Record& r);

    Presenter& presenter_;
    StatBook& stats_;
    Telemetry& telemetry_;
    std::deque<Record> records_;
    uint64_t presented_ = 0;
    uint64_t missedVblanks_ = 0;
    int64_t lastPresentCall_ = 0;
    double presentIntervalEmaMs_ = 0;
    UINT lastStatsPresent_ = 0, lastStatsRefresh_ = 0;
    double refreshesPerPresent_ = 3.0;
    bool vrrActive_ = false;
    bool shownAtScanout_ = false;
    std::optional<int> presentationMode_;
    int64_t lastModePoll_ = 0;
    FILE* csv_ = nullptr;
    int64_t csvStartQpc_ = 0;
};

}
