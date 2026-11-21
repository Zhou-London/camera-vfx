// Content cadence detection: a 60 Hz signal often carries content rendered at a lower rate
// (a 30 fps game shows every frame twice, 24p film uses 3:2 pulldown). From the per-frame
// "new content / repeat" flags this estimates the content period Pc in source frames
// (1 = every frame new, 2 = 30 fps in 60 Hz, 2.5 = 24p 3:2, 1.5 = 40 fps ...), with
// hysteresis so the pacer is only retimed on a sustained change.
#pragma once
#include <cstdint>
#include <deque>

namespace camss {

struct CadenceConfig {
    int windowFrames = 120;        // source frames considered (2 s at 60 Hz)
    int maxInterval = 6;           // unique-frame intervals longer than this are discontinuities (static scene, hitch)
    int minIntervals = 6;          // intervals needed in the window before estimating
    double switchFrac = 0.15;      // relative difference to the active period that starts a switch
    int holdFrames = 45;           // ... sustained for this many source frames
    double snapTol = 0.04;         // snap to a common ratio (1, 1.25, 1.5, 2, 2.5, 3, 4) within this relative tolerance
};

class CadenceDetector {
public:
    explicit CadenceDetector(const CadenceConfig& c = {}) : cfg_(c) {}
    void Reset();
    // Feed every received source frame in order: n = source index (gaps = dropped frames),
    // unique = carries new content (false = repeat of the previous frame).
    // Returns true when the active content period changed.
    bool OnFrame(int64_t n, bool unique);
    double ContentPeriod() const { return active_; }        // active Pc (>= 1)
    double EstimatedPeriod() const { return estimate_; }    // current raw estimate (0 = none)
    double UniqueFraction() const;                          // fraction of unique frames in the window
private:
    CadenceConfig cfg_;
    double active_ = 1.0;
    double estimate_ = 0.0;
    int pendingFrames_ = 0;
    // implementation state (owned by the cadence work item; may be changed freely)
    std::deque<int64_t> unique_;   // source indices of unique frames in the window
    std::deque<int64_t> seen_;     // source indices of all frames in the window
    int64_t lastN_ = -1;
};

} // namespace camss
