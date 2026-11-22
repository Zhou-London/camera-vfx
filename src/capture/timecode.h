#pragma once
#include "video_format.h"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace cameravfx {

constexpr int kTimecodeBits = 32;
constexpr int kTimecodeBlocks = kTimecodeBits + 2;
constexpr float kTimecodeBandFrac = 1.0f / 24.0f;
constexpr double kTimecodeUnitsPerMs = 100.0;

uint32_t TimecodeAt(int64_t qpc);
std::optional<uint32_t> DecodeTimecode(const uint8_t* top, ptrdiff_t pitch, PixFmt format, int width, int height);

}
