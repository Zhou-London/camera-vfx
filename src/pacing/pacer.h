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
//
// Content cadence. The signal rate (e.g. 60 Hz HDMI) and the content rate can
// differ: a 30 fps game shows every frame twice, 24p film uses 3:2 pulldown.
// S stays the SIGNAL clock (every received frame feeds it, unique or repeat),
// so it never needs a reset when the content rate changes; only frames carrying
// new content ("unique") can be interpolated from. With content period Pc (signal
// frames per content frame, SetContentPeriod) the pair is resolved among unique
// frames: nA = newest unique <= x, nB = the next unique; t = (x - nA)/(nB - nA).
// When nB has not arrived it is expected at nA + Pc. Everything else (slack,
// late penalty, VRR grid with step Pc/k, steady state D = (k-1)/k * Pc * P +
// inference) generalises with "the next content frame" in place of "frame i+1".
// A static scene (repeats keep arriving, no new content) is a stall: the newest
// unique frame is held and D is not raised. Pc = 1 is exactly the plain behaviour.
#pragma once
#include "clocks.h"
#include <deque>
#include <map>

namespace camss {

enum class PaceMode { Passthrough, Fixed, Vrr };
const char* PaceModeName(PaceMode m);

struct PacerConfig {
    PaceMode mode = PaceMode::Fixed;
    int vrrMultiplier = 2;          // k: output frames per content frame in VRR mode
    double snapEps = 0.06;          // t <= eps (or >= 1-eps) is snapped to a real frame
    double marginMs = 1.0;          // slack safety margin
    double releasePerSec = 2.0;     // max ms of delay reduction per second (playback speed change)
    double initialDelayMs = 30.0;
    double minDelayMs = 0.0;
    double maxDelayMs = 80.0;
    double windowSec = 2.0;         // slack min-window (covers phase cycles and rare jitter spikes)
    double holdAfterLateSec = 5.0;  // no delay release for this long after a late frame
    // Fixed refresh with content period > 1: the output phases per content frame are sparse, and a
    // release down to the exact edge lets clock-fit noise / drift push a snapped phase past eps
    // (needing a frame a whole phase step later). Keep D such that it would still be safe this much
    // lower. (Not applied at Pc = 1, which keeps the original behaviour.)
    double phaseGuardMs = 1.0;
};

struct PaceDecision {
    bool valid = false;
    double x = 0;          // fractional source (signal) index
    int64_t nA = 0, nB = 0;// source indices of the pair (nB == nA when no interpolation needed); always unique frames
    float t = 0;           // interpolation step in [0,1) between nA and nB
    bool late = false;     // needed frame was not available (genuinely late, or the source itself stalled); clamped
    bool stall = false;    // no new frame was due to arrive in time: source stall or static content (D not raised)
    double cx = 0;         // content position of the shown image in source frames (monotonic; == x when interpolating
                           // across a regular cadence; after a static scene the held image counts as content at nB - Pc)
    double delayMs = 0;    // D used for this decision
    int64_t displayQpc = 0;// V
};

class Pacer {
public:
    void Configure(const PacerConfig& cfg, double sourceFps);
    const PacerConfig& config() const { return cfg_; }

    // Source (signal) frame n became usable (uploaded to GPU) at readyQpc. Feed EVERY received
    // signal frame in order; unique = false for a repeat of the previous content frame (it only
    // advances the signal clock and is never shown or interpolated from).
    void OnSourceFrame(int64_t n, int64_t readyQpc, bool unique = true);
    bool SourceValid() const { return src_.Valid(); }
    double SourcePeriodMs() const { return Qpc::ToMs((int64_t)src_.Period()); }
    double SourceHz() const { return src_.Valid() ? (double)Qpc::Freq() / src_.Period() : 0; }
    double SourceJitterMs() const;
    int64_t NewestIndex() const { return newestN_; }            // newest UNIQUE frame
    int64_t NewestSignalIndex() const { return newestSignal_; } // newest received signal frame (unique or repeat)

    // Content period Pc in signal frames (>= 1; 1 = every frame new, 2 = 30 fps in 60 Hz,
    // 2.5 = 24p 3:2 pulldown, 1.5 = 40 fps). Changing it retimes D by the theoretical change
    // of the steady-state delay and re-anchors the VRR grid.
    void SetContentPeriod(double pc);
    double ContentPeriod() const { return pc_; }
    // VRR output frames per content frame (k). Output rate = k * content rate. Retimes like above.
    void SetMultiplier(int k);
    int Multiplier() const { return cfg_.vrrMultiplier; }

    // Decide what to show at display time V given the render starts at `renderStart`.
    PaceDecision Decide(int64_t renderStart, int64_t V);

    // VRR scheduling: next output display time (QPC) on the k-per-content-frame grid
    // (step Pc/k in x, phase anchored to the unique frames). Returns 0 if the source clock is not yet valid.
    int64_t NextVrrDisplayTime(int64_t now, double renderLeadMs);

    double DelayMs() const { return Qpc::ToMs((int64_t)delay_); }
    uint64_t LateCount() const { return lateCount_; }
    uint64_t StallCount() const { return stallCount_; }
    void SetMode(PaceMode m) { cfg_.mode = m; }
    double MinSlackMs() const { return minSlackMs_; }

private:
    struct Entry { int64_t ready; int64_t lastRepeat; };   // unique frame: ready QPC, last repeat of it received (-1)
    // Pair for content position x among the unique frames.
    struct Pick {
        int64_t nA = 0, nB = 0;  // shown frame(s): nB == nA when not interpolating
        double tt = 0;           // snapped step
        double start = 0;        // interpolation start in x (nA, or later after a static scene)
        int64_t need = 0;        // frame that must be ready (when !missing)
        int64_t needReady = 0;   // its ready QPC
        bool missing = false;    // the next content frame is needed but has not arrived
        double pred = 0;         // predicted index of the next content frame (when nB has not arrived)
        bool overdueContent = false; // nB has not arrived although the signal frame where it was due has (as a repeat)
    };
    bool Resolve(double x, Pick& p) const;
    double PairStart(int64_t nA, double nB, int64_t rep) const;
    double SteadyExtraTicks(double pc, int k) const;
    void Retime(double pc, int k);
    bool ReadyTime(int64_t n, int64_t& t) const;
    PacerConfig cfg_;
    LinearClock src_{240};
    std::map<int64_t, Entry> ready_;     // unique n -> ready QPC (recent)
    int64_t newestN_ = -1;               // newest unique
    int64_t newestSignal_ = -1;          // newest received signal frame
    double pc_ = 1.0;                    // content period (signal frames)
    double delay_ = 0;                   // QPC ticks
    struct SlackSample { int64_t s; int64_t V; double slackMinusD; double raw; };
    std::deque<SlackSample> slack_;   // window of measured slack (ticks)
    double TrialMinSlack(double trialDelay) const;
    int64_t lastDecideT_ = 0;
    double lastX_ = -1;
    double vrrX_ = -1;
    bool regrid_ = false;                // re-anchor the VRR grid at the next schedule
    int64_t lastVrrV_ = 0;               // last scheduled VRR display time
    uint64_t lateCount_ = 0;
    int64_t lastLateT_ = 0;
    double lastLateNeed_ = -1;
    uint64_t stallCount_ = 0;
    double minSlackMs_ = 0;
};

} // namespace camss
