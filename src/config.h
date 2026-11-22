#pragma once
#include "util/common.h"
#include "capture/video_format.h"
#include "pacing/pace_mode.h"
#include <optional>
#include <string>
#include <vector>

namespace cameravfx {

inline constexpr int kMaxFgMultiplier = 4;

enum class UiMode { Auto, Tui, Plain };

struct KeyScriptStep {
    double atSec = 0;
    int key = 0;
    std::string setting;
    std::string value;
};

struct Options {
    DeviceSelector device;
    CaptureMode capture{PixFmt::YUY2, 1920, 1080, 60.0};
    ColorOptions color;
    bool synthetic = false;
    std::optional<double> syntheticContentFps;

    bool fg = false;
    int fgQuality = 0;
    int fgMultiplier = 2;
    double fgThresholdFps = 60;
    bool detectRepeats = true;
    std::vector<double> contentRates;
    std::optional<int> repeatThreshold;
    double repeatMaxChanged = 2e-5;
    std::optional<uint32_t> repeatMaxTile = 16;

    bool sr = false;
    int srQuality = 21;
    bool hdr = false;
    bool hdrAfterSr = false;
    std::optional<FrameSize> out;
    double sdrWhiteNits = 203;

    bool audio = true;
    std::wstring audioIn;
    std::wstring audioOut;
    double audioVolumePercent = 100;
    double audioBufferMs = 10;
    double audioOffsetMs = 0;
    bool audioSync = true;
    bool audioTone = false;

    std::optional<PaceMode> mode;
    bool fixedLateLatch = true;
    double leadMs = 3;
    double baseDelayMs = 7;
    double marginMs = 1;
    double snap = 0.06;
    int passthroughSync = 1;
    bool windowed = false;
    int monitor = 0;

    UiMode ui = UiMode::Auto;
    LogLevel logLevel = LogLevel::Info;
    std::string log;
    double reportSec = 2;
    bool reportDetail = false;
    double warmupSec = 6;
    double durationSec = 0;
    std::vector<KeyScriptStep> keyScript;
    std::string csv;
    bool timecode = false;
    bool decodeTimecode = false;
    bool marker = false;
    bool d3dDebug = false;

    bool list = false;
    bool selftest = false;
    std::string config;
};

enum class ParseResult { Run, Exit, Error };

ParseResult LoadOptions(int argc, wchar_t** argv, Options& o);
bool SaveOptions(const Options& o, const std::vector<std::string>& keys);

}
