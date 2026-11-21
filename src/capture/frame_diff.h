// Repeat-frame detection on the CPU copy of a captured frame (capture thread, before the
// frame is published). A digital HDMI source repeats frames bit-exactly when the content
// rate is below the signal rate (30 fps game in 60 Hz), so a frame is a repeat when (almost)
// no sampled byte changed by more than a small threshold. Keeps a subsampled copy of the
// previous content frame; AVX2 (SSE2 fallback), tens of microseconds for 1080p.
//
// Decision (both must hold for a repeat):
//  * global: changed samples <= maxChangedFrac * compared samples. A sample is "changed" when
//    |cur - ref| > levelThreshold (8-bit levels). This tolerates capture noise / dithering of a
//    few levels anywhere and a few isolated outliers.
//  * local: no tile (64 compared samples wide x ~16 image rows, i.e. 32 px for YUY2, 64 px for
//    NV12 / P010 luma, ~21 px for RGB24, 16 px for RGB32) has more than maxTileChanged changed samples.
//    A global fraction alone could hide a small moving object if maxChangedFrac is raised for
//    a noisy source: a 30x30 px cursor in 1080p YUY2 changes ~1800 bytes per frame; with every
//    2nd row compared that is ~900 changed samples of 2.07M (4.3e-4). With the default 2e-5
//    (41 samples) the global test already catches it, but at e.g. 1e-3 it would not. Noise is
//    spread over the frame while a moving object concentrates its changes, so a per-tile
//    count catches any strongly changed ~16x16 region independent of maxChangedFrac.
//
// Reference frame: the detector compares against the last frame that was NOT a repeat (the
// last content frame), not simply the previous frame. The compact copy is written in the same
// pass into a second buffer, which becomes the reference only if the frame is new content. A
// slow sub-threshold change (fade) therefore accumulates until it crosses the threshold instead
// of being called a repeat forever.
//
// Deliberate blind spot: a uniform change of <= levelThreshold levels everywhere (e.g. a fade
// step of 1-2 levels in a dark scene) is a repeat until it accumulates past the threshold.
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
    int maxTileChanged = 16;       // ... and no tile (64 samples x ~16 image rows) has more changed samples (< 0 = off)
};

class RepeatDetector {
public:
    // rowBytes: bytes per row to compare (e.g. width*2 for YUY2, width for NV12 luma, width*2 for P010 luma).
    // Allocates the reference copy (2 x compared bytes); call outside the hot path.
    void Configure(DiffLayout layout, int rowBytes, int rows, const RepeatConfig& cfg = {});
    // top = row 0 of the frame, pitch in bytes (may be negative for bottom-up images).
    // Returns true if the frame repeats the last content frame. The first frame after
    // Configure()/Reset() is never a repeat. No allocation.
    bool Check(const uint8_t* top, ptrdiff_t pitch);
    void Reset();                    // forget the previous frame
    bool Configured() const { return samplesPerRow_ > 0 && cmpRows_ > 0; }
    double LastChangedFrac() const { return changedFrac_; }
    double LastMeanAbsDiff() const { return meanAbs_; }   // mean |diff| of compared samples (8-bit levels)
    double LastCostMs() const { return costMs_; }
    uint64_t LastChangedCount() const { return changed_; }
    int LastMaxTileChanged() const { return maxTile_; }  // largest per-tile changed count of the last check
    uint64_t ComparedSamples() const { return (uint64_t)samplesPerRow_ * (uint64_t)cmpRows_; }
    const RepeatConfig& Config() const { return cfg_; }
    // The kernel uses AVX2 when the CPU supports it (runtime check), SSE2 otherwise.
    // Results are bit-identical; this switch exists for tests/benchmarks.
    static bool Avx2Supported();
    void SetAllowAvx2(bool allow) { allowAvx2_ = allow; }
    bool UsingAvx2() const { return allowAvx2_ && Avx2Supported(); }
private:
    DiffLayout layout_ = DiffLayout::Bytes8;
    int rowBytes_ = 0, rows_ = 0;
    RepeatConfig cfg_;
    // One allocation, 64-byte aligned at prevOff_: [tile lane counters][copy 0][copy 1].
    // Tile lane counters: 32 byte-wide counters per 64-sample tile column (current tile band).
    // Copies: compact compared rows (reference + scratch), stride_ bytes per compared row.
    std::vector<uint8_t> prev_;
    int tileCols_ = 0;              // full 64-sample tile columns per row (a shorter tail is counted separately)
    size_t lanesBytes_ = 0;         // bytes reserved for the tile lane counters
    int samplesPerRow_ = 0;         // compared samples per row (rowBytes, or rowBytes/2 for HighBytes16)
    int cmpRows_ = 0;               // compared rows
    int bandRows_ = 1;              // compared rows per tile band
    size_t stride_ = 0;             // compact row stride (bytes, multiple of 64)
    size_t prevOff_ = 0;            // offset of the 64-byte aligned start in prev_
    int refIdx_ = 0;                // which of the two copies is the reference
    bool havePrev_ = false;
    bool allowAvx2_ = true;
    uint64_t changed_ = 0;
    int maxTile_ = 0;
    double changedFrac_ = 0, meanAbs_ = 0, costMs_ = 0;
};

} // namespace camss
