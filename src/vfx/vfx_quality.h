#pragma once
#include <optional>
#include <span>
#include <string_view>

namespace cameravfx {

struct QualityLevel {
    const char* name;
    int value;
};

std::span<const QualityLevel> FgQualityLevels();
std::span<const QualityLevel> SrQualityLevels();
const char* FgQualityName(int fgQuality);
const char* SrQualityName(int srQuality);
std::optional<int> FindFgQuality(std::string_view name);
std::optional<int> FindSrQuality(std::string_view name);
bool SrQualityNeedsSameSize(int srQuality);

}
