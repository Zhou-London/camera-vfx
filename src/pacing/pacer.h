#pragma once
#include "clocks.h"
#include "pace_mode.h"
#include <deque>
#include <map>
#include <optional>

namespace cameravfx {

struct PacerConfig {
    PaceMode mode = PaceMode::Fixed;
    int multiplier = 1;
    bool fg = false;
    double contentPeriod = 1.0;
    double snapEps = 0;
    double marginMs = 0;
    double baseDelayMs = 0;
};

struct PaceDecision {
    bool valid = false;
    double sourcePosition = 0;
    int64_t frameA = 0, frameB = 0;
    float t = 0;
    bool late = false;
    bool hold = false;
    double delayMs = 0;
};

class Pacer {
public:
    void Configure(const PacerConfig& config, double signalHz);
    void OnSourceFrame(int64_t n, int64_t readyQpc, bool unique);
    double SignalHz() const { return signalClock_.Valid() ? (double)Qpc::Freq() / signalClock_.Period() : 0; }
    double SignalJitterMs() const { return signalClock_.JitterMs(); }
    int64_t NewestUniqueIndex() const { return newestUnique_; }
    int64_t NewestSignalIndex() const { return newestSignal_; }

    void SetMode(PaceMode mode) { Retarget(mode, contentPeriod_, multiplier_, fg_); }
    void SetContentPeriod(double contentPeriod) { Retarget(mode_, contentPeriod, multiplier_, fg_); }
    void SetMultiplier(int multiplier) { Retarget(mode_, contentPeriod_, multiplier, fg_); }
    void SetFg(bool on) { Retarget(mode_, contentPeriod_, multiplier_, on); }
    double ContentPeriod() const { return contentPeriod_; }
    int Multiplier() const { return multiplier_; }

    PaceDecision Decide(int64_t renderStart, int64_t displayTime);
    int64_t NextVrrDisplayTime(int64_t now, double renderLeadMs);

    double DelayMs() const { return Qpc::ToMs((int64_t)delay_); }
    double MinSlackMs() const { return minSlackMs_; }
    uint64_t LateCount() const { return lates_; }
    uint64_t HoldCount() const { return holds_; }
    uint64_t SkipCount() const { return skips_; }

private:
    static constexpr size_t kSignalClockWindow = 240;
    static constexpr int64_t kUniqueHistoryFrames = 240;
    struct UniqueFrame { int64_t readyQpc; int64_t lastRepeat; };
    using UniqueFrames = std::map<int64_t, UniqueFrame>;
    struct PairChoice {
        int64_t frameA = 0, frameB = 0;
        double t = 0;
        int64_t needReady = 0;
        bool nextMissing = false;
        double predictedNext = 0;
        bool contentOverdue = false;
    };
    struct SlackSample { int64_t renderStart; int64_t displayTime; double slackBeyondDelay; };

    void Retarget(PaceMode mode, double contentPeriod, int multiplier, bool fg);
    double SteadyStateExtraDelay(PaceMode mode, double contentPeriod, int multiplier, bool fg) const;
    double NextFrameNeededAbove() const { return fg_ ? cfg_.snapEps : 1.0 - cfg_.snapEps; }
    double InterpolationStart(int64_t frameA, double frameB, int64_t lastRepeat) const;
    std::optional<PairChoice> ChoosePair(double position) const;
    double TrialMinSlack(double trialDelay) const;
    double MinSlackBeyondDelay() const;
    void ServoDelay(int64_t renderStart);
    double GridAnchor(UniqueFrames::const_iterator newest) const;
    double GridPointAtOrAfter(double position, double step) const;
    void ClampDelay();

    PacerConfig cfg_;
    PaceMode mode_ = PaceMode::Fixed;
    double contentPeriod_ = 1.0;
    int multiplier_ = 1;
    bool fg_ = false;
    LinearClock signalClock_{kSignalClockWindow};
    UniqueFrames uniqueFrames_;
    int64_t newestUnique_ = -1;
    int64_t newestSignal_ = -1;
    double delay_ = 0;
    std::deque<SlackSample> slackWindow_;
    int64_t lastDecideTime_ = 0;
    int64_t lastLateTime_ = 0;
    double lastLateNeed_ = -1;
    double lastPosition_ = -1;
    int64_t lastVrrDisplayTime_ = 0;
    uint64_t lates_ = 0, holds_ = 0, skips_ = 0;
    double minSlackMs_ = 0;
};

}
