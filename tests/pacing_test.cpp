#include "check.h"
#include "pacing/cadence.h"
#include "pacing/lead.h"
#include "pacing/multiplier.h"
#include "pacing/pacer.h"
#include "util/stats.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace cameravfx;
using check::Verdict;

namespace {

constexpr double kSignalHz = 60.0;
constexpr double kPeriodMs = 1000.0 / kSignalHz;
constexpr double kLeadMs = 3.0;
constexpr double kMarginMs = 1.0;
constexpr double kBaseDelayMs = 7.0;
constexpr double kSnap = 0.06;
constexpr double kJitterMs = 1.5;
constexpr double kToleranceMs = 1.0;
constexpr double kSpikeMs = 4.0;
constexpr double kProcessingMs = 1.0;
constexpr double kStallSec = 0.25;
constexpr double kRampMs = 4.0;
constexpr double kVrrPollMs = 1.0;
constexpr double kVrrOutputFps = 120.0;
constexpr double kWarmUpSec = 8.0;
constexpr double kEventWatchSec = 10.0;
constexpr double kSlowReleaseMsPerSec = 2.0;
constexpr double kSmallExcessMs = 3.0;
constexpr double kReleaseRateWindowSec = 0.25;
constexpr double kReleaseRateTolerance = 1.05;

using UniquePattern = std::function<bool(int64_t n, double sec)>;

bool Every2nd(int64_t n) { return n % 2 == 0; }
bool Cadence32(int64_t n) { const int64_t r = n % 5; return r == 0 || r == 3; }
bool Cadence21(int64_t n) { return n % 3 != 1; }
bool Cadence112(int64_t n) { return n % 4 != 3; }

double P50(std::vector<double> values) { return values.empty() ? 0 : Quantile(values, 0.5); }

double Sec(int64_t ticks) { return (double)ticks / (double)Qpc::Freq(); }

PacerConfig TestConfig(PaceMode mode, int multiplier, bool fg) {
    PacerConfig cfg;
    cfg.mode = mode;
    cfg.multiplier = multiplier;
    cfg.fg = fg;
    cfg.snapEps = kSnap;
    cfg.marginMs = kMarginMs;
    cfg.baseDelayMs = kBaseDelayMs;
    return cfg;
}

enum class Detector { Off, ContentPeriod, ContentPeriodAndMultiplier };

struct SimCase {
    const char* name = "";
    PaceMode mode = PaceMode::Vrr;
    double displayHz = 164.0;
    int multiplier = 2;
    bool fg = true;
    UniquePattern unique = [](int64_t, double) { return true; };
    double contentPeriod = 1.0;
    Detector detector = Detector::Off;
    double driftPpm = 20;
    double jitterMs = kJitterMs;
    double spikeProb = 0;
    double seconds = 60;
    unsigned seed = 4321;
    double stallAtSec = -1;
    double rampAtSec = -1;
    double rampMsPerSec = 0;
    double eventSec = -1;
    double switchToFixedAtSec = -1;
    double reconfigureAtSec = -1;
};

struct SimResult {
    uint64_t outputs = 0, interpolated = 0, lates = 0, holds = 0, backwards = 0;
    double outFps = 0, delayP50 = 0, delayMax = 0, latencyP50 = 0;
    double maxSpeedDev = 0, maxGridErr = 0, maxMotionStep = 0, maxPresentGapMs = 0, maxSlowReleaseMsPerSec = 0;
    std::vector<double> lateSec;
    std::vector<std::pair<double, double>> delays;
    std::vector<std::pair<double, double>> delayTrace;
    std::vector<std::pair<double, bool>> interpolatedAt;
    std::vector<double> latencies;
    double delayBeforeEvent = 0, delayMaxAfterEvent = 0;
    uint64_t holdsAfterEvent = 0;
};

double MaxSlowReleaseMsPerSec(const std::vector<std::pair<double, double>>& trace, double settledMs) {
    double maxRate = 0;
    size_t later = 0;
    for (size_t i = 0; i < trace.size(); i++) {
        while (later < trace.size() && trace[later].first < trace[i].first + kReleaseRateWindowSec) later++;
        if (later == trace.size()) break;
        if (trace[i].second - settledMs > kSmallExcessMs) continue;
        maxRate = std::max(maxRate, (trace[i].second - trace[later].second) / (trace[later].first - trace[i].first));
    }
    return maxRate;
}

class Simulation {
public:
    explicit Simulation(const SimCase& c)
        : c_(c), freq_((double)Qpc::Freq()), period_(freq_ / (kSignalHz * (1.0 + c.driftPpm * 1e-6))), base_((int64_t)(freq_ * 10)),
          warm_(base_ + (int64_t)(kWarmUpSec * freq_)), mode_(c.mode) {
        BuildSource();
        cfg_ = TestConfig(c.mode, c.multiplier, c.fg);
        if (c.detector == Detector::Off) cfg_.contentPeriod = c.contentPeriod;
        pacer_.Configure(cfg_, kSignalHz);
    }

