#include "pacer.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace cameravfx {

namespace {
constexpr double kMaxDelayMs = 80.0;
constexpr double kSlackWindowSec = 2.0;
constexpr double kReleaseAfterWindowFraction = 0.75;
constexpr double kHoldAfterLateSec = 5.0;
constexpr double kAttackTimeSec = 0.03;
constexpr double kReleaseTimeSec = 0.5;
constexpr double kSlowReleaseMsPerSec = 2.0;
constexpr double kFastSlewMsPerSec = 8.0;
constexpr double kLargeExcessMs = 4.0;
constexpr double kArrivalResidualQuantile = 0.99;
constexpr double kStalledSourceAfterPeriods = 0.5;
constexpr double kMaxLatePenaltyContentPeriods = 0.5;
constexpr double kLongGapContentPeriods = 1.5;
constexpr double kIndexTolerance = 1e-9;
constexpr int kGridAnchorSearchDepth = 8;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

int64_t SlackWindowTicks() { return Qpc::FromMs(kSlackWindowSec * 1000.0); }

double ReleasePerSec(double excess) {
    const bool attack = excess < 0;
    const bool largeExcess = excess > (double)Qpc::FromMs(kLargeExcessMs);
    const double maxSlew = (double)Qpc::FromMs(attack || largeExcess ? kFastSlewMsPerSec : kSlowReleaseMsPerSec);
    return std::clamp(excess / (attack ? kAttackTimeSec : kReleaseTimeSec), -maxSlew, maxSlew);
}
}

const char* PaceModeName(PaceMode mode) {
    switch (mode) {
    case PaceMode::Passthrough: return "passthrough";
    case PaceMode::Fixed: return "fixed";
    case PaceMode::Vrr: return "vrr";
    }
    return "?";
}

void Pacer::Configure(const PacerConfig& config, double signalHz) {
    *this = Pacer{};
    cfg_ = config;
    mode_ = config.mode;
    contentPeriod_ = config.contentPeriod;
    multiplier_ = config.multiplier;
    fg_ = config.fg;
    signalClock_.SetNominalPeriod((double)Qpc::Freq() / signalHz);
    delay_ = (double)Qpc::FromMs(config.baseDelayMs) + SteadyStateExtraDelay(mode_, contentPeriod_, multiplier_, fg_);
    ClampDelay();
}

void Pacer::OnSourceFrame(int64_t n, int64_t readyQpc, bool unique) {
    signalClock_.Add(n, readyQpc);
    newestSignal_ = std::max(newestSignal_, n);
    if (unique) {
        uniqueFrames_[n] = UniqueFrame{readyQpc, -1};
        newestUnique_ = std::max(newestUnique_, n);
        while (uniqueFrames_.begin()->first < newestUnique_ - kUniqueHistoryFrames) uniqueFrames_.erase(uniqueFrames_.begin());
    } else if (!uniqueFrames_.empty() && n > uniqueFrames_.rbegin()->first) {
        uniqueFrames_.rbegin()->second.lastRepeat = n;
    }
}

void Pacer::Retarget(PaceMode mode, double contentPeriod, int multiplier, bool fg) {
    if (mode == mode_ && contentPeriod == contentPeriod_ && multiplier == multiplier_ && fg == fg_) return;
    const double delayChange =
        SteadyStateExtraDelay(mode, contentPeriod, multiplier, fg) - SteadyStateExtraDelay(mode_, contentPeriod_, multiplier_, fg_);
    mode_ = mode;
    contentPeriod_ = contentPeriod;
    multiplier_ = multiplier;
    fg_ = fg;
    if (delayChange == 0) return;
    delay_ += delayChange;
    ClampDelay();
    slackWindow_.clear();
    lastLateNeed_ = -1;
}

double Pacer::SteadyStateExtraDelay(PaceMode mode, double contentPeriod, int multiplier, bool fg) const {
    if (!fg) return 0;
    const double contentPeriodTicks = contentPeriod * signalClock_.Period();
    return mode == PaceMode::Vrr ? contentPeriodTicks * (double)(multiplier - 1) / (double)multiplier : contentPeriodTicks;
}

void Pacer::ClampDelay() {
    delay_ = std::clamp(delay_, 0.0, (double)Qpc::FromMs(kMaxDelayMs));
}

