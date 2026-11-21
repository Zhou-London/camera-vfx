// Offline simulation of the pacing core: a jittery, drifting 60 Hz source feeding
// fixed-refresh (120 / 144 / 164 Hz) and VRR (k = 2, 3) output. Verifies that
//  * the delay controller converges and stays late-free after warm-up,
//  * the steady-state delay matches the theory  D ≈ (1 - t_min)·P + jitter + margin,
//  * content time advances monotonically with smooth speed (drift absorbed),
//  * VRR output lands exactly on the k-grid (t ∈ {0, 1/k, ...}).
#include "pacing/pacer.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>

using namespace camss;

struct Result {
    double delayP50 = 0, delayMax = 0;
    uint64_t lateAfterWarmup = 0, outputs = 0;
    double maxSpeedDev = 0;       // max |dx/dV - 1/P| relative, after warm-up
    double maxGridErr = 0;        // VRR: distance of t from the k-grid
    uint64_t interpolated = 0;
    uint64_t backwards = 0;
};

static double Ms(double ticks) { return ticks * 1000.0 / (double)Qpc::Freq(); }

static Result Simulate(PaceMode mode, double displayHz, int k, double srcHz, double driftPpm, double jitterMs, double seconds,
                       unsigned seed, double spikeProb) {
    const double F = (double)Qpc::Freq();
    const double P = F / (srcHz * (1.0 + driftPpm * 1e-6));
    const double T = F / displayHz;
    const double lead = F * 0.003;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);

    Pacer pacer;
    PacerConfig cfg;
    cfg.mode = mode; cfg.vrrMultiplier = k; cfg.initialDelayMs = 30;
    pacer.Configure(cfg, srcHz);

    // Pre-generate ready times: base + processing 1 ms + jitter, occasional 4 ms spike.
    const int64_t base = (int64_t)(F * 10);
    const int N = (int)(seconds * srcHz) + 10;
    std::vector<int64_t> ready(N);
    for (int n = 0; n < N; n++) {
        double j = U(rng) * jitterMs + (U(rng) < spikeProb ? 4.0 : 0.0);
        ready[n] = base + (int64_t)(n * P + F * (1.0 + j) / 1000.0);
    }
    int fed = 0;
    auto feed = [&](int64_t until) { while (fed < N && ready[fed] <= until) { pacer.OnSourceFrame(fed, ready[fed]); fed++; } };

    Result r;
    std::vector<double> delays;
    const int64_t end = base + (int64_t)(seconds * F);
    const int64_t warm = base + (int64_t)(8 * F);
    double prevX = -1, prevV = 0;
    int64_t lastLate = 0;
    int64_t V = base + (int64_t)T;
    while (V < end) {
        int64_t s;
        if (mode == PaceMode::Vrr) {
            feed(V - (int64_t)lead - (int64_t)T);   // what we know when scheduling
            int64_t nv = pacer.NextVrrDisplayTime(V - (int64_t)lead, 3.0);
            if (!nv) { V += (int64_t)T; continue; }
            V = nv;
        }
        s = V - (int64_t)lead;
        feed(s);
        PaceDecision d = pacer.Decide(s, V);
        if (d.valid) {
            r.outputs++;
            if (getenv("PACE_TRACE") && mode == PaceMode::Vrr && k == 2 && V > base + (int64_t)(30 * F) && V < base + (int64_t)(30.15 * F))
                printf("    V=%.3f x=%.3f nA=%lld nB=%lld t=%.3f late=%d newest=%d D=%.2f minslack=%.2f slack(B)=%.2f\n",
                       Ms((double)(V - base)), d.x, (long long)d.nA, (long long)d.nB, d.t, d.late, fed - 1, d.delayMs,
                       pacer.MinSlackMs(), Ms((double)(s - ready[d.nB])));
            if (getenv("PACE_UPTRACE") && spikeProb == 0 && mode == PaceMode::Vrr) {
                static double lastD = 0;
                static int printed = 0;
                if (d.delayMs > lastD + 0.01 && printed++ < 25)
                    printf("    up: V=%.3f D=%.3f (+%.3f) minslack=%.2f t=%.3f x=%.4f slack(B)=%.2f late=%d\n", Ms((double)(V - base)),
                           d.delayMs, d.delayMs - lastD, pacer.MinSlackMs(), d.t, d.x, Ms((double)(s - ready[d.nB])), d.late);
                lastD = d.delayMs;
            }
            if (getenv("PACE_DTRACE")) {
                static int64_t lastPrint = 0;
                if (V - lastPrint > (int64_t)(5 * F) || V < lastPrint) {
                    lastPrint = V;
                    printf("    [%5.1fs] D=%.2f minslack=%.2f\n", Ms((double)(V - base)) / 1000.0, d.delayMs, pacer.MinSlackMs());
                }
            }
            if (V > warm) {
                delays.push_back(d.delayMs);
                if (d.late) {
                    lastLate = V;
                    r.lateAfterWarmup++;
                    if (getenv("PACE_DEBUG") && r.lateAfterWarmup < 6)
                        printf("    late: V=%.3f s=%.3f x=%.3f newest=%d ready(newest)=%.3f ready(next)=%.3f D=%.2f\n", Ms((double)(V - base)),
                               Ms((double)(s - base)), d.x, fed - 1, Ms((double)(ready[fed - 1] - base)), Ms((double)(ready[fed] - base)), d.delayMs);
                }
                // Speed smoothness, excluding the intentional freeze right after a late frame.
                if (prevX >= 0 && V - lastLate > (int64_t)(0.5 * F)) {
                    double speed = (d.x - prevX) / ((double)(V - prevV) / P);
                    r.maxSpeedDev = std::max(r.maxSpeedDev, std::abs(speed - 1.0));
                }
                if (d.t > 0) r.interpolated++;
                if (mode == PaceMode::Vrr) {
                    double g = d.x * k;
                    r.maxGridErr = std::max(r.maxGridErr, std::abs(g - std::round(g)) / k);
                }
            }
            if (d.x < prevX - 1e-9) { printf("  ERROR: content time went backwards\n"); r.lateAfterWarmup += 1000; }
            prevX = d.x; prevV = (double)V;
        }
        if (mode != PaceMode::Vrr) V += (int64_t)T;
        else V += 1;  // next schedule computed by the pacer
    }
    if (!delays.empty()) {
        std::sort(delays.begin(), delays.end());
        r.delayP50 = delays[delays.size() / 2];
        r.delayMax = delays.back();
    }
    return r;
}

