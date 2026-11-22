#pragma once
#include <optional>
#include <string>
#include <variant>

namespace cameravfx {

enum class PixFmt { YUY2, NV12, P010, RGB24, RGB32 };

enum class ColorMatrix { Bt709, Bt601, Bt2020 };

struct CaptureMode {
    PixFmt format = PixFmt::YUY2;
    unsigned width = 0, height = 0;
    double rateHz = 0;
};

inline constexpr double kSameRateToleranceHz = 0.01;

inline bool SameSize(const CaptureMode& a, const CaptureMode& b) { return a.width == b.width && a.height == b.height; }
inline bool SameRate(double a, double b) { return a - b < kSameRateToleranceHz && b - a < kSameRateToleranceHz; }
inline bool SameRate(const CaptureMode& a, const CaptureMode& b) { return SameRate(a.rateHz, b.rateHz); }
inline bool SameMode(const CaptureMode& a, const CaptureMode& b) { return a.format == b.format && SameSize(a, b) && SameRate(a, b); }

using DeviceSelector = std::variant<std::monostate, int, std::wstring>;

struct ColorOptions {
    std::optional<ColorMatrix> matrix;
    std::optional<bool> fullRange;
};

inline bool IsRgb(PixFmt f) { return f == PixFmt::RGB24 || f == PixFmt::RGB32; }

inline const char* PixFmtName(PixFmt f) {
    constexpr const char* kNames[] = {"YUY2", "NV12", "P010", "RGB24", "RGB32"};
    return kNames[(int)f];
}

inline const char* PixFmtLabel(PixFmt f) {
    constexpr const char* kLabels[] = {"YUY2 4:2:2", "NV12 4:2:0", "P010 10-bit", "RGB24 4:4:4", "RGB32 4:4:4"};
    return kLabels[(int)f];
}

inline double PixFmtBytesPerPixel(PixFmt f) {
    constexpr double kBytes[] = {2, 1.5, 3, 3, 4};
    return kBytes[(int)f];
}

inline const char* MatrixName(ColorMatrix m) {
    constexpr const char* kNames[] = {"BT.709", "BT.601", "BT.2020"};
    return kNames[(int)m];
}

}
