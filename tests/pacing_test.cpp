// Offline simulation of the pacing core: a jittery, drifting 60 Hz source feeding
// fixed-refresh (120 / 144 / 164 Hz) and VRR (k = 2, 3) output. Verifies that
//  * the delay controller converges and stays late-free after warm-up,
//  * the steady-state delay matches the theory  D ≈ (1 - t_min)·P + jitter + margin,
//  * content time advances monotonically with smooth speed (drift absorbed),
//  * VRR output lands exactly on the k-grid (t ∈ {0, 1/k, ...}).
// Content cadence (a 60 Hz signal carrying 30 fps / 24p / 40 fps content):
//  * CadenceDetector estimates the content period with hysteresis (no flapping, no change on static scenes),
//  * output = k x content rate, D ≈ (k-1)/k·Pc·P + lead + jitter + margin, t on the content grid,
//  * 60 -> 30 -> 60 switches retime D with few late frames, static scenes are stalls (D not raised).
#include "pacing/pacer.h"
#include "pacing/cadence.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
    double delayBeforeStall = 0, delayMaxAfterStall = 0;
    uint64_t stalls = 0;
};

static double Ms(double ticks) { return ticks * 1000.0 / (double)Qpc::Freq(); }

static Result Simulate(PaceMode mode, double displayHz, int k, double srcHz, double driftPpm, double jitterMs, double seconds,
                       unsigned seed, double spikeProb, double stallAtSec = -1) {
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
    // Optional 250 ms source stall: frames due in the window arrive in a burst at its end.
    if (stallAtSec > 0) {
        const int64_t a = base + (int64_t)(stallAtSec * F), b = a + (int64_t)(0.25 * F);
        for (int n = 0; n < N; n++) if (ready[n] >= a && ready[n] < b) ready[n] = b + n % 3;
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
            if (stallAtSec > 0) {
                const double tsec = (double)(V - base) / F;
                if (tsec < stallAtSec) r.delayBeforeStall = d.delayMs;
                else if (tsec < stallAtSec + 10) r.delayMaxAfterStall = std::max(r.delayMaxAfterStall, d.delayMs);
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
    r.stalls = pacer.StallCount();
    if (!delays.empty()) {
        std::sort(delays.begin(), delays.end());
        r.delayP50 = delays[delays.size() / 2];
        r.delayMax = delays.back();
    }
    return r;
}

// ======================================================================= content cadence
// A 60 Hz signal carrying content at a lower rate: each signal frame is either new content
// ("unique") or a repeat of the previous one.
using Pattern = std::function<bool(int64_t n)>;
static bool Pat60(int64_t) { return true; }
static bool Pat30(int64_t n) { return n % 2 == 0; }                         // 30 fps: every frame twice
static bool Pat24(int64_t n) { int64_t r = n % 5; return r == 0 || r == 3; } // 24p 3:2 pulldown (intervals 3,2)
static bool Pat40(int64_t n) { int64_t r = n % 3; return r != 1; }           // 40 fps (intervals 2,1)
static bool Pat45(int64_t n) { return n % 4 != 3; }                          // ~45 fps (intervals 1,1,2)

struct CadRun {
    int switches = 0;
    double final = 1.0;
    std::vector<std::pair<int64_t, double>> sw;   // (frame, new period)
};

// Feed `frames` signal frames; `received` (optional) drops frames that return false.
static CadRun RunCadence(const Pattern& uniq, int64_t frames, const Pattern& received = nullptr, const CadenceConfig& cfg = {}) {
    CadenceDetector det(cfg);
    CadRun r;
    for (int64_t n = 0; n < frames; n++) {
        if (received && !received(n)) continue;
        if (det.OnFrame(n, uniq(n))) { r.switches++; r.sw.push_back({n, det.ContentPeriod()}); }
    }
    r.final = det.ContentPeriod();
    return r;
}

static int CadenceTests() {
    int fails = 0;
    const CadenceConfig cfg;
    auto report = [&](const char* name, const CadRun& r, bool ok, const char* extra = "") {
        printf("%-34s active=%.3f switches=%d", name, r.final, r.switches);
        for (size_t i = 0; i < r.sw.size() && i < 4; i++) printf(" [@%lld -> %.3f]", (long long)r.sw[i].first, r.sw[i].second);
        printf("%s  %s\n", extra, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    };
    { CadRun r = RunCadence(Pat60, 1200); report("cadence 60 fps", r, r.final == 1.0 && r.switches == 0); }
    { CadRun r = RunCadence(Pat30, 1200); report("cadence 30-in-60", r, r.final == 2.0 && r.switches == 1); }
    { CadRun r = RunCadence(Pat24, 1200); report("cadence 24p 3:2", r, r.final == 2.5 && r.switches == 1); }
    { CadRun r = RunCadence(Pat40, 1200); report("cadence 40 fps (2:1)", r, r.final == 1.5 && r.switches == 1); }
    { CadRun r = RunCadence(Pat45, 1200); report("cadence ~45 fps (1,1,2)", r, std::abs(r.final - 4.0 / 3.0) < 0.05 && r.switches == 1); }
    {   // 60 fps with a 1 s static pause (no unique frames for 60 signal frames).
        CadRun r = RunCadence([](int64_t n) { return n < 600 || n >= 660; }, 1200);
        report("cadence 60 + 1 s static", r, r.final == 1.0 && r.switches == 0);
    }
    {   // 60 fps with a 5 s static pause (window empties completely).
        CadRun r = RunCadence([](int64_t n) { return n < 600 || n >= 900; }, 1500);
        report("cadence 60 + 5 s static", r, r.final == 1.0 && r.switches == 0);
    }
    {   // Alternating 30 / 60 fps segments shorter than the hold: never flap back and forth.
        for (int seg : {20, 30}) {
            CadRun r = RunCadence([seg](int64_t n) { return ((n / seg) % 2 == 0) ? true : (n % 2 == 0); }, 3600);
            char name[64];
            snprintf(name, sizeof(name), "cadence 30/60 alternating %d fr", seg);
            report(name, r, r.switches <= 1);
        }
    }
    for (int dir = 0; dir < 2; dir++) {
        // Switch latency: 60 -> 30 (and 30 -> 60) at frame 1200.
        const int64_t at = 1200;
        Pattern p = dir == 0 ? Pattern([at](int64_t n) { return n < at ? true : n % 2 == 0; })
                             : Pattern([at](int64_t n) { return n < at ? n % 2 == 0 : true; });
        CadRun r = RunCadence(p, 2400);
        const int64_t lat = r.sw.empty() ? -1 : r.sw.back().first - at;
        const bool ok = r.final == (dir == 0 ? 2.0 : 1.0) && r.switches == (dir == 0 ? 1 : 2) && lat >= cfg.holdFrames &&
                        lat <= cfg.holdFrames + cfg.windowFrames / 2 + 2;
        char extra[64];
        snprintf(extra, sizeof(extra), "  latency %lld frames (%.0f ms)", (long long)lat, lat * 1000.0 / 60.0);
        report(dir == 0 ? "cadence 60->30 switch" : "cadence 30->60 switch", r, ok, extra);
    }
    {   // 30-in-60 with 3% random drops and a 100-frame signal gap: stays at 2.
        std::mt19937 rng(7);
        std::vector<bool> rx(3000);
        for (auto&& v : rx) v = (rng() % 100) >= 3;
        for (int i = 1500; i < 1600; i++) rx[i] = false;
        CadRun r = RunCadence(Pat30, 3000, [&](int64_t n) { return (bool)rx[(size_t)n]; });
        report("cadence 30-in-60, drops + gap", r, r.final == 2.0 && r.switches == 1);
    }
    {   // 24p with drops: stays at 2.5.
        std::mt19937 rng(9);
        std::vector<bool> rx(3000);
        for (auto&& v : rx) v = (rng() % 100) >= 2;
        CadRun r = RunCadence(Pat24, 3000, [&](int64_t n) { return (bool)rx[(size_t)n]; });
        report("cadence 24p 3:2, 2% drops", r, r.final == 2.5 && r.switches == 1);
    }
    return fails;
}

// The needed-next-frame rule before the next content frame has arrived must match the snap rule it
// gets once it arrives. 24p 3:2 (Pc = 2.5): unique 290 starts a 3-interval (next unique 293). With
// repeats 291, 292 received, x = 290.165 snaps to A once 293 is here (t = 0.055 <= eps), so it is
// not late; and one late content frame is penalised once, even when its predicted index moves
// (290 + 2.5 -> 293) because a repeat arrived where it was expected.
static int PredictionTests() {
    int fails = 0;
    const double F = (double)Qpc::Freq(), P = F / 60.0;
    const int64_t base = (int64_t)(F * 10);
    auto mk = [&](Pacer& p, int64_t last, double delayMs) {   // perfect 60 Hz timing, frames 0..last received
        PacerConfig cfg;
        cfg.mode = PaceMode::Fixed; cfg.initialDelayMs = delayMs;
        p.SetContentPeriod(2.5);   // before Configure (which keeps Pc), so D starts at exactly delayMs
        p.Configure(cfg, 60.0);
        for (int64_t n = 0; n <= last; n++) p.OnSourceFrame(n, base + (int64_t)std::llround((double)n * P), Pat24(n));
    };
    auto at = [&](Pacer& p, double x) {   // decide for content position x (render starts 3 ms before V)
        const int64_t V = base + (int64_t)std::llround(x * P + (double)Qpc::FromMs(p.DelayMs()));
        return p.Decide(V - Qpc::FromMs(3.0), V);
    };
    {
        Pacer p; mk(p, 292, 50.0);                            // render starts after 292 arrived, before 293
        PaceDecision d = at(p, 290.165);
        const bool ok = d.valid && !d.late && !d.stall && d.nA == 290 && d.t == 0 && p.LateCount() == 0;
        printf("%-34s x=%.3f nA=%lld t=%.3f late=%d lates=%llu  %s\n", "predict 24p: snaps to A, not late", d.x, (long long)d.nA, d.t,
               d.late, (unsigned long long)p.LateCount(), ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    {
        Pacer p; mk(p, 291, 25.0);                            // render starts before 292 arrives
        PaceDecision d1 = at(p, 290.3);                       // 293 needed, expected at 292.5: late
        p.OnSourceFrame(292, base + (int64_t)std::llround(292.0 * P), false);
        PaceDecision d2 = at(p, 290.6);                       // same content frame, now expected at 293
        const bool ok = d1.late && !d1.stall && d2.late && !d2.stall && p.LateCount() == 1;
        printf("%-34s late=%d,%d penalties=%llu  %s\n", "predict 24p: one penalty per frame", d1.late, d2.late,
               (unsigned long long)p.LateCount(), ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    return fails;
}

// Pacer driven by content at a lower rate than the signal.
struct ContentCase {
    const char* name;
    PaceMode mode;
    double hz;              // display Hz (VRR: scheduler granularity)
    int k;
    std::function<bool(int64_t n, double tsec)> uniq;
    double pc = 1.0;        // content period set up front (detector == 0)
    int detector = 0;       // 1: CadenceDetector -> SetContentPeriod; 2: also SetMultiplier for a constant 120 fps target
    double spike = 0.0;
    double stallAtSec = -1;
    double seconds = 60;
};

struct ContentResult {
    uint64_t outputs = 0, interpolated = 0, lates = 0, stalls = 0, backwards = 0;
    double outFps = 0;
    double delayP50 = 0, delayMax = 0, latP50 = 0;
    double maxGridErr = 0;
    double maxMotionStep = 0;     // largest per-output change of the shown motion (in content frames)
    std::vector<double> lateT;    // seconds
    std::vector<std::pair<double, double>> delays;   // (sec, D)
    std::vector<std::pair<double, int>> interpT;     // (sec, interpolated?)
    double delayBefore = 0, delayMaxAfter = 0;      // around stallAtSec / a static scene
    uint64_t stallsDuring = 0;
};

static double P50(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static ContentResult SimulateContent(const ContentCase& c, double eventSec = -1) {
    const double F = (double)Qpc::Freq();
    const double srcHz = 60.0, jitterMs = 1.5, driftPpm = 20;
    const double P = F / (srcHz * (1.0 + driftPpm * 1e-6));
    const double T = F / c.hz;
    const double lead = F * 0.003;
    std::mt19937 rng(4321);
    std::uniform_real_distribution<double> U(0.0, 1.0);

    Pacer pacer;
    PacerConfig cfg;
    cfg.mode = c.mode; cfg.vrrMultiplier = c.k; cfg.initialDelayMs = 30;
    pacer.Configure(cfg, srcHz);
    if (!c.detector) pacer.SetContentPeriod(c.pc);
    CadenceDetector det;

    const int64_t base = (int64_t)(F * 10);
    const int N = (int)(c.seconds * srcHz) + 10;
    std::vector<int64_t> ready(N);
    std::vector<char> uniq(N);
    std::vector<double> motion(N);   // content frames shown up to n (motion position of frame n)
    double m = -1;
    for (int n = 0; n < N; n++) {
        double j = U(rng) * jitterMs + (U(rng) < c.spike ? 4.0 : 0.0);
        ready[n] = base + (int64_t)(n * P + F * (1.0 + j) / 1000.0);
        uniq[n] = (n == 0 || c.uniq(n, n / srcHz)) ? 1 : 0;
        if (uniq[n]) m += 1;
        motion[n] = m;
    }
    if (c.stallAtSec > 0) {
        const int64_t a = base + (int64_t)(c.stallAtSec * F), b = a + (int64_t)(0.25 * F);
        for (int n = 0; n < N; n++) if (ready[n] >= a && ready[n] < b) ready[n] = b + n % 3;
    }
    int fed = 0;
    auto feed = [&](int64_t until) {
        while (fed < N && ready[fed] <= until) {
            const bool u = uniq[fed] != 0;
            pacer.OnSourceFrame(fed, ready[fed], u);
            if (c.detector && det.OnFrame(fed, u)) {
                pacer.SetContentPeriod(det.ContentPeriod());
                if (c.detector == 2) pacer.SetMultiplier(std::max(1, (int)std::lround(120.0 / (srcHz / det.ContentPeriod()))));
            }
            fed++;
        }
    };

    ContentResult r;
    std::vector<double> lat;
    const int64_t end = base + (int64_t)(c.seconds * F);
    const int64_t warm = base + (int64_t)(8 * F);
    double prevCx = -1, prevMotion = -1;
    int64_t V = base + (int64_t)T;
    while (V < end) {
        if (c.mode == PaceMode::Vrr) {
            feed(V - (int64_t)lead - (int64_t)T);
            int64_t nv = pacer.NextVrrDisplayTime(V - (int64_t)lead, 3.0);
            if (!nv) { V += (int64_t)T; continue; }
            V = nv;
        }
        const int64_t s = V - (int64_t)lead;
        feed(s);
        const uint64_t stallsBefore = pacer.StallCount();
        PaceDecision d = pacer.Decide(s, V);
        if (d.valid) {
            const double tsec = (double)(V - base) / F;
            if (!uniq[(size_t)d.nA] || !uniq[(size_t)d.nB]) { printf("  ERROR: repeat frame used\n"); r.backwards += 1000; }
            if (d.cx < prevCx - 1e-9) { printf("  ERROR: content time went backwards (%.3f -> %.3f)\n", prevCx, d.cx); r.backwards++; }
            const double mo = d.t > 0 ? motion[(size_t)d.nA] + d.t * (motion[(size_t)d.nB] - motion[(size_t)d.nA]) : motion[(size_t)d.nA];
            if (mo < prevMotion - 1e-9) { printf("  ERROR: motion went backwards\n"); r.backwards++; }
            if (eventSec > 0) {
                if (tsec < eventSec) r.delayBefore = d.delayMs;
                else if (tsec < eventSec + 10) {
                    r.delayMaxAfter = std::max(r.delayMaxAfter, d.delayMs);
                    r.stallsDuring += pacer.StallCount() - stallsBefore;
                }
            }
            if (V > warm) {
                r.outputs++;
                r.delays.push_back({tsec, d.delayMs});
                r.interpT.push_back({tsec, d.t > 0 ? 1 : 0});
                if (d.late && !d.stall) {
                    r.lates++;
                    r.lateT.push_back(tsec);
                    if (getenv("PACE_DEBUG") && r.lates < 8)
                        printf("    late: t=%.3fs x=%.3f nA=%lld newest signal=%d (unique %d) ready(next)=%+.2f ms vs s, D=%.2f minslack=%.2f\n",
                               tsec, d.x, (long long)d.nA, fed - 1, (int)uniq[(size_t)fed - 1], Ms((double)(ready[(size_t)fed] - s)),
                               d.delayMs, pacer.MinSlackMs());
                }
                if (d.t > 0) r.interpolated++;
                if (!d.stall) lat.push_back(Ms((double)(V - ready[(size_t)(d.t > 0 ? d.nB : d.nA)])));
                if (c.mode == PaceMode::Vrr) {
                    // On the content grid t is a multiple of 1/k (exact for integer Pc).
                    const double g = d.t * pacer.Multiplier();
                    r.maxGridErr = std::max(r.maxGridErr, std::abs(g - std::round(g)) / pacer.Multiplier());
                }
                if (prevMotion >= 0) r.maxMotionStep = std::max(r.maxMotionStep, mo - prevMotion);
                if (getenv("PACE_CTRACE") && tsec > atof(getenv("PACE_CTRACE")) && tsec < atof(getenv("PACE_CTRACE")) + 0.15)
                    printf("    V=%.3f x=%.3f cx=%.3f nA=%lld nB=%lld t=%.3f late=%d stall=%d sig=%d D=%.2f minslack=%.2f\n", tsec, d.x, d.cx,
                           (long long)d.nA, (long long)d.nB, d.t, d.late, d.stall, fed - 1, d.delayMs, pacer.MinSlackMs());
            }
            prevCx = d.cx; prevMotion = mo;
        }
        if (c.mode != PaceMode::Vrr) V += (int64_t)T;
        else V += 1;
    }
    r.stalls = pacer.StallCount();
    r.outFps = (double)r.outputs / (c.seconds - 8.0);
    std::vector<double> ds;
    for (auto& p : r.delays) ds.push_back(p.second);
    r.delayP50 = P50(ds);
    r.delayMax = ds.empty() ? 0 : *std::max_element(ds.begin(), ds.end());
    r.latP50 = P50(lat);
    return r;
}

static double DelayP50In(const ContentResult& r, double a, double b) {
    std::vector<double> v;
    for (auto& p : r.delays) if (p.first >= a && p.first < b) v.push_back(p.second);
    return P50(v);
}

static int ContentTests() {
    int fails = 0;
    const double P = 1000.0 / 60.0;
    const double lead = 3.0, margin = 1.0;
    auto theoryVrr = [&](double pc, int k) { return (double)(k - 1) / k * pc * P + lead + margin; };
    auto p30 = [](int64_t n, double) { return Pat30(n); };
    auto p24 = [](int64_t n, double) { return Pat24(n); };

    // Steady cadences with Pc set up front. Theory: (k-1)/k * Pc * P + lead + margin (VRR);
    // fixed refresh: up to a whole content period (all phases occur). Bound: theory + jitter (1.5)
    // + tolerance (1.0). 3:2 pulldown: content frames arrive off the Pc grid (intervals 3,2); with
    // the grid anchored at the start of a 3-interval the worst wait is 1.75 P (k = 2).
    struct Steady { ContentCase c; double theory; double bound; double expectFps; bool gridCheck; };
    Steady cases[] = {
        {{"30-in-60 vrr k=2 (60 fps)", PaceMode::Vrr, 1000.0, 2, p30, 2.0}, theoryVrr(2, 2), theoryVrr(2, 2) + 2.5, 60, true},
        {{"30-in-60 vrr k=4 (120 fps)", PaceMode::Vrr, 1000.0, 4, p30, 2.0}, theoryVrr(2, 4), theoryVrr(2, 4) + 2.5, 120, true},
        {{"30-in-60 fixed 120Hz", PaceMode::Fixed, 120.0, 2, p30, 2.0}, 2 * P + lead + margin, 2 * P + lead + margin + 2.5, 120, false},
        {{"30-in-60 fixed 165Hz", PaceMode::Fixed, 165.0, 2, p30, 2.0}, 2 * P + lead + margin, 2 * P + lead + margin + 2.5, 165, false},
        {{"24p 3:2 vrr k=2 (48 fps)", PaceMode::Vrr, 1000.0, 2, p24, 2.5}, 1.75 * P + lead + margin, 1.75 * P + lead + margin + 2.5, 48, false},
        {{"30-in-60 vrr k=2, 1% spikes", PaceMode::Vrr, 1000.0, 2, p30, 2.0, 0, 0.01}, theoryVrr(2, 2), theoryVrr(2, 2) + 2.5 + 4.0, 60, true},
    };
    for (auto& sc : cases) {
        ContentResult r = SimulateContent(sc.c);
        const bool ok = r.lates <= (sc.c.spike > 0 ? 20u : 0u) && r.backwards == 0 && r.delayP50 <= sc.bound &&
                        std::abs(r.outFps - sc.expectFps) < 0.02 * sc.expectFps && (!sc.gridCheck || r.maxGridErr < 0.01) &&
                        r.maxMotionStep < 1.01;
        printf("%-30s out %6.1f fps interp=%5llu late=%2llu back=%llu  D p50=%6.2f max=%6.2f (theory %5.2f, bound %5.2f) ms  "
               "arrival->display p50 %5.2f ms  grid err=%.4f  motion step<=%.2f  %s\n",
               sc.c.name, r.outFps, (unsigned long long)r.interpolated, (unsigned long long)r.lates, (unsigned long long)r.backwards,
               r.delayP50, r.delayMax, sc.theory, sc.bound, r.latP50, r.maxGridErr, r.maxMotionStep, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }

    // 60 -> 30 -> 60 driven by the CadenceDetector (content changes at 20 s and 40 s).
    for (int det : {1, 2}) {
        ContentCase c{det == 1 ? "switch 60->30->60 vrr k=2" : "switch 60->30->60 vrr 120 fps", PaceMode::Vrr, 1000.0, 2,
                      [](int64_t n, double t) { return (t >= 20 && t < 40) ? Pat30(n) : true; }, 1.0, det};
        ContentResult r = SimulateContent(c);
        int nearSw = 0, elsewhere = 0;
        for (double t : r.lateT) ((t >= 20 && t < 23) || (t >= 40 && t < 43)) ? nearSw++ : elsewhere++;
        const int k30 = det == 1 ? 2 : 4;
        const double d60a = DelayP50In(r, 15, 20), d30 = DelayP50In(r, 35, 40), d60b = DelayP50In(r, 55, 60);
        const double t60 = theoryVrr(1, 2), t30 = theoryVrr(2, k30);
        const bool ok = nearSw <= 6 && elsewhere == 0 && r.backwards == 0 && d60a - t60 < 2.5 && d60a > t60 - 0.5 && d30 - t30 < 2.5 &&
                        d30 > t30 - 0.5 && d60b - t60 < 2.5 && d60b > t60 - 0.5;
        printf("%-30s late near switches=%d elsewhere=%d back=%llu stalls=%llu  D p50 60fps %.2f (theory %.2f) | 30fps %.2f (theory %.2f) | "
               "60fps %.2f  %s\n", c.name, nearSw, elsewhere, (unsigned long long)r.backwards, (unsigned long long)r.stalls, d60a, t60, d30,
               t30, d60b, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    {   // Same in fixed refresh (164 Hz).
        ContentCase c{"switch 60->30->60 fixed 164", PaceMode::Fixed, 164.0, 2,
                      [](int64_t n, double t) { return (t >= 20 && t < 40) ? Pat30(n) : true; }, 1.0, 1};
        ContentResult r = SimulateContent(c);
        int nearSw = 0, elsewhere = 0;
        for (double t : r.lateT) ((t >= 20 && t < 23) || (t >= 40 && t < 43)) ? nearSw++ : elsewhere++;
        const double d60a = DelayP50In(r, 15, 20), d30 = DelayP50In(r, 35, 40), d60b = DelayP50In(r, 55, 60);
        const bool ok = nearSw <= 6 && elsewhere == 0 && r.backwards == 0 && d60a < P + 6.5 && d30 < 2 * P + 6.5 && d60b < P + 6.5;
        printf("%-30s late near switches=%d elsewhere=%d back=%llu stalls=%llu  D p50 60fps %.2f | 30fps %.2f | 60fps %.2f  %s\n", c.name,
               nearSw, elsewhere, (unsigned long long)r.backwards, (unsigned long long)r.stalls, d60a, d30, d60b, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }

    // 1 s static scene in 60 fps content (no new frames from 20 s to 21 s, repeats keep coming).
    for (PaceMode mode : {PaceMode::Vrr, PaceMode::Fixed}) {
        ContentCase c{mode == PaceMode::Vrr ? "static 1 s (vrr k=2)" : "static 1 s (fixed 164)", mode, mode == PaceMode::Vrr ? 1000.0 : 164.0,
                      2, [](int64_t, double t) { return !(t >= 20 && t < 21); }, 1.0};
        ContentResult r = SimulateContent(c, 20.0);
        uint64_t interpAfter = 0;
        for (auto& p : r.interpT) if (p.first > 21.5 && p.first < 30) interpAfter += p.second;
        const bool ok = r.delayMaxAfter - r.delayBefore < 1.0 && r.stallsDuring > 0 && r.lates == 0 && r.backwards == 0 &&
                        interpAfter > 100 && r.maxMotionStep < 0.76;
        printf("%-30s D before %.2f ms, max within 10 s after %.2f ms, stall outputs %llu, late %llu, interp after %llu, "
               "max motion step %.2f frames  %s\n", c.name, r.delayBefore, r.delayMaxAfter, (unsigned long long)r.stallsDuring,
               (unsigned long long)r.lates, (unsigned long long)interpAfter, r.maxMotionStep, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    {   // 30 fps content whose cadence phase shifts (a game hitch: one frame shown 3 times at 20 s), VRR k=3:
        // the grid must re-anchor on the new phase, otherwise no output shows a real frame and D grows by Pc*P/(2k).
        ContentCase c{"30-in-60 phase shift vrr k=3", PaceMode::Vrr, 1000.0, 3,
                      [](int64_t n, double t) { return t < 20 ? Pat30(n) : !Pat30(n); }, 2.0};
        ContentResult r = SimulateContent(c);
        const double before = DelayP50In(r, 12, 20), after = DelayP50In(r, 30, 60), th = theoryVrr(2, 3);
        const bool ok = r.lates <= 2 && r.backwards == 0 && before - th < 2.5 && after - th < 2.5;
        printf("%-30s late=%llu back=%llu  D p50 before %.2f | after %.2f (theory %.2f)  %s\n", c.name, (unsigned long long)r.lates,
               (unsigned long long)r.backwards, before, after, th, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    {   // 250 ms source stall with 30 fps content.
        ContentCase c{"stall 250ms (30-in-60 vrr k=2)", PaceMode::Vrr, 1000.0, 2, p30, 2.0, 0, 0.0, 15.0, 30};
        ContentResult r = SimulateContent(c, 15.0);
        const bool ok = r.delayMaxAfter - r.delayBefore < 3.0 && r.backwards == 0 && r.stalls > 0;
        printf("%-30s D before %.2f ms, max within 10 s after %.2f ms, stall outputs %llu, late %llu  %s\n", c.name, r.delayBefore,
               r.delayMaxAfter, (unsigned long long)r.stalls, (unsigned long long)r.lates, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    return fails;
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
        // Allowed lateness: injected 4 ms spikes on 1% of frames (far worse than the ~0.2 ms p99
        // jitter measured on the real card) may cause up to 20 hitches/min; clean sources: none.
        bool ok = r.lateAfterWarmup <= (c.spike > 0 ? 20u : 0u) && r.backwards == 0 && r.delayP50 <= c.expectMaxDelay && r.maxSpeedDev < 0.03 &&
                  (c.mode != PaceMode::Vrr || r.maxGridErr < 0.02);
        printf("%-24s out=%6llu interp=%6llu late=%2llu back=%llu  D p50=%6.2f max=%6.2f (bound %5.2f) ms  speed dev=%.4f  grid err=%.4f  %s\n",
               c.name, (unsigned long long)r.outputs, (unsigned long long)r.interpolated, (unsigned long long)r.lateAfterWarmup,
               (unsigned long long)r.backwards, r.delayP50, r.delayMax, c.expectMaxDelay, r.maxSpeedDev, r.maxGridErr,
               ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    // Source stall: a 250 ms USB hiccup must not inflate D (it is not a pacing error).
    for (PaceMode m : {PaceMode::Vrr, PaceMode::Fixed}) {
        Result r = Simulate(m, m == PaceMode::Vrr ? 1000.0 : 164.0, 2, 60.0, 20, 0.5, 30.0, 99, 0.0, 15.0);
        bool ok = r.delayMaxAfterStall - r.delayBeforeStall < 3.0 && r.backwards == 0 && r.stalls > 0;
        printf("%-24s D before %.2f ms, max within 10 s after %.2f ms, stall outputs %llu, late %llu  %s\n",
               m == PaceMode::Vrr ? "stall 250ms (vrr)" : "stall 250ms (fixed 164)", r.delayBeforeStall, r.delayMaxAfterStall,
               (unsigned long long)r.stalls, (unsigned long long)r.lateAfterWarmup, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    fails += CadenceTests();
    fails += PredictionTests();
    fails += ContentTests();
    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
