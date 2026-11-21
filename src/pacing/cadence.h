// Content cadence detection: a 60 Hz signal often carries content rendered at a lower rate
// (a 30 fps game shows every frame twice, 24p film uses 3:2 pulldown). From the per-frame
// "new content / repeat" flags this estimates the content period Pc in source frames
// (1 = every frame new, 2 = 30 fps in 60 Hz, 2.5 = 24p 3:2, 1.5 = 40 fps ...), with
// hysteresis so the pacer is only retimed on a sustained change.
//
// Estimate: mean of the intervals between consecutive unique frames inside the last
// windowFrames signal frames. Intervals longer than maxInterval (static scene, hitch) and
// intervals spanning a signal gap (dropped frames: unknown whether they were new) are ignored.
// The switch latency is about windowFrames/4 + holdFrames for a 60 <-> 30 change.
#pragma once
#include <cstdint>
#include <vector>

namespace camss {

struct CadenceConfig {
    int windowFrames = 40;         // source frames considered (2/3 s at 60 Hz; >= 16 intervals at 24p)
    int maxInterval = 6;           // unique-frame intervals longer than this are discontinuities (static scene, hitch)
    int minIntervals = 6;          // intervals needed in the window before estimating
    double switchFrac = 0.15;      // relative difference to the active period that starts a switch
    int holdFrames = 45;           // ... sustained for this many source frames
    double snapTol = 0.04;         // snap to a common ratio (1, 1.25, 1.5, 2, 2.5, 3, 4) within this relative tolerance
};

class CadenceDetector {
public:
    explicit CadenceDetector(const CadenceConfig& c = {});
    void Reset();
    // Feed every received source frame in order: n = source index (gaps = dropped frames),
    // unique = carries new content (false = repeat of the previous frame).
    // Returns true when the active content period changed. O(windowFrames), no allocation.
    bool OnFrame(int64_t n, bool unique);
    double ContentPeriod() const { return active_; }        // active Pc (>= 1)
    double EstimatedPeriod() const { return estimate_; }    // current raw estimate (0 = none)
    double UniqueFraction() const;                          // fraction of unique frames in the window
    const CadenceConfig& config() const { return cfg_; }
private:
    void Estimate();
    CadenceConfig cfg_;
    double active_ = 1.0;
    double estimate_ = 0.0;
    int pendingFrames_ = 0;
    // Ring over the last windowFrames signal indices: 0 = not received, 1 = repeat, 2 = unique.
    std::vector<uint8_t> slot_;
    int64_t lastN_ = -1;
    int received_ = 0, uniques_ = 0;   // counts of received / unique frames in the window
};

} // namespace camss