int main() {
    struct Case { const char* name; PaceMode mode; double hz; int k; double drift; double spike; double expectMaxDelay; };
    // D is measured from the fitted ready time of a frame to its display time. Theory:
    //   D = (1 - t_min) * P + lead (3 ms) + ready jitter (1.5 ms, +4 ms spikes) + margin (1 ms)
    // with t_min = smallest interpolation step that needs the next frame
    // (fixed refresh: ~eps for non-integer ratios; VRR k-grid: 1/k).
    const double P = 1000.0 / 60.0;
    const double base = 3.0 + 1.5 + 1.0 + 1.0;   // lead + jitter + margin + tolerance
    Case cases[] = {
        {"fixed 120Hz +80ppm", PaceMode::Fixed, 120.0, 2, 80, 0.01, P + base + 4.0},
        {"fixed 120Hz -30ppm", PaceMode::Fixed, 119.998, 2, -30, 0.01, P + base + 4.0},
        {"fixed 144Hz", PaceMode::Fixed, 144.0, 2, 20, 0.01, P + base + 4.0},
        {"fixed 164Hz (AW2725QF)", PaceMode::Fixed, 164.0, 2, 20, 0.01, P + base + 4.0},
        {"fixed 164Hz, no spikes", PaceMode::Fixed, 164.0, 2, 20, 0.0, P + base},
        {"vrr k=2 (120 fps)", PaceMode::Vrr, 1000.0, 2, 40, 0.01, P / 2 + base + 4.0},
        {"vrr k=2, no spikes", PaceMode::Vrr, 1000.0, 2, 40, 0.0, P / 2 + base},
        {"vrr k=3 (180 fps)", PaceMode::Vrr, 1000.0, 3, 40, 0.01, 2 * P / 3 + base + 4.0},
        // 59.94 Hz source while configured for nominal 60 Hz (-1000 ppm).
        {"vrr k=2, 59.94 Hz source", PaceMode::Vrr, 1000.0, 2, -1000, 0.0, P / 2 + base},
        {"fixed 164Hz, 59.94 Hz src", PaceMode::Fixed, 164.0, 2, -1000, 0.0, P + base},
    };
    int fails = 0;
    for (auto& c : cases) {
        Result r = Simulate(c.mode, c.hz, c.k, 60.0, c.drift, 1.5, 60.0, 1234, c.spike);
        // Allowed lateness: a few hitches per minute caused by the injected 4 ms spikes.
        bool ok = r.lateAfterWarmup <= 12 && r.backwards == 0 && r.delayP50 <= c.expectMaxDelay && r.maxSpeedDev < 0.03 &&
                  (c.mode != PaceMode::Vrr || r.maxGridErr < 0.02);
        printf("%-24s out=%6llu interp=%6llu late=%2llu back=%llu  D p50=%6.2f max=%6.2f (bound %5.2f) ms  speed dev=%.4f  grid err=%.4f  %s\n",
               c.name, (unsigned long long)r.outputs, (unsigned long long)r.interpolated, (unsigned long long)r.lateAfterWarmup,
               (unsigned long long)r.backwards, r.delayP50, r.delayMax, c.expectMaxDelay, r.maxSpeedDev, r.maxGridErr,
               ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
