// Repeat-frame detection on the CPU copy of a captured frame (capture thread, before the
// frame is published). A digital HDMI source repeats frames bit-exactly when the content
// rate is below the signal rate (30 fps game in 60 Hz), so a frame is a repeat when (almost)
// no sampled byte changed by more than a small threshold. Keeps a subsampled copy of the
// previous frame; SSE2, tens of microseconds for 1080p.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace camss {

enum class DiffLayout {
    Bytes8,       // every byte is an 8-bit sample (YUY2, NV12 luma, RGB24, RGB32)
    HighBytes16,  // 16-bit little-endian samples, compare the high byte only (P010 luma)
};

struct RepeatConfig {
    int rowStep = 2;               // compare every n-th row
    int levelThreshold = 6;        // |difference| above this (8-bit levels) counts as changed
    double maxChangedFrac = 2e-5;  // repeat if at most this fraction of compared samples changed
};

class RepeatDetector {
public:
    // rowBytes: bytes per row to compare (e.g. width*2 for YUY2, width for NV12 luma, width*2 for P010 luma).
    void Configure(DiffLayout layout, int rowBytes, int rows, const RepeatConfig& cfg = {});
    // top = row 0 of the frame, pitch in bytes (may be negative for bottom-up images).
    // Returns true if the frame repeats the previously checked frame. The first frame is never a repeat.
    bool Check(const uint8_t* top, ptrdiff_t pitch);
    void Reset();                    // forget the previous frame
    double LastChangedFrac() const { return changedFrac_; }
    double LastMeanAbsDiff() const { return meanAbs_; }   // mean |diff| of compared samples (8-bit levels)
    double LastCostMs() const { return costMs_; }
private:
    DiffLayout layout_ = DiffLayout::Bytes8;
    int rowBytes_ = 0, rows_ = 0;
    RepeatConfig cfg_;
    std::vector<uint8_t> prev_;
    bool havePrev_ = false;
    double changedFrac_ = 0, meanAbs_ = 0, costMs_ = 0;
};

} // namespace camss
