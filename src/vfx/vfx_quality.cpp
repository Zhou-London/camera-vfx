#include "vfx_quality.h"
#include <algorithm>

namespace cameravfx {

namespace {
constexpr QualityLevel kFgLevels[] = {{"low", 0}, {"medium", 1}, {"high", 2}};

constexpr QualityLevel kSrLevels[] = {
    {"bicubic", 0}, {"low", 1}, {"medium", 2}, {"high", 3}, {"ultra", 4},
    {"denoise-low", 8}, {"denoise-medium", 9}, {"denoise-high", 10}, {"denoise-ultra", 11},
    {"deblur-low", 12}, {"deblur-medium", 13}, {"deblur-high", 14}, {"deblur-ultra", 15},
    {"hb-low", 16}, {"hb-medium", 17}, {"hb-high", 18}, {"hb-ultra", 19},
    {"streaming-medium", 21}, {"streaming-ultra", 23},
};

constexpr int kFirstSameSizeSrLevel = 8;
constexpr int kLastSameSizeSrLevel = 15;

const char* NameOf(std::span<const QualityLevel> levels, int value) {
    const auto it = std::find_if(levels.begin(), levels.end(), [&](const QualityLevel& l) { return l.value == value; });
    return it == levels.end() ? "?" : it->name;
}

std::optional<int> Find(std::span<const QualityLevel> levels, std::string_view name) {
    const auto it = std::find_if(levels.begin(), levels.end(), [&](const QualityLevel& l) { return name == l.name; });
    return it == levels.end() ? std::nullopt : std::optional<int>(it->value);
}
}

std::span<const QualityLevel> FgQualityLevels() { return kFgLevels; }
std::span<const QualityLevel> SrQualityLevels() { return kSrLevels; }
const char* FgQualityName(int fgQuality) { return NameOf(kFgLevels, fgQuality); }
const char* SrQualityName(int srQuality) { return NameOf(kSrLevels, srQuality); }
std::optional<int> FindFgQuality(std::string_view name) { return Find(kFgLevels, name); }
std::optional<int> FindSrQuality(std::string_view name) { return Find(kSrLevels, name); }
bool SrQualityNeedsSameSize(int srQuality) { return srQuality >= kFirstSameSizeSrLevel && srQuality <= kLastSameSizeSrLevel; }

}
