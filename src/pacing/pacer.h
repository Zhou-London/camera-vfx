// Output frame pacing ("出帧节奏") — the core of camss.
//
// Every output frame m has a display time V_m (predicted vblank in fixed-refresh
// mode, or a scheduled present time in VRR mode). The pacer maps it onto the
// source timeline:      x_m = S^-1(V_m - D)
// where S(n) = a + n*P is the fitted source clock (frame n becomes GPU-ready at
// S(n)) and D is the pipeline delay. x_m = i + t selects the frame pair (i, i+1)
// and the interpolation time step t that the frame generator is asked for.
//
// Because S is refitted continuously and V comes from the separately fitted
// display clock, drift between capture and display clocks is absorbed
// automatically: the phase of t simply slides.
//
// D is not a constant: it is servoed by measuring "slack" — how long before the
// render started the newest frame we needed was actually available. A frame
// with t <= eps only needs frame i (snapped to t = 0); otherwise frame i+1 is
// needed, which costs (1 - t) * P of extra delay. The controller keeps the
// minimum slack over a short window near a safety margin: fast attack when a
// frame would be late, slow release (≤ ~0.5 % playback speed change) so the
// adjustment is invisible. Result: D settles at (1 - t_min) * P + jitter +
// render lead, i.e. (k-1)/k * P + inference for k-aligned VRR output.
#pragma once
#include "clocks.h"
#include <deque>
#include <map>

namespace camss {

enum class PaceMode { Passthrough, Fixed, Vrr };
const char* PaceModeName(PaceMode m);

struct PacerConfig {
    PaceMode mode = PaceMode::Fixed;
    int vrrMultiplier = 2;          // k: output frames per source frame in VRR mode
    double snapEps = 0.06;          // t <= eps (or >= 1-eps) is snapped to a real frame
    double marginMs = 1.0;          // slack safety margin
    double releasePerSec = 2.0;     // max ms of delay reduction per second (playback speed change)
    double initialDelayMs = 30.0;
    double minDelayMs = 0.0;
    double maxDelayMs = 80.0;
    double windowSec = 2.0;         // slack min-window (covers phase cycles and rare jitter spikes)
    double holdAfterLateSec = 5.0;  // no delay release for this long after a late frame
};

struct PaceDecision {
    bool valid = false;
    double x = 0;          // fractional source index
    int64_t nA = 0, nB = 0;// source indices of the pair (nB == nA when no interpolation needed)
    float t = 0;           // interpolation step in [0,1)
    bool late = false;     // needed frame was not available; clamped
    double delayMs = 0;    // D used for this decision
    int64_t displayQpc = 0;// V
};

class Pacer {
public:
    void Configure(const PacerConfig& cfg, double sourceFps);
    const PacerConfig& config() const { return cfg_; }

    // Source frame n became usable (uploaded to GPU) at readyQpc.
    void OnSourceFrame(int64_t n, int64_t readyQpc);
    bool SourceValid() const { return src_.Valid(); }
    double SourcePeriodMs() const { return Qpc::ToMs((int64_t)src_.Period()); }
    double SourceHz() const { return src_.Valid() ? (double)Qpc::Freq() / src_.Period() : 0; }
    double SourceJitterMs() const;
    int64_t NewestIndex() const { return newestN_; }

    // Decide what to show at display time V given the render starts at `renderStart`.
    PaceDecision Decide(int64_t renderStart, int64_t V);

    // VRR scheduling: next output display time (QPC) for the k-aligned grid.
    // Returns 0 if the source clock is not yet valid.
    int64_t NextVrrDisplayTime(int64_t now, double renderLeadMs);

    double DelayMs() const { return Qpc::ToMs((int64_t)delay_); }
    uint64_t LateCount() const { return lateCount_; }
    double MinSlackMs() const { return minSlackMs_; }

private:
    bool ReadyTime(int64_t n, int64_t& t) const;
    PacerConfig cfg_;
    LinearClock src_{240};
    std::map<int64_t, int64_t> ready_;   // n -> ready QPC (recent)
    int64_t newestN_ = -1;
    double delay_ = 0;                   // QPC ticks
    std::deque<std::pair<int64_t, double>> slack_; // (render time, slack ticks)
    int64_t lastDecideT_ = 0;
    double lastX_ = -1;
    double vrrX_ = -1;
    uint64_t lateCount_ = 0;
    int64_t lastLateT_ = 0;
    double minSlackMs_ = 0;
};

} // namespace camss
