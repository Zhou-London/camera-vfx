#include "timecode.h"
#include "../util/common.h"
#include <algorithm>
#include <cstdlib>

namespace cameravfx {

namespace {

constexpr int kMinMarkerContrast = 80;
constexpr int kMinBitMargin = 30;
constexpr float kBlockSampleRadiusFrac = 0.25f;

struct LumaSampling {
    int bytesPerPixel;
    int offset;
};

constexpr LumaSampling kLumaSampling[] = {{2, 0}, {1, 0}, {2, 1}, {3, 1}, {4, 1}};

}

uint32_t TimecodeAt(int64_t qpc) { return (uint32_t)(int64_t)(Qpc::ToMs(qpc) * kTimecodeUnitsPerMs); }

std::optional<uint32_t> DecodeTimecode(const uint8_t* top, ptrdiff_t pitch, PixFmt format, int width, int height) {
    const int bandMiddle = std::max(1, (int)(height * kTimecodeBandFrac * 0.5f));
    const uint8_t* row = top + (ptrdiff_t)bandMiddle * pitch;
    const LumaSampling luma = kLumaSampling[(int)format];
    const float blockWidth = (float)width / kTimecodeBlocks;
    const int radius = std::max(1, (int)(blockWidth * kBlockSampleRadiusFrac));
    int levels[kTimecodeBlocks];
    for (int b = 0; b < kTimecodeBlocks; b++) {
        const int center = (int)((b + 0.5f) * blockWidth);
        const int x0 = std::max(0, center - radius), x1 = std::min(width - 1, center + radius);
        int sum = 0;
        for (int x = x0; x <= x1; x++) sum += row[x * luma.bytesPerPixel + luma.offset];
        levels[b] = sum / (x1 - x0 + 1);
    }
    const int white = levels[0], black = levels[kTimecodeBlocks - 1];
    if (white - black < kMinMarkerContrast) return std::nullopt;
    const int threshold = (white + black) / 2;
    uint32_t value = 0;
    for (int b = 1; b <= kTimecodeBits; b++) {
        if (std::abs(levels[b] - threshold) < kMinBitMargin) return std::nullopt;
        value = (value << 1) | (levels[b] > threshold ? 1u : 0u);
    }
    return value;
}

}