    SimResult Run() {
        const int64_t lead = (int64_t)(freq_ * kLeadMs / 1000.0);
        const int64_t vrrPoll = (int64_t)(freq_ * kVrrPollMs / 1000.0);
        const int64_t refresh = (int64_t)(freq_ / c_.displayHz);
        const int64_t end = base_ + (int64_t)(c_.seconds * freq_);
        int64_t displayTime = base_ + vrrPoll;
        while (displayTime < end) {
            const double sec = Sec(displayTime - base_);
            if (mode_ == PaceMode::Vrr && c_.switchToFixedAtSec > 0 && sec >= c_.switchToFixedAtSec) {
                mode_ = PaceMode::Fixed;
                pacer_.SetMode(mode_);
            }
            if (!reconfigured_ && c_.reconfigureAtSec > 0 && sec >= c_.reconfigureAtSec) Reconfigure();
            if (mode_ == PaceMode::Vrr) {
                Feed(displayTime - lead - vrrPoll);
                const int64_t next = pacer_.NextVrrDisplayTime(displayTime - lead, kLeadMs);
                if (!next) {
                    displayTime += vrrPoll;
                    continue;
                }
                displayTime = next;
            }
            const int64_t renderStart = displayTime - lead;
            Feed(renderStart);
            const uint64_t holdsBefore = pacer_.HoldCount();
            const PaceDecision d = pacer_.Decide(renderStart, displayTime);
            if (d.valid) Score(d, displayTime, pacer_.HoldCount() - holdsBefore);
            displayTime += mode_ == PaceMode::Vrr ? 1 : refresh;
        }
        return Summarize();
    }

private:
    void BuildSource() {
        std::mt19937 rng(c_.seed);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        const int frames = (int)(c_.seconds * kSignalHz) + 10;
        ready_.resize(frames);
        unique_.resize(frames);
        motion_.resize(frames);
        double content = -1;
        for (int n = 0; n < frames; n++) {
            const double sec = n / kSignalHz;
            const double jitter = uniform(rng) * c_.jitterMs + (uniform(rng) < c_.spikeProb ? kSpikeMs : 0.0);
            const double ramp = c_.rampAtSec > 0 ? std::clamp((sec - c_.rampAtSec) * c_.rampMsPerSec, 0.0, kRampMs) : 0.0;
            ready_[n] = base_ + (int64_t)(n * period_ + freq_ * (kProcessingMs + jitter + ramp) / 1000.0);
            unique_[n] = n == 0 || c_.unique(n, sec);
            if (unique_[n]) content += 1;
            motion_[n] = content;
        }
        if (c_.stallAtSec > 0) {
            const int64_t from = base_ + (int64_t)(c_.stallAtSec * freq_), to = from + (int64_t)(kStallSec * freq_);
            for (int n = 0; n < frames; n++)
                if (ready_[n] >= from && ready_[n] < to) ready_[n] = to + n % 3;
        }
    }

    void Feed(int64_t until) {
        for (; fed_ < ready_.size() && ready_[fed_] <= until; fed_++) {
            const int64_t n = (int64_t)fed_;
            pacer_.OnSourceFrame(n, ready_[fed_], unique_[fed_]);
            if (c_.detector == Detector::Off || !detector_.OnFrame(n, unique_[fed_])) continue;
            pacer_.SetContentPeriod(detector_.ContentPeriod());
            if (c_.detector == Detector::ContentPeriodAndMultiplier)
                pacer_.SetMultiplier(std::max(1, (int)std::lround(kVrrOutputFps / (kSignalHz / detector_.ContentPeriod()))));
        }
    }

    void Reconfigure() {
        reconfigured_ = true;
        cfg_.contentPeriod = detector_.ContentPeriod();
        pacer_.Configure(cfg_, kSignalHz);
        detector_ = CadenceDetector(kDefaultContentPeriods, cfg_.contentPeriod);
        previousPosition_ = -1;
    }

    void Score(const PaceDecision& d, int64_t displayTime, uint64_t newHolds) {
        const double sec = Sec(displayTime - base_);
        if (!unique_[(size_t)d.frameA] || !unique_[(size_t)d.frameB]) Error("repeat frame used");
        if (d.sourcePosition < previousPosition_ - 1e-9) Error("source position went backwards");
        const double shown = motion_[(size_t)d.frameA] + (d.t > 0 ? d.t * (motion_[(size_t)d.frameB] - motion_[(size_t)d.frameA]) : 0.0);
        if (shown < previousMotion_ - 1e-9) Error("motion went backwards");
        r_.delayTrace.push_back({sec, d.delayMs});
        if (c_.eventSec > 0) {
            if (sec < c_.eventSec) r_.delayBeforeEvent = d.delayMs;
            else if (sec < c_.eventSec + kEventWatchSec) {
                r_.delayMaxAfterEvent = std::max(r_.delayMaxAfterEvent, d.delayMs);
                r_.holdsAfterEvent += newHolds;
            }
        }
        if (d.late) {
            r_.lateSec.push_back(sec);
            lastLate_ = displayTime;
        }
        if (displayTime > warm_) {
            r_.outputs++;
            r_.delays.push_back({sec, d.delayMs});
            r_.interpolatedAt.push_back({sec, d.t > 0});
            if (d.late) r_.lates++;
            if (d.t > 0) r_.interpolated++;
            if (!d.hold) r_.latencies.push_back(Qpc::ToMs(displayTime - ready_[(size_t)(d.t > 0 ? d.frameB : d.frameA)]));
            if (previousPosition_ >= 0 && displayTime - lastLate_ > (int64_t)(0.5 * freq_)) {
                const double speed = (d.sourcePosition - previousPosition_) / ((double)(displayTime - previousDisplayTime_) / period_);
                r_.maxSpeedDev = std::max(r_.maxSpeedDev, std::abs(speed - 1.0));
            }
            if (mode_ == PaceMode::Vrr) {
                const double grid = d.sourcePosition * pacer_.Multiplier() / pacer_.ContentPeriod();
                r_.maxGridErr = std::max(r_.maxGridErr, std::abs(grid - std::round(grid)) / pacer_.Multiplier());
                r_.maxPresentGapMs = std::max(r_.maxPresentGapMs, Qpc::ToMs(displayTime - previousDisplayTime_));
            }
            if (previousMotion_ >= 0) r_.maxMotionStep = std::max(r_.maxMotionStep, shown - previousMotion_);
        }
        previousPosition_ = d.sourcePosition;
        previousMotion_ = shown;
        previousDisplayTime_ = displayTime;
    }

    void Error(const char* what) {
        printf("  ERROR: %s\n", what);
        r_.backwards++;
    }