double Pacer::InterpolationStart(int64_t frameA, double frameB, int64_t lastRepeat) const {
    const bool repeatsFilledLongGap =
        frameB - (double)frameA > kLongGapContentPeriods * contentPeriod_ && lastRepeat > frameA && (double)lastRepeat < frameB;
    return repeatsFilledLongGap ? std::max((double)frameA, std::min((double)lastRepeat + 1.0, frameB) - contentPeriod_) : (double)frameA;
}

std::optional<Pacer::PairChoice> Pacer::ChoosePair(double position) const {
    auto itA = uniqueFrames_.upper_bound((int64_t)std::floor(position));
    if (itA == uniqueFrames_.begin()) return std::nullopt;
    --itA;
    PairChoice pair;
    pair.frameA = pair.frameB = itA->first;
    pair.needReady = itA->second.readyQpc;
    const double neededAbove = NextFrameNeededAbove();
    const auto itB = std::next(itA);
    if (itB != uniqueFrames_.end()) {
        const double frameB = (double)itB->first;
        const double start = InterpolationStart(pair.frameA, frameB, itA->second.lastRepeat);
        const double step = std::clamp((position - start) / (frameB - start), 0.0, 1.0);
        if (step > neededAbove) {
            pair.needReady = itB->second.readyQpc;
            pair.frameB = itB->first;
            if (fg_ && step < 1.0 - cfg_.snapEps) pair.t = step;
            else pair.frameA = itB->first;
        }
        return pair;
    }
    pair.predictedNext = std::max((double)pair.frameA + contentPeriod_, (double)(newestSignal_ + 1));
    pair.contentOverdue = newestSignal_ >= (int64_t)std::ceil((double)pair.frameA + contentPeriod_ - kIndexTolerance);
    const double start = pair.contentOverdue ? (double)pair.frameA : InterpolationStart(pair.frameA, pair.predictedNext, itA->second.lastRepeat);
    const double expected = pair.contentOverdue ? (double)pair.frameA + contentPeriod_ : pair.predictedNext;
    pair.nextMissing = (position - start) / (expected - start) > neededAbove;
    return pair;
}

double Pacer::TrialMinSlack(double trialDelay) const {
    double minSlack = kInfinity;
    for (const SlackSample& sample : slackWindow_) {
        const std::optional<PairChoice> pair = ChoosePair(signalClock_.IndexAt((double)sample.displayTime - trialDelay));
        if (!pair || pair->nextMissing || pair->needReady > sample.renderStart) return -kInfinity;
        minSlack = std::min(minSlack, (double)(sample.renderStart - pair->needReady));
    }
    return minSlack;
}

double Pacer::MinSlackBeyondDelay() const {
    double minSlack = kInfinity;
    for (const SlackSample& sample : slackWindow_) minSlack = std::min(minSlack, sample.slackBeyondDelay);
    return minSlack;
}

void Pacer::ServoDelay(int64_t renderStart) {
    const bool gridMode = mode_ == PaceMode::Vrr;
    const double margin = (double)Qpc::FromMs(cfg_.marginMs);
    const double phaseGuard = !gridMode && contentPeriod_ > 1.0 ? margin : 0.0;
    const double minSlack = gridMode ? MinSlackBeyondDelay() + delay_ : TrialMinSlack(delay_ - phaseGuard);
    minSlackMs_ = std::isfinite(minSlack) ? Qpc::ToMs((int64_t)minSlack) : -1.0;
    const double elapsedSec = lastDecideTime_ ? Qpc::ToSec(renderStart - lastDecideTime_) : 0.0;
    const double release = ReleasePerSec(minSlack - margin) * elapsedSec;
    if (release < 0) {
        delay_ -= release;
        return;
    }
    const bool windowFull = renderStart - slackWindow_.front().renderStart > (int64_t)(kReleaseAfterWindowFraction * (double)SlackWindowTicks());
    const bool recentlyLate = lastLateTime_ && renderStart - lastLateTime_ < Qpc::FromMs(kHoldAfterLateSec * 1000.0);
    if (windowFull && !recentlyLate && (gridMode || TrialMinSlack(delay_ - release - phaseGuard) >= margin)) delay_ -= release;
}

