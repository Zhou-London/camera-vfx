#include "check.h"
#include "pacing/cadence.h"
#include "pacing/pacer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace cameravfx;

namespace {

constexpr double kSignalHz = 59.94;
constexpr double kNominalHz = 60.0;
constexpr double kTargetOutputFps = 60.0;
constexpr double kLeadMs = 10.7;
constexpr int64_t kStaticUntil = 88;
constexpr int64_t kTransitionUntil = 130;
constexpr int kTransitionRun = 6;
constexpr double kSeconds = 38;
constexpr double kSettleSec = 8.0;
constexpr double kSteadyToleranceMs = 1.5;
constexpr double kLateGraceSec = 1.0;
constexpr double kMarginMs = 1.0;
constexpr double kBaseDelayMs = 7.0;
constexpr double kSnap = 0.06;

bool UniqueAt(int64_t n, int phase) {
    if (n < kStaticUntil) return n == 0;
    if (n < kTransitionUntil) return (n - kStaticUntil) % (kTransitionRun + 1) != kTransitionRun;
    return (n + phase) % 2 == 0;
}

void RunStartup(int phase) {
    const double freq = (double)Qpc::Freq();
    const double period = freq / kSignalHz;
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> uniform(0, 1);
    PacerConfig cfg;
    cfg.mode = PaceMode::Vrr;
    cfg.multiplier = 1;
    cfg.fg = true;
    cfg.snapEps = kSnap;
    cfg.marginMs = kMarginMs;
    cfg.baseDelayMs = kBaseDelayMs;
    Pacer pacer;
    pacer.Configure(cfg, kNominalHz);
    CadenceDetector cadence;
    const int64_t base = (int64_t)(freq * 10);
    const int frames = (int)(kNominalHz * (kSeconds + 2));
    std::vector<int64_t> ready(frames);
    for (int n = 0; n < frames; n++) ready[n] = base + (int64_t)(n * period + freq * (1.0 + 0.2 * uniform(rng)) / 1000.0);
    int fed = 0;
    double switchSec = -1;
    auto feed = [&](int64_t until) {
        for (; fed < frames && ready[fed] <= until; fed++) {
            const bool unique = UniqueAt(fed, phase);
            pacer.OnSourceFrame(fed, ready[fed], unique);
            if (!cadence.OnFrame(fed, unique)) continue;
            const double contentPeriod = cadence.ContentPeriod();
            pacer.SetContentPeriod(contentPeriod);
            pacer.SetMultiplier(std::clamp((int)std::lround(kTargetOutputFps / (kSignalHz / contentPeriod)), 1, 8));
            if (contentPeriod >= 2.0) switchSec = (double)(ready[fed] - base) / freq;
        }
    };
    const int64_t lead = Qpc::FromMs(kLeadMs);
    int64_t now = base;
    uint64_t lates = 0, latesAfterSwitch = 0;
    double settledDelay = -1, finalDelay = 0;
    while (now < base + (int64_t)(freq * kSeconds)) {
        feed(now);
        const int64_t displayTime = pacer.NextVrrDisplayTime(now, kLeadMs);
        if (!displayTime) { now += Qpc::FromMs(5.0); continue; }
        const int64_t renderStart = std::max(now, displayTime - lead);
        feed(renderStart);
        const PaceDecision d = pacer.Decide(renderStart, displayTime);
        const double sec = (double)(displayTime - base) / freq;
        if (d.late) {
            lates++;
            if (switchSec > 0 && sec > switchSec + kLateGraceSec) latesAfterSwitch++;
        }
        if (switchSec > 0 && settledDelay < 0 && sec > switchSec + kSettleSec) settledDelay = d.delayMs;
        finalDelay = d.delayMs;
        now = displayTime;
    }
    const bool ok = switchSec > 0 && settledDelay > 0 && settledDelay < finalDelay + kSteadyToleranceMs && latesAfterSwitch == 0;
    printf("start-up phase %d: switch to 30 fps at %.2f s, D 8 s later %.2f ms, steady %.2f ms, lates %llu (after switch %llu)  %s\n", phase,
           switchSec, settledDelay, finalDelay, (unsigned long long)lates, (unsigned long long)latesAfterSwitch, check::Verdict(ok));
}

}

int main() {
    for (int phase : {0, 1}) RunStartup(phase);
    return check::Summary();
}
