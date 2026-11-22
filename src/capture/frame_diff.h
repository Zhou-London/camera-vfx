#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cameravfx {

enum class DiffLayout { Bytes8, HighBytes16 };

struct RepeatConfig {
    int rowStep = 2;
    int levelThreshold = 0;
    double maxChangedFrac = 0;
    std::optional<uint32_t> maxTileChanged;
};

struct RepeatCheck {
    bool repeat = false;
    uint64_t changed = 0;
    uint64_t compared = 0;
    uint32_t maxTileChanged = 0;
    double costMs = 0;
};

class RepeatDetector {
public:
    void Configure(DiffLayout layout, int rowBytes, int rows, const RepeatConfig& cfg);
    RepeatCheck Check(const uint8_t* top, ptrdiff_t pitch);
    void Reset() { haveReference_ = false; }

private:
    DiffLayout layout_ = DiffLayout::Bytes8;
    RepeatConfig cfg_;
    std::vector<uint8_t> buffer_;
    size_t alignOffset_ = 0;
    size_t laneBytes_ = 0;
    size_t copyStride_ = 0;
    int samplesPerRow_ = 0;
    int comparedRows_ = 0;
    int bandRows_ = 1;
    int tileCols_ = 0;
    int referenceCopy_ = 0;
    bool haveReference_ = false;
};

}