PaceDecision Pacer::Decide(int64_t renderStart, int64_t displayTime) {
    PaceDecision decision;
    if (!signalClock_.Valid() || uniqueFrames_.empty()) return decision;
    double position = std::max(signalClock_.IndexAt((double)displayTime - delay_), lastPosition_);
    std::optional<PairChoice> pair = ChoosePair(position);
    if (pair && pair->nextMissing) {
        const double period = signalClock_.Period();
        const double residual = signalClock_.AbsResidualQuantile(kArrivalResidualQuantile);
        const double neededAbove = NextFrameNeededAbove();
        const double contentFramesPastNext = (position - pair->predictedNext) / contentPeriod_;
        const double furtherFramesNeeded = contentFramesPastNext > neededAbove ? std::floor(contentFramesPastNext - neededAbove) + 1.0 : 0.0;
        const double neededIndex = pair->predictedNext + contentPeriod_ * furtherFramesNeeded;
        const double neededKey = (double)pair->frameA + contentPeriod_ * (furtherFramesNeeded + 1.0);
        const double nextSignalOverdue = (double)renderStart - (signalClock_.TimeOf((double)(newestSignal_ + 1)) + residual);
        if (pair->contentOverdue || nextSignalOverdue > kStalledSourceAfterPeriods * period) {
            holds_++;
            decision.hold = true;
        } else {
            decision.late = true;
            if (neededKey != lastLateNeed_) {
                lates_++;
                lastLateTime_ = renderStart;
                lastLateNeed_ = neededKey;
                const double deficit = std::max(0.0, signalClock_.TimeOf(neededIndex) + residual - (double)renderStart);
                delay_ += std::min(deficit, kMaxLatePenaltyContentPeriods * contentPeriod_ * period) + (double)Qpc::FromMs(cfg_.marginMs);
            }
        }
        position = std::max(std::min(position, (double)pair->frameA), lastPosition_);
        pair->frameB = pair->frameA;
        pair->t = 0;
    } else if (pair) {
        slackWindow_.push_back({renderStart, displayTime, (double)(renderStart - pair->needReady) - delay_});
    }

    while (!slackWindow_.empty() && slackWindow_.front().renderStart < renderStart - SlackWindowTicks()) slackWindow_.pop_front();
    if (!slackWindow_.empty() && !decision.late && !decision.hold) ServoDelay(renderStart);
    lastDecideTime_ = renderStart;
    ClampDelay();
    if (!pair) return decision;

    lastPosition_ = position;
    decision.valid = true;
    decision.sourcePosition = position;
    decision.frameA = pair->frameA;
    decision.frameB = pair->frameB;
    decision.t = (float)pair->t;
    decision.delayMs = DelayMs();
    return decision;
}

double Pacer::GridAnchor(UniqueFrames::const_iterator newest) const {
    const int64_t cadenceStartInterval = (int64_t)std::ceil(contentPeriod_ - kIndexTolerance);
    auto it = newest;
    for (int depth = 0; depth < kGridAnchorSearchDepth; depth++) {
        const auto next = std::next(it);
        if (next != uniqueFrames_.end() && next->first - it->first == cadenceStartInterval) return (double)it->first;
        if (it == uniqueFrames_.begin()) break;
        --it;
    }
    return (double)newest->first;
}

double Pacer::GridPointAtOrAfter(double position, double step) const {
    const auto after = uniqueFrames_.upper_bound((int64_t)std::floor(position));
    const double anchor = after == uniqueFrames_.begin() ? 0.0 : GridAnchor(std::prev(after));
    return anchor + std::ceil((position - anchor) / step - kIndexTolerance) * step;
}

int64_t Pacer::NextVrrDisplayTime(int64_t now, double renderLeadMs) {
    if (!signalClock_.Valid()) return 0;
    const double step = contentPeriod_ / (double)multiplier_;
    const double halfLeadPosition = signalClock_.IndexAt((double)(now + Qpc::FromMs(renderLeadMs) / 2) - delay_);
    double position;
    if (lastVrrDisplayTime_ == 0) {
        position = GridPointAtOrAfter(halfLeadPosition, step);
    } else {
        const double earliest = signalClock_.IndexAt((double)lastVrrDisplayTime_ - delay_) + 0.5 * step;
        position = GridPointAtOrAfter(std::max(earliest, halfLeadPosition), step);
        if (position > earliest + step) skips_++;
    }
    lastVrrDisplayTime_ = (int64_t)(signalClock_.TimeOf(position) + delay_);
    return lastVrrDisplayTime_;
}

}