    SimResult Summarize() {
        r_.holds = pacer_.HoldCount();
        r_.outFps = (double)r_.outputs / (c_.seconds - kWarmUpSec);
        std::vector<double> delays;
        for (const auto& [sec, delay] : r_.delays) delays.push_back(delay);
        r_.delayP50 = P50(delays);
        r_.delayMax = delays.empty() ? 0 : *std::max_element(delays.begin(), delays.end());
        r_.latencyP50 = P50(r_.latencies);
        r_.maxSlowReleaseMsPerSec = MaxSlowReleaseMsPerSec(r_.delayTrace, r_.delayP50);
        return r_;
    }

    const SimCase c_;
    const double freq_, period_;
    const int64_t base_, warm_;
    PacerConfig cfg_;
    Pacer pacer_;
    CadenceDetector detector_;
    PaceMode mode_;
    std::vector<int64_t> ready_;
    std::vector<bool> unique_;
    std::vector<double> motion_;
    size_t fed_ = 0;
    bool reconfigured_ = false;
    double previousPosition_ = -1, previousMotion_ = -1;
    int64_t previousDisplayTime_ = 0, lastLate_ = 0;
    SimResult r_;
};

SimResult Simulate(const SimCase& c) { return Simulation(c).Run(); }

double DelayP50In(const SimResult& r, double fromSec, double toSec) {
    std::vector<double> values;
    for (const auto& [sec, delay] : r.delays)
        if (sec >= fromSec && sec < toSec) values.push_back(delay);
    return P50(values);
}

size_t LatesIn(const SimResult& r, double fromSec, double toSec) {
    return (size_t)std::count_if(r.lateSec.begin(), r.lateSec.end(), [&](double sec) { return sec >= fromSec && sec < toSec; });
}

double TheoryVrr(double contentPeriod, int multiplier) { return (double)(multiplier - 1) / multiplier * contentPeriod * kPeriodMs + kLeadMs + kMarginMs; }

void SteadyTests() {
    struct Case { SimCase sim; double boundMs; };
    const double clean = kLeadMs + kJitterMs + kMarginMs + kToleranceMs;
    const double fixedNoGuard = kPeriodMs + kLeadMs + kMarginMs;
    auto fixed = [](const char* name, double hz, double drift, double spikes) {
        SimCase c; c.name = name; c.mode = PaceMode::Fixed; c.displayHz = hz; c.driftPpm = drift; c.spikeProb = spikes; c.seed = 1234; return c;
    };
    auto vrr = [](const char* name, int multiplier, double drift, double spikes) {
        SimCase c; c.name = name; c.multiplier = multiplier; c.driftPpm = drift; c.spikeProb = spikes; c.seed = 1234; return c;
    };
    const Case cases[] = {
        {fixed("fixed 120Hz +80ppm", 120.0, 80, 0.01), kPeriodMs + clean + kSpikeMs},
        {fixed("fixed 120Hz -30ppm", 119.998, -30, 0.01), kPeriodMs + clean + kSpikeMs},
        {fixed("fixed 144Hz", 144.0, 20, 0.01), kPeriodMs + clean + kSpikeMs},
        {fixed("fixed 164Hz", 164.0, 20, 0.01), kPeriodMs + clean + kSpikeMs},
        {fixed("fixed 164Hz, no spikes", 164.0, 20, 0.0), fixedNoGuard},
        {vrr("vrr k=2 (120 fps)", 2, 40, 0.01), kPeriodMs / 2 + clean + kSpikeMs},
        {vrr("vrr k=2, no spikes", 2, 40, 0.0), kPeriodMs / 2 + clean},
        {vrr("vrr k=3 (180 fps)", 3, 40, 0.01), 2 * kPeriodMs / 3 + clean + kSpikeMs},
        {vrr("vrr k=2, 59.94 Hz source", 2, -1000, 0.0), kPeriodMs / 2 + clean},
        {fixed("fixed 164Hz, 59.94 Hz src", 164.0, -1000, 0.0), fixedNoGuard},
    };
    for (const Case& c : cases) {
        const SimResult r = Simulate(c.sim);
        const bool vrrMode = c.sim.mode == PaceMode::Vrr;
        const bool spikes = c.sim.spikeProb > 0;
        const bool ok = r.lates <= (spikes ? 20u : 0u) && r.backwards == 0 && r.delayP50 <= c.boundMs && r.maxSpeedDev < 0.03 &&
                        (!vrrMode || r.maxGridErr < 0.02) && (spikes || r.maxSlowReleaseMsPerSec <= kSlowReleaseMsPerSec * kReleaseRateTolerance);
        printf("%-26s out=%6llu interp=%6llu late=%2llu back=%llu  D p50=%6.2f max=%6.2f (bound %5.2f) ms  release<=%.2f ms/s  speed dev=%.4f  "
               "grid err=%.4f  %s\n",
               c.sim.name, (unsigned long long)r.outputs, (unsigned long long)r.interpolated, (unsigned long long)r.lates,
               (unsigned long long)r.backwards, r.delayP50, r.delayMax, c.boundMs, r.maxSlowReleaseMsPerSec, r.maxSpeedDev, r.maxGridErr,
               Verdict(ok));
    }
    for (PaceMode mode : {PaceMode::Vrr, PaceMode::Fixed}) {
        SimCase c;
        c.name = mode == PaceMode::Vrr ? "stall 250ms (vrr)" : "stall 250ms (fixed 164)";
        c.mode = mode; c.jitterMs = 0.5; c.seconds = 30; c.seed = 99; c.stallAtSec = 15; c.eventSec = 15;
        const SimResult r = Simulate(c);
        const bool ok = r.delayMaxAfterEvent - r.delayBeforeEvent < 3.0 && r.backwards == 0 && r.holds > 0;
        printf("%-26s D before %.2f ms, max within 10 s after %.2f ms, hold outputs %llu, late %llu  %s\n", c.name, r.delayBeforeEvent,
               r.delayMaxAfterEvent, (unsigned long long)r.holds, (unsigned long long)r.lateSec.size(), Verdict(ok));
    }
    for (double rampMsPerSec : {4.0, 6.0}) {
        SimCase c;
        c.name = rampMsPerSec == 4.0 ? "arrival +4 ms at 4 ms/s" : "arrival +4 ms at 6 ms/s";
        c.seconds = 40; c.rampAtSec = 20; c.rampMsPerSec = rampMsPerSec;
        const SimResult r = Simulate(c);
        const bool ok = r.lates == 0 && r.backwards == 0;
        printf("%-26s late=%llu back=%llu  D before %.2f ms, after %.2f ms  %s\n", c.name, (unsigned long long)r.lates,
               (unsigned long long)r.backwards, DelayP50In(r, 15, 20), DelayP50In(r, 30, 40), Verdict(ok));
    }
}

struct CadenceRun {
    int switches = 0;
    double final = 1.0;
    std::vector<std::pair<int64_t, double>> changes;
};

CadenceRun RunCadence(const std::function<bool(int64_t)>& unique, int64_t frames, const std::function<bool(int64_t)>& received = nullptr,
                      const std::vector<double>& periods = kDefaultContentPeriods, double startPeriod = 1.0) {
    CadenceDetector detector(periods, startPeriod);
    CadenceRun r;
    r.final = startPeriod;
    for (int64_t n = 0; n < frames; n++) {
        if (received && !received(n)) continue;
        if (detector.OnFrame(n, unique(n))) { r.switches++; r.changes.push_back({n, detector.ContentPeriod()}); }
    }
    r.final = detector.ContentPeriod();
    return r;
}

void CadenceTests() {
    auto report = [&](const char* name, const CadenceRun& r, bool ok, const char* extra = "") {
        printf("%-34s active=%.3f switches=%d", name, r.final, r.switches);
        for (size_t i = 0; i < r.changes.size() && i < 4; i++) printf(" [@%lld -> %.3f]", (long long)r.changes[i].first, r.changes[i].second);
        printf("%s  %s\n", extra, Verdict(ok));
    };
    auto every = [](int64_t) { return true; };
    { CadenceRun r = RunCadence(every, 1200); report("cadence 60 fps", r, r.final == 1.0 && r.switches == 0); }
    { CadenceRun r = RunCadence(Every2nd, 1200); report("cadence 30-in-60", r, r.final == 2.0 && r.switches == 1); }
    { CadenceRun r = RunCadence(Cadence32, 1200); report("cadence 24p 3:2", r, r.final == 2.5 && r.switches == 1); }
    { CadenceRun r = RunCadence(Cadence21, 1200); report("cadence 40 fps (2:1)", r, r.final == 1.5 && r.switches == 1); }
    { CadenceRun r = RunCadence(Cadence112, 1200); report("cadence ~45 fps (1,1,2)", r, std::abs(r.final - 4.0 / 3.0) < 0.05 && r.switches == 1); }
    { CadenceRun r = RunCadence(Every2nd, 1200, nullptr, kDefaultContentPeriods, 2.0); report("cadence 30-in-60, started at 30", r, r.final == 2.0 && r.switches == 0); }
    { CadenceRun r = RunCadence(every, 1200, nullptr, kDefaultContentPeriods, 2.0); report("cadence 60 fps, started at 30", r, r.final == 1.0 && r.switches == 1); }
    {
        CadenceRun r = RunCadence([](int64_t n) { return n < 600 || n >= 660; }, 1200);
        report("cadence 60 + 1 s static", r, r.final == 1.0 && r.switches == 0);
    }
    {
        CadenceRun r = RunCadence([](int64_t n) { return n < 600 || n >= 900; }, 1500);
        report("cadence 60 + 5 s static", r, r.final == 1.0 && r.switches == 0);
    }
    for (int segment : {20, 30}) {
        CadenceRun r = RunCadence([segment](int64_t n) { return (n / segment) % 2 == 0 || n % 2 == 0; }, 3600);
        char name[64];
        snprintf(name, sizeof(name), "cadence 30/60 alternating %d fr", segment);
        report(name, r, r.switches <= 1);
    }
    for (bool toThirty : {true, false}) {
        const int64_t at = 1200;
        auto pattern = [at, toThirty](int64_t n) { return (n < at) == toThirty ? true : n % 2 == 0; };
        CadenceRun r = RunCadence(pattern, 2400);
        const int64_t latency = r.changes.empty() ? -1 : r.changes.back().first - at;
        const bool ok = r.final == (toThirty ? 2.0 : 1.0) && r.switches == (toThirty ? 1 : 2) && latency >= CadenceDetector::kHoldFrames &&
                        latency <= CadenceDetector::kHoldFrames + CadenceDetector::kWindowFrames + 2;
        char extra[64];
        snprintf(extra, sizeof(extra), "  latency %lld frames (%.0f ms)", (long long)latency, latency * kPeriodMs);
        report(toThirty ? "cadence 60->30 switch" : "cadence 30->60 switch", r, ok, extra);
    }
    {
        std::mt19937 rng(7);
        std::vector<bool> received(3000);
        for (auto&& v : received) v = (rng() % 100) >= 3;
        for (int i = 1500; i < 1600; i++) received[i] = false;
        CadenceRun r = RunCadence(Every2nd, 3000, [&](int64_t n) { return (bool)received[(size_t)n]; });
        report("cadence 30-in-60, drops + gap", r, r.final == 2.0 && r.switches == 1);
    }
    {
        const std::vector<double> console = ContentPeriods({60, 30}, kSignalHz);
        auto pattern = [](int64_t n) { return n < 600 ? true : n < 690 ? n % 4 != 3 : n % 2 == 0; };
        CadenceRun r = RunCadence(pattern, 1500, nullptr, console);
        bool onlyListed = true;
        for (const auto& change : r.changes) onlyListed &= change.second == 1.0 || change.second == 2.0;
        report("console 60->45 mix->30", r, onlyListed && r.final == 2.0 && r.switches == 1);
        CadenceRun all = RunCadence(pattern, 1500);
        report("default 60->45 mix->30", all, all.final == 2.0);
        CadenceRun menu = RunCadence([](int64_t n) { return n < 300 ? true : (n * 100) % 471 < 100; }, 1500, nullptr, console);
        report("console menu ~12.7 fps", menu, menu.final == 1.0 && menu.switches == 0);
    }
    {
        std::mt19937 rng(9);
        std::vector<bool> received(3000);
        for (auto&& v : received) v = (rng() % 100) >= 2;
        CadenceRun r = RunCadence(Cadence32, 3000, [&](int64_t n) { return (bool)received[(size_t)n]; });
        report("cadence 24p 3:2, 2% drops", r, r.final == 2.5 && r.switches == 1);
    }
    {
        const std::vector<double> periods = ContentPeriods({120, 60, 30, 24}, kSignalHz);
        const bool ok = periods == std::vector<double>{1.0, 2.0, 2.5} && ContentPeriods({120}, kSignalHz).empty();
        printf("%-34s %zu periods  %s\n", "content rates 120,60,30,24 at 60 Hz", periods.size(), Verdict(ok));
    }
}

void PredictionTests() {
    const double freq = (double)Qpc::Freq(), period = freq / kSignalHz;
    const int64_t base = (int64_t)(freq * 10);
    auto make = [&](Pacer& p, int64_t last, double delayMs) {
        PacerConfig cfg = TestConfig(PaceMode::Fixed, 1, true);
        cfg.contentPeriod = 2.5;
        cfg.baseDelayMs = delayMs - 2.5 * kPeriodMs;
        p.Configure(cfg, kSignalHz);
        for (int64_t n = 0; n <= last; n++) p.OnSourceFrame(n, base + (int64_t)std::llround((double)n * period), Cadence32(n));
    };
    auto at = [&](Pacer& p, double position) {
        const int64_t displayTime = base + (int64_t)std::llround(position * period + (double)Qpc::FromMs(p.DelayMs()));
        return p.Decide(displayTime - Qpc::FromMs(kLeadMs), displayTime);
    };
    {
        Pacer p;
        make(p, 292, 50.0);
        const PaceDecision d = at(p, 290.165);
        const bool ok = d.valid && !d.late && !d.hold && d.frameA == 290 && d.t == 0 && p.LateCount() == 0;
        printf("%-34s x=%.3f frameA=%lld t=%.3f late=%d lates=%llu  %s\n", "predict 24p: snaps to A, not late", d.sourcePosition,
               (long long)d.frameA, d.t, d.late, (unsigned long long)p.LateCount(), Verdict(ok));
    }
    {
        Pacer p;
        make(p, 291, 25.0);
        const PaceDecision first = at(p, 290.3);
        p.OnSourceFrame(292, base + (int64_t)std::llround(292.0 * period), false);
        const PaceDecision second = at(p, 290.6);
        const bool ok = first.late && !first.hold && second.late && !second.hold && p.LateCount() == 1;
        printf("%-34s late=%d,%d penalties=%llu  %s\n", "predict 24p: one penalty per frame", first.late, second.late,
               (unsigned long long)p.LateCount(), Verdict(ok));
    }
}

void ContentTests() {
    auto every2nd = [](int64_t n, double) { return Every2nd(n); };
    auto cadence32 = [](int64_t n, double) { return Cadence32(n); };
    auto content = [](const char* name, PaceMode mode, double hz, int multiplier, UniquePattern unique, double contentPeriod) {
        SimCase c; c.name = name; c.mode = mode; c.displayHz = hz; c.multiplier = multiplier; c.unique = std::move(unique); c.contentPeriod = contentPeriod;
        return c;
    };
    const double bound = kJitterMs + kToleranceMs;
    const double fixedTheory = 2 * kPeriodMs + kLeadMs + kMarginMs;
    struct Steady { SimCase sim; double theory; double expectFps; bool gridCheck; };
    Steady cases[] = {
        {content("30-in-60 vrr k=2 (60 fps)", PaceMode::Vrr, 0, 2, every2nd, 2.0), TheoryVrr(2, 2), 60, true},
        {content("30-in-60 vrr k=4 (120 fps)", PaceMode::Vrr, 0, 4, every2nd, 2.0), TheoryVrr(2, 4), 120, true},
        {content("30-in-60 fixed 120Hz", PaceMode::Fixed, 120.0, 2, every2nd, 2.0), fixedTheory, 120, false},
        {content("30-in-60 fixed 165Hz", PaceMode::Fixed, 165.0, 2, every2nd, 2.0), fixedTheory, 165, false},
        {content("24p 3:2 vrr k=2 (48 fps)", PaceMode::Vrr, 0, 2, cadence32, 2.5), 1.75 * kPeriodMs + kLeadMs + kMarginMs, 48, false},
        {content("30-in-60 vrr k=2, 1% spikes", PaceMode::Vrr, 0, 2, every2nd, 2.0), TheoryVrr(2, 2) + kSpikeMs, 60, true},
        {content("30-in-60 fixed 120Hz, no fg", PaceMode::Fixed, 120.0, 2, every2nd, 2.0), kLeadMs + 2 * kMarginMs + kSnap * 2 * kPeriodMs, 120, false},
    };
    cases[5].sim.spikeProb = 0.01;
    cases[6].sim.fg = false;
    for (const Steady& sc : cases) {
        const SimResult r = Simulate(sc.sim);
        const bool ok = r.lates <= (sc.sim.spikeProb > 0 ? 20u : 0u) && r.backwards == 0 && r.delayP50 <= sc.theory + bound &&
                        std::abs(r.outFps - sc.expectFps) < 0.02 * sc.expectFps && (!sc.gridCheck || r.maxGridErr < 0.01) && r.maxMotionStep < 1.01;
        printf("%-30s out %6.1f fps interp=%5llu late=%2llu back=%llu  D p50=%6.2f max=%6.2f (theory %5.2f, bound %5.2f) ms  "
               "arrival->display p50 %5.2f ms  grid err=%.4f  motion step<=%.2f  %s\n",
               sc.sim.name, r.outFps, (unsigned long long)r.interpolated, (unsigned long long)r.lates, (unsigned long long)r.backwards,
               r.delayP50, r.delayMax, sc.theory, sc.theory + bound, r.latencyP50, r.maxGridErr, r.maxMotionStep, Verdict(ok));
    }

    auto switching = [](int64_t n, double sec) { return sec >= 20 && sec < 40 ? Every2nd(n) : true; };
    for (Detector detector : {Detector::ContentPeriod, Detector::ContentPeriodAndMultiplier}) {
        const bool multiplierFollows = detector == Detector::ContentPeriodAndMultiplier;
        SimCase c = content(multiplierFollows ? "switch 60->30->60 vrr 120 fps" : "switch 60->30->60 vrr k=2", PaceMode::Vrr, 0, 2, switching, 1.0);
        c.detector = detector;
        const SimResult r = Simulate(c);
        const size_t nearSwitch = LatesIn(r, 20, 23) + LatesIn(r, 40, 43);
        const size_t elsewhere = LatesIn(r, kWarmUpSec, 60) - nearSwitch;
        const int multiplier30 = multiplierFollows ? 4 : 2;
        const double d60a = DelayP50In(r, 15, 20), d30 = DelayP50In(r, 35, 40), d60b = DelayP50In(r, 55, 60);
        const double t60 = TheoryVrr(1, 2), t30 = TheoryVrr(2, multiplier30);
        const double maxGapMs = 1.5 * 1000.0 / kVrrOutputFps + kToleranceMs;
        auto closeToTheory = [](double measured, double theory) { return measured - theory < 2.5 && measured > theory - 0.5; };
        const bool ok = nearSwitch <= 6 && elsewhere == 0 && r.backwards == 0 && closeToTheory(d60a, t60) && closeToTheory(d30, t30) &&
                        closeToTheory(d60b, t60) && (!multiplierFollows || r.maxPresentGapMs <= maxGapMs);
        printf("%-30s late near switches=%zu elsewhere=%zu back=%llu holds=%llu  D p50 60fps %.2f (theory %.2f) | 30fps %.2f (theory %.2f) | "
               "60fps %.2f  max present gap %.2f ms  %s\n", c.name, nearSwitch, elsewhere, (unsigned long long)r.backwards,
               (unsigned long long)r.holds, d60a, t60, d30, t30, d60b, r.maxPresentGapMs, Verdict(ok));
    }
    {
        SimCase c = content("switch 60->30->60 fixed 164", PaceMode::Fixed, 164.0, 2, switching, 1.0);
        c.detector = Detector::ContentPeriod;
        const SimResult r = Simulate(c);
        const size_t nearSwitch = LatesIn(r, 20, 23) + LatesIn(r, 40, 43);
        const size_t elsewhere = LatesIn(r, kWarmUpSec, 60) - nearSwitch;
        const double d60a = DelayP50In(r, 15, 20), d30 = DelayP50In(r, 35, 40), d60b = DelayP50In(r, 55, 60);
        const double fixedBound = kLeadMs + kJitterMs + kMarginMs + kToleranceMs;
        const bool ok = nearSwitch <= 6 && elsewhere == 0 && r.backwards == 0 && d60a < kPeriodMs + fixedBound && d30 < 2 * kPeriodMs + fixedBound &&
                        d60b < kPeriodMs + fixedBound;
        printf("%-30s late near switches=%zu elsewhere=%zu back=%llu holds=%llu  D p50 60fps %.2f | 30fps %.2f | 60fps %.2f  %s\n", c.name,
               nearSwitch, elsewhere, (unsigned long long)r.backwards, (unsigned long long)r.holds, d60a, d30, d60b, Verdict(ok));
    }
    for (PaceMode mode : {PaceMode::Vrr, PaceMode::Fixed}) {
        SimCase c = content(mode == PaceMode::Vrr ? "static 1 s (vrr k=2)" : "static 1 s (fixed 164)", mode, 164.0, 2,
                            [](int64_t, double sec) { return !(sec >= 20 && sec < 21); }, 1.0);
        c.eventSec = 20;
        const SimResult r = Simulate(c);
        uint64_t interpolatedAfter = 0;
        for (const auto& [sec, interpolated] : r.interpolatedAt)
            if (sec > 21.5 && sec < 30) interpolatedAfter += interpolated;
        const bool ok = r.delayMaxAfterEvent - r.delayBeforeEvent < 1.0 && r.holdsAfterEvent > 0 && r.lates == 0 && r.backwards == 0 &&
                        interpolatedAfter > 100 && r.maxMotionStep < 0.76;
        printf("%-30s D before %.2f ms, max within 10 s after %.2f ms, hold outputs %llu, late %llu, interp after %llu, "
               "max motion step %.2f frames  %s\n", c.name, r.delayBeforeEvent, r.delayMaxAfterEvent, (unsigned long long)r.holdsAfterEvent,
               (unsigned long long)r.lates, (unsigned long long)interpolatedAfter, r.maxMotionStep, Verdict(ok));
    }
    {
        SimCase c = content("30-in-60 phase shift vrr k=3", PaceMode::Vrr, 0, 3, [](int64_t n, double sec) { return sec < 20 ? Every2nd(n) : !Every2nd(n); },
                            2.0);
        const SimResult r = Simulate(c);
        const double before = DelayP50In(r, 12, 20), after = DelayP50In(r, 30, 60), theory = TheoryVrr(2, 3);
        const bool ok = r.lates <= 2 && r.backwards == 0 && before - theory < 2.5 && after - theory < 2.5;
        printf("%-30s late=%llu back=%llu  D p50 before %.2f | after %.2f (theory %.2f)  %s\n", c.name, (unsigned long long)r.lates,
               (unsigned long long)r.backwards, before, after, theory, Verdict(ok));
    }
    {
        SimCase c = content("stall 250ms (30-in-60 vrr k=2)", PaceMode::Vrr, 0, 2, every2nd, 2.0);
        c.stallAtSec = 15; c.eventSec = 15; c.seconds = 30;
        const SimResult r = Simulate(c);
        const bool ok = r.delayMaxAfterEvent - r.delayBeforeEvent < 3.0 && r.backwards == 0 && r.holds > 0;
        printf("%-30s D before %.2f ms, max within 10 s after %.2f ms, hold outputs %llu, late %llu  %s\n", c.name, r.delayBeforeEvent,
               r.delayMaxAfterEvent, (unsigned long long)r.holds, (unsigned long long)r.lates, Verdict(ok));
    }
}

void StartupPathTests() {
    {
        SimCase c;
        c.name = "vrr start, fixed 164 at 3 s";
        c.switchToFixedAtSec = 3;
        c.seconds = 30;
        const SimResult r = Simulate(c);
        const double bound = kPeriodMs + kLeadMs + kJitterMs + kMarginMs + kToleranceMs;
        const bool ok = LatesIn(r, 6, 30) == 0 && r.backwards == 0 && r.delayP50 <= bound;
        printf("%-30s late after 6 s=%zu back=%llu  D p50=%.2f (bound %.2f) ms  %s\n", c.name, LatesIn(r, 6, 30), (unsigned long long)r.backwards,
               r.delayP50, bound, Verdict(ok));
    }
    {
        SimCase c;
        c.name = "30-in-60 vrr k=2, reconfigure";
        c.unique = [](int64_t n, double) { return Every2nd(n); };
        c.detector = Detector::ContentPeriod;
        c.reconfigureAtSec = 20;
        c.eventSec = 20;
        c.seconds = 40;
        const SimResult r = Simulate(c);
        const double after = DelayP50In(r, 30, 40), theory = TheoryVrr(2, 2);
        const bool ok = LatesIn(r, 20, 22) <= 2 && LatesIn(r, 22, 40) == 0 && r.holdsAfterEvent == 0 && r.backwards == 0 && after - theory < 2.5;
        printf("%-30s late in 2 s after=%zu later=%zu holds after=%llu back=%llu  D p50 after %.2f (theory %.2f)  %s\n", c.name, LatesIn(r, 20, 22),
               LatesIn(r, 22, 40), (unsigned long long)r.holdsAfterEvent, (unsigned long long)r.backwards, after, theory, Verdict(ok));
    }
}

void ReleaseTests() {
    const double freq = (double)Qpc::Freq();
    const double period = freq / 59.94, vblank = freq / 165.0;
    for (double leadMs : {8.0, 11.5, 12.5}) {
        PacerConfig cfg = TestConfig(PaceMode::Vrr, 2, true);
        cfg.contentPeriod = 2.0;
        Pacer pacer;
        pacer.Configure(cfg, kSignalHz);
        const int64_t base = (int64_t)(freq * 10);
        int64_t now = base, fed = 0;
        auto feed = [&](int64_t until) {
            for (; base + (int64_t)(fed * period) <= until; fed++) pacer.OnSourceFrame(fed, base + (int64_t)(fed * period), fed % 2 == 0);
        };
        uint64_t outputs = 0, gaps = 0;
        int64_t lastDisplayTime = 0;
        const int64_t end = base + (int64_t)(freq * 30);
        const int64_t settled = base + (int64_t)(freq * 5);
        const double outputStep = period * pacer.ContentPeriod() / pacer.Multiplier();
        while (now < end) {
            feed(now);
            const int64_t displayTime = pacer.NextVrrDisplayTime(now, leadMs);
            if (!displayTime) { now += (int64_t)(freq * 0.002); continue; }
            const int64_t start = std::max(now, displayTime - (int64_t)(freq * leadMs / 1000.0));
            feed(start);
            const PaceDecision d = pacer.Decide(start, displayTime);
            if (!d.valid) { now = start + (int64_t)(freq * 0.001); continue; }
            if (now > settled) {
                outputs++;
                if (lastDisplayTime && (double)(displayTime - lastDisplayTime) > 1.5 * outputStep) gaps++;
            }
            lastDisplayTime = displayTime;
            now = (int64_t)(std::ceil((double)displayTime / vblank) * vblank) + (int64_t)(freq * 0.0002);
        }
        const double fps = (double)outputs / 25.0;
        const bool ok = fps > 59.0 && gaps == 0;
        printf("vrr release at vblank, lead %4.1f   output %.1f fps, 2-step gaps %llu  %s\n", leadMs, fps, (unsigned long long)gaps, Verdict(ok));
    }
}

void VblankIndexTests() {
    const double period = (double)Qpc::Freq() / 60.0;
    const int64_t base = Qpc::Freq() * 10;
    auto at = [&](double vblanks) { return base + (int64_t)std::llround(vblanks * period); };
    LinearClock warming(600);
    const bool first = VblankIndex(warming, -1, 0, at(0), period) == 0;
    const bool missedWhileWarming = VblankIndex(warming, 5, at(5), at(7.1), period) == 7;
    const bool doubleWakeWhileWarming = VblankIndex(warming, 5, at(5), at(5.2), period) == 6;
    LinearClock fitted(600);
    for (int n = 0; n < 20; n++) fitted.Add(n, at(n));
    const bool lateWake = VblankIndex(fitted, 19, at(19), at(20.3), period) == 20;
    const bool doubleWake = VblankIndex(fitted, 20, at(20.3), at(20.4), period) <= 20;
    const bool missed = VblankIndex(fitted, 19, at(19), at(22.05), period) == 22;
    const bool ok = first && missedWhileWarming && doubleWakeWhileWarming && lateWake && doubleWake && missed;
    printf("%-34s first=%d missed=%d,%d double=%d,%d late=%d  %s\n", "vblank index", first, missedWhileWarming, missed, doubleWakeWhileWarming,
           doubleWake, lateWake, Verdict(ok));
}

void MultiplierTests() {
    MultiplierInputs base;
    base.fgActive = true;
    base.vrr = true;
    base.contentFps = 30;
    base.requested = 2;
    base.thresholdFps = 60;
    base.maxRefreshHz = 165;
    base.leadMs = 3;
    base.current = 2;
    auto choose = [&](auto change) {
        MultiplierInputs in = base;
        change(in);
        return ChooseMultiplier(in);
    };
    const MultiplierChoice doubled = choose([](MultiplierInputs&) {});
    const MultiplierChoice off = choose([](MultiplierInputs& in) { in.fgActive = false; });
    const MultiplierChoice atThreshold = choose([](MultiplierInputs& in) { in.contentFps = 59.94; });
    const MultiplierChoice refresh = choose([](MultiplierInputs& in) { in.requested = 4; in.maxRefreshHz = 100; });
    const MultiplierChoice gpu = choose([](MultiplierInputs& in) { in.requested = 4; in.generatedFrameMs = 7.0; });
    const MultiplierChoice fixedIgnoresGpu = choose([](MultiplierInputs& in) { in.vrr = false; in.requested = 4; in.generatedFrameMs = 10.0; });
    const MultiplierChoice raiseNeedsRoom = choose([](MultiplierInputs& in) { in.requested = 3; in.current = 2; in.generatedFrameMs = 10.5; });
    const MultiplierChoice keepWithLessRoom = choose([](MultiplierInputs& in) { in.requested = 3; in.current = 3; in.generatedFrameMs = 10.5; });
    const bool ok = doubled.multiplier == 2 && doubled.limit == MultiplierLimit::None && off.multiplier == 1 && off.limit == MultiplierLimit::Off &&
                    atThreshold.multiplier == 1 && atThreshold.limit == MultiplierLimit::Threshold && refresh.multiplier == 3 &&
                    refresh.limit == MultiplierLimit::Refresh && gpu.multiplier == 3 && gpu.limit == MultiplierLimit::Gpu &&
                    fixedIgnoresGpu.multiplier == 4 && raiseNeedsRoom.multiplier == 2 && keepWithLessRoom.multiplier == 3;
    printf("%-34s x%d off=x%d threshold=x%d refresh=x%d gpu=x%d fixed=x%d raise=x%d keep=x%d  %s\n", "multiplier choice", doubled.multiplier,
           off.multiplier, atThreshold.multiplier, refresh.multiplier, gpu.multiplier, fixedIgnoresGpu.multiplier, raiseNeedsRoom.multiplier,
           keepWithLessRoom.multiplier, Verdict(ok));
}

double RaisedLeadMs(double outputPeriodMs) {
    LeadController lead(kLeadMs);
    for (int i = 0; i < 400; i++) lead.OnGpuBusyAtPresent(Qpc::Freq() * 10 + i, outputPeriodMs);
    return lead.LeadMs();
}

void LeadTests() {
    constexpr double kOutputPeriodMs = 1000.0 / 60.0;
    const int64_t second = Qpc::Freq();
    {
        LeadController lead(kLeadMs);
        for (int i = 0; i < 400; i++) lead.OnGpuBusyAtPresent(second * 10 + i, kOutputPeriodMs);
        const bool capped = std::abs(lead.LeadMs() - (kOutputPeriodMs - kOutputPeriodHeadroomMs)) < 1e-9;
        for (int i = 0; i < 200; i++) lead.OnFrameCost(4.0, i % 2 == 0);
        const bool generatedKnown = lead.GeneratedFrameMs() && std::abs(*lead.GeneratedFrameMs() - 4.75) < 1e-9;
        for (int s = 1; s <= 600; s++) lead.Relax(second * (10 + 4 * s), kOutputPeriodMs);
        const bool relaxedToCost = std::abs(lead.LeadMs() - 4.75) < 1e-9;
        const bool ok = capped && generatedKnown && relaxedToCost;
        printf("%-34s capped=%d generated=%d relaxed=%.2f ms  %s\n", "render lead", capped, generatedKnown, lead.LeadMs(), Verdict(ok));
    }
    {
        constexpr double kContentFps = 60;
        const double outputPeriodMs = 1000.0 / (kContentFps * 2);
        const double ceilingMs = RaisedLeadMs(outputPeriodMs);
        double largestKeptCostMs = 0;
        for (double cost = 4.0; cost <= 10.0; cost += 0.01) {
            MultiplierInputs in;
            in.fgActive = true;
            in.vrr = true;
            in.contentFps = kContentFps;
            in.requested = 2;
            in.current = 2;
            in.maxRefreshHz = 165;
            in.generatedFrameMs = cost;
            in.leadMs = std::min(cost, ceilingMs);
            if (ChooseMultiplier(in).multiplier == 2) largestKeptCostMs = cost;
        }
        const double fixedCeilingMs = RaisedLeadMs(1000.0 / 120.0);
        const bool ok = ceilingMs >= largestKeptCostMs && fixedCeilingMs >= 7.75;
        printf("%-34s vrr x2 at 60 fps: lead up to %.2f ms, x2 kept up to %.2f ms | fixed 120 Hz: lead up to %.2f ms  %s\n", "render lead covers kept x2",
               ceilingMs, largestKeptCostMs, fixedCeilingMs, Verdict(ok));
    }
}

}

int main() {
    SteadyTests();
    MultiplierTests();
    LeadTests();
    CadenceTests();
    PredictionTests();
    ContentTests();
    StartupPathTests();
    ReleaseTests();
    VblankIndexTests();
    return check::Summary();
}
