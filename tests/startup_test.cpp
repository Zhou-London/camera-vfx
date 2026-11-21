// Regression test from a real PS5 start-up (static screen -> ~50 fps menu transition -> 30 fps game),
// VRR, fg-target-fps 60, render lead 10.7 ms. The cadence retime used to land on top of the transient
// delay of the irregular transition (D 46 ms instead of 28.5) and took > 12 s to recover.
// Pass: D within 1.5 ms of its steady state 8 s after the switch to 30 fps, no late frame after it.
//   startup_test [variant ones transitionEnd] [-v]
#include "pacing/pacer.h"
#include "pacing/cadence.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <cstring>
#include <cstdlib>

using namespace camss;

static int g_ones = 6, g_transEnd = 130;
static bool UniqueAt(int64_t n, int variant) {
    if (n < 88) return n == 0;                       // static screen
    if (n < g_transEnd) {                            // transition: g_ones x interval 1, then one interval 2
        int m = (int)((n - 88) % (g_ones + 1));
        return m != g_ones;
    }
    return ((n + variant) % 2) == 0;                 // 30 fps game, phase variant 0/1
}

int main(int argc, char** argv) {
    const int variant = argc > 1 ? atoi(argv[1]) : 0;
    const double leadMs = 10.7;
    bool verbose = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = true;
    if (argc > 2 && strcmp(argv[2], "-v")) g_ones = atoi(argv[2]);
    if (argc > 3 && strcmp(argv[3], "-v")) g_transEnd = atoi(argv[3]);
    const double F = (double)Qpc::Freq();
    const double P = F / 59.94;
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> U(0, 1);
    Pacer pacer;
    PacerConfig cfg;
    cfg.mode = PaceMode::Vrr; cfg.vrrMultiplier = 1;
    cfg.initialDelayMs = 7.0;   // app: (k-1)/k*P + 7 with k = 1
    pacer.Configure(cfg, 60.0);
    CadenceDetector cad;
    const int64_t base = (int64_t)(F * 10);
    const int N = 60 * 40;
    std::vector<int64_t> ready(N);
    for (int n = 0; n < N; n++) ready[n] = base + (int64_t)(n * P + F * (1.0 + 0.2 * U(rng)) / 1000.0);
    int fed = 0;
    double switchT = -1;
    auto feed = [&](int64_t until) {
        while (fed < N && ready[fed] <= until) {
            const bool u = UniqueAt(fed, variant);
            pacer.OnSourceFrame(fed, ready[fed], u);
            if (cad.OnFrame(fed, u)) {
                const double pc = cad.ContentPeriod();
                const double contentFps = 59.94 / pc;
                int k = (int)std::lround(60.0 / contentFps);
                k = std::max(1, std::min(k, 8));
                pacer.SetContentPeriod(pc);
                pacer.SetMultiplier(k);
                if (pc >= 2.0) switchT = (double)(ready[fed] - base) / F;
                if (verbose) printf("%7.3f s  cadence -> Pc %.2f, k %d, D %.2f\n", (ready[fed] - base) / F, pc, k, pacer.DelayMs());
            }
            fed++;
        }
    };
    int64_t now = base;
    const int64_t lead = (int64_t)(F * leadMs / 1000.0);
    double lastPrint = -1;
    uint64_t lates = 0, latesAfter = 0;
    double dAt8 = -1, dEnd = 0;
    while (now < base + (int64_t)(F * 38)) {
        feed(now);
        int64_t V = pacer.NextVrrDisplayTime(now, leadMs);
        if (!V) { now += (int64_t)(F * 0.005); continue; }
        int64_t s = V - lead;
        if (s < now) s = now;
        feed(s);
        PaceDecision d = pacer.Decide(s, V);
        const double ts = (V - base) / F;
        if (d.late) { lates++; if (switchT > 0 && ts > switchT + 1.0) latesAfter++; }
        if (switchT > 0 && dAt8 < 0 && ts > switchT + 8.0) dAt8 = d.delayMs;
        dEnd = d.delayMs;
        if (verbose && (ts - lastPrint >= 2.0 || (ts > 1.5 && ts < 4.5 && ts - lastPrint >= 0.25))) {
            printf("%7.3f s  x %.3f nA %lld nB %lld t %.3f  D %.2f minslack %.2f lates %llu stall %llu\n", ts, d.x, (long long)d.nA,
                   (long long)d.nB, d.t, d.delayMs, pacer.MinSlackMs(), (unsigned long long)lates, (unsigned long long)pacer.StallCount());
            lastPrint = ts;
        }
        now = V;   // present at V, next iteration
    }
    const bool ok = switchT > 0 && dAt8 > 0 && dAt8 < dEnd + 1.5 && latesAfter == 0;
    printf("start-up %d/%d/%d: switch to 30 fps at %.2f s, D 8 s later %.2f ms, steady %.2f ms, lates %llu (after switch %llu)  %s\n",
           variant, g_ones, g_transEnd, switchT, dAt8, dEnd, (unsigned long long)lates, (unsigned long long)latesAfter, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
