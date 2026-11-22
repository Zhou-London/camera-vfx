#include "config.h"
#include "ui/telemetry.h"
#include "vfx/vfx_quality.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

namespace cameravfx {

namespace {

constexpr char kConfigFileName[] = "CameraVFX.ini";
constexpr unsigned kMinFrameSide = 16;
constexpr unsigned kMaxFrameSide = 16384;
constexpr double kMaxRateHz = 1000;
constexpr int kMaxDeviceIndex = 99;
constexpr double kMaxSeconds = 86400;
constexpr size_t kHelpColumn = 35;
constexpr const char* kNoneArg = nullptr;

std::string_view Trim(std::string_view s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<std::string_view> Split(std::string_view s, char separator) {
    std::vector<std::string_view> parts;
    for (size_t start = 0;;) {
        const size_t end = s.find(separator, start);
        parts.push_back(Trim(s.substr(start, end - start)));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

std::optional<double> ParseNumber(std::string_view v, double lo, double hi) {
    const std::string s(Trim(v));
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const double d = strtod(s.c_str(), &end);
    if (errno || *end || !std::isfinite(d) || d < lo || d > hi) return std::nullopt;
    return d;
}

std::optional<int> ParseInteger(std::string_view v, int lo, int hi) {
    const std::optional<double> d = ParseNumber(v, lo, hi);
    if (!d || *d != std::floor(*d)) return std::nullopt;
    return (int)*d;
}

std::optional<unsigned> ParseSide(std::string_view v) {
    unsigned side = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), side);
    if (ec != std::errc() || end != v.data() + v.size() || side < kMinFrameSide || side > kMaxFrameSide) return std::nullopt;
    return side;
}

std::optional<FrameSize> ParseFrameSize(std::string_view v) {
    const std::string s = Lower(Trim(v));
    const size_t x = s.find('x');
    if (x == std::string::npos) return std::nullopt;
    const std::optional<unsigned> w = ParseSide(Trim(std::string_view(s).substr(0, x)));
    const std::optional<unsigned> h = ParseSide(Trim(std::string_view(s).substr(x + 1)));
    if (!w || !h) return std::nullopt;
    return FrameSize{*w, *h};
}

std::string Fmt(double v) {
    char b[64];
    const auto r = std::to_chars(b, b + sizeof(b), v);
    return std::string(b, r.ptr);
}
std::string Fmt(int v) { return std::to_string(v); }
std::string Fmt(uint32_t v) { return std::to_string(v); }
std::string Fmt(bool v) { return v ? "true" : "false"; }
std::string Fmt(const FrameSize& s) { return std::to_string(s.width) + "x" + std::to_string(s.height); }

std::string NormKey(std::string_view k) {
    std::string s = Lower(Trim(k));
    std::replace(s.begin(), s.end(), '_', '-');
    return s;
}

struct ParseActions {
    bool help = false;
    bool printConfig = false;
    bool noConfig = false;
};

struct Target {
    Options& options;
    ParseActions& actions;
};

struct Range {
    double lo, hi;
};

struct Codec {
    std::function<bool(Target, std::string_view)> set;
    std::function<std::string(const Options&)> get;
    std::optional<Range> range;
};

Codec Action(bool ParseActions::*flag) {
    return {[flag](Target t, std::string_view v) {
                const std::optional<bool> b = ParseBool(Trim(v));
                if (b) t.actions.*flag = *b;
                return b.has_value();
            },
            [](const Options&) { return std::string(); }};
}

template <class Field> Codec Switch(Field field) {
    return {[field](Target t, std::string_view v) {
                const std::optional<bool> b = ParseBool(Trim(v));
                if (b) std::invoke(field, t.options) = *b;
                return b.has_value();
            },
            [field](const Options& o) { return Fmt(std::invoke(field, o)); }};
}

template <class Field> Codec Number(Field field, double lo, double hi) {
    return {[=](Target t, std::string_view v) {
                const std::optional<double> d = ParseNumber(v, lo, hi);
                if (d) std::invoke(field, t.options) = *d;
                return d.has_value();
            },
            [field](const Options& o) { return Fmt(std::invoke(field, o)); }, Range{lo, hi}};
}

template <class Field> Codec Integer(Field field, int lo, int hi) {
    return {[=](Target t, std::string_view v) {
                const std::optional<int> i = ParseInteger(v, lo, hi);
                if (i) std::invoke(field, t.options) = *i;
                return i.has_value();
            },
            [field](const Options& o) { return Fmt(std::invoke(field, o)); }, Range{(double)lo, (double)hi}};
}

template <class Field> Codec Text(Field field) {
    return {[field](Target t, std::string_view v) {
                std::invoke(field, t.options) = std::string(Trim(v));
                return true;
            },
            [field](const Options& o) { return std::invoke(field, o); }};
}

Codec ConfigFileCodec() {
    return {[](Target t, std::string_view v) {
                t.options.config = std::string(Trim(v));
                return !t.options.config.empty();
            },
            [](const Options& o) { return o.config; }};
}

template <class Field> Codec WideText(Field field) {
    return {[field](Target t, std::string_view v) {
                std::invoke(field, t.options) = Utf8ToWide(std::string(Trim(v)).c_str());
                return true;
            },
            [field](const Options& o) { return WideToUtf8(std::invoke(field, o).c_str()); }};
}

template <class Field, class Value> Codec Choice(Field field, std::vector<std::pair<const char*, Value>> choices) {
    return {[=](Target t, std::string_view v) {
                const std::string s = Lower(Trim(v));
                const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& c) { return s == c.first; });
                if (it != choices.end()) std::invoke(field, t.options) = it->second;
                return it != choices.end();
            },
            [=](const Options& o) {
                const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& c) { return std::invoke(field, o) == c.second; });
                return std::string(it->first);
            }};
}

template <class Field> Codec Quality(Field field, std::optional<int> (*find)(std::string_view), const char* (*name)(int)) {
    return {[=](Target t, std::string_view v) {
                const std::optional<int> q = find(Lower(Trim(v)));
                if (q) std::invoke(field, t.options) = *q;
                return q.has_value();
            },
            [=](const Options& o) { return std::string(name(std::invoke(field, o))); }};
}

template <class Field> Codec OptionalNumber(Field field, double lo, double hi, const char* noneName) {
    return {[=](Target t, std::string_view v) {
                if (Lower(Trim(v)) == noneName) {
                    std::invoke(field, t.options).reset();
                    return true;
                }
                const std::optional<double> d = ParseNumber(v, lo, hi);
                if (d) std::invoke(field, t.options) = *d;
                return d.has_value();
            },
            [=](const Options& o) {
                const auto& f = std::invoke(field, o);
                return f ? Fmt(*f) : std::string(noneName);
            },
            Range{lo, hi}};
}

template <class Field> Codec OptionalInteger(Field field, int lo, int hi, const char* noneName) {
    return {[=](Target t, std::string_view v) {
                if (Lower(Trim(v)) == noneName) {
                    std::invoke(field, t.options).reset();
                    return true;
                }
                const std::optional<int> i = ParseInteger(v, lo, hi);
                if (i) std::invoke(field, t.options) = *i;
                return i.has_value();
            },
            [=](const Options& o) {
                const auto& f = std::invoke(field, o);
                return f ? Fmt(*f) : std::string(noneName);
            },
            Range{(double)lo, (double)hi}};
}

Codec DeviceCodec() {
    return {[](Target t, std::string_view v) {
                const std::string_view s = Trim(v);
                const bool digits = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
                const std::optional<int> index = digits ? ParseInteger(s, 0, kMaxDeviceIndex) : std::nullopt;
                if (s.empty()) t.options.device = std::monostate{};
                else if (index) t.options.device = *index;
                else t.options.device = Utf8ToWide(std::string(s).c_str());
                return true;
            },
            [](const Options& o) {
                if (const int* index = std::get_if<int>(&o.device)) return Fmt(*index);
                if (const std::wstring* name = std::get_if<std::wstring>(&o.device)) return WideToUtf8(name->c_str());
                return std::string();
            }};
}

Codec CaptureCodec() {
    return {[](Target t, std::string_view v) {
                const std::string s = Lower(Trim(v));
                const size_t at = s.find('@');
                if (at == std::string::npos) return false;
                const std::optional<FrameSize> size = ParseFrameSize(std::string_view(s).substr(0, at));
                const std::optional<double> rateHz = ParseNumber(std::string_view(s).substr(at + 1), 1, kMaxRateHz);
                if (!size || !rateHz) return false;
                t.options.capture.width = size->width;
                t.options.capture.height = size->height;
                t.options.capture.rateHz = *rateHz;
                return true;
            },
            [](const Options& o) { return Fmt(FrameSize{o.capture.width, o.capture.height}) + "@" + Fmt(o.capture.rateHz); }};
}

Codec OutCodec() {
    return {[](Target t, std::string_view v) {
                if (Lower(Trim(v)) == "auto") {
                    t.options.out.reset();
                    return true;
                }
                const std::optional<FrameSize> size = ParseFrameSize(v);
                if (size) t.options.out = size;
                return size.has_value();
            },
            [](const Options& o) { return o.out ? Fmt(*o.out) : std::string("auto"); }};
}

Codec ContentRatesCodec() {
    return {[](Target t, std::string_view v) {
                if (Lower(Trim(v)) == "auto") {
                    t.options.contentRates.clear();
                    return true;
                }
                std::vector<double> rates;
                for (std::string_view item : Split(v, ',')) {
                    const std::optional<double> fps = ParseNumber(item, 1, kMaxRateHz);
                    if (!fps) return false;
                    rates.push_back(*fps);
                }
                t.options.contentRates = rates;
                return true;
            },
            [](const Options& o) {
                if (o.contentRates.empty()) return std::string("auto");
                std::string list;
                for (double fps : o.contentRates) list += (list.empty() ? "" : ",") + Fmt(fps);
                return list;
            }};
}

bool IsSettingName(std::string_view name) {
    for (int id = 0; id < (int)SettingId::Count; id++)
        if (name == SettingName((SettingId)id)) return true;
    return false;
}

std::optional<KeyScriptStep> ParseKeyScriptStep(std::string_view item) {
    const size_t colon = item.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::optional<double> atSec = ParseNumber(item.substr(0, colon), 0, kMaxSeconds);
    const std::string action = Lower(Trim(item.substr(colon + 1)));
    if (!atSec) return std::nullopt;
    KeyScriptStep step;
    step.atSec = *atSec;
    if (action == "esc") {
        step.key = VK_ESCAPE;
        return step;
    }
    if (action == "m") {
        step.key = 'M';
        return step;
    }
    const size_t eq = action.find('=');
    step.setting = std::string(Trim(std::string_view(action).substr(0, eq)));
    if (eq != std::string::npos) step.value = std::string(Trim(std::string_view(action).substr(eq + 1)));
    if (!IsSettingName(step.setting) || (eq != std::string::npos && step.value.empty())) return std::nullopt;
    return step;
}

std::string FormatKeyScriptStep(const KeyScriptStep& step) {
    std::string action = step.key == VK_ESCAPE ? "esc" : step.key == 'M' ? "m" : step.setting;
    if (!step.value.empty()) action += "=" + step.value;
    return Fmt(step.atSec) + ":" + action;
}

Codec KeyScriptCodec() {
    return {[](Target t, std::string_view v) {
                std::vector<KeyScriptStep> steps;
                if (!Trim(v).empty()) {
                    for (std::string_view item : Split(v, ',')) {
                        const std::optional<KeyScriptStep> step = ParseKeyScriptStep(item);
                        if (!step) return false;
                        steps.push_back(*step);
                    }
                }
                std::stable_sort(steps.begin(), steps.end(), [](const KeyScriptStep& a, const KeyScriptStep& b) { return a.atSec < b.atSec; });
                t.options.keyScript = steps;
                return true;
            },
            [](const Options& o) {
                std::string text;
                for (const KeyScriptStep& step : o.keyScript) text += (text.empty() ? "" : ",") + FormatKeyScriptStep(step);
                return text;
            }};
}

auto CaptureFormat = [](auto& o) -> auto& { return o.capture.format; };
auto ColorMatrixField = [](auto& o) -> auto& { return o.color.matrix; };
auto ColorRangeField = [](auto& o) -> auto& { return o.color.fullRange; };

struct OptionDef {
    const char* name;
    const char* arg;
    const char* group;
    const char* help;
    Codec codec;
    bool commandLineOnly = false;
};

constexpr const char* kActions = "actions";
constexpr const char* kCapture = "capture";
constexpr const char* kFrameGeneration = "frame generation";
constexpr const char* kSuperResolution = "super resolution / HDR";
constexpr const char* kAudio = "audio";
constexpr const char* kPacing = "pacing / display";
constexpr const char* kLogging = "logging / measurement";

const std::vector<OptionDef>& Table() {
    static const std::vector<OptionDef> table = {
        {"help", kNoneArg, kActions, "show this help", Action(&ParseActions::help), true},
        {"list", kNoneArg, kActions, "list video capture devices, their native formats and the audio endpoints", Switch(&Options::list), true},
        {"selftest", kNoneArg, kActions, "offline quality and timing self test of the processing stages", Switch(&Options::selftest), true},
        {"print-config", kNoneArg, kActions, "print the effective options in configuration-file syntax and exit", Action(&ParseActions::printConfig), true},
        {"config", "FILE", kActions, "configuration file to read and to save to (default: CameraVFX.ini next to CameraVFX.exe)", ConfigFileCodec(), true},
        {"no-config", kNoneArg, kActions, "do not read the configuration file (saving still writes it)", Action(&ParseActions::noConfig), true},

        {"device", "NAME|INDEX", kCapture, "video capture device: substring of its name or index from --list (empty: the first device)", DeviceCodec()},
        {"format", "yuy2|nv12|p010|rgb24|rgb32", kCapture, "preferred native capture format",
         Choice(CaptureFormat, std::vector<std::pair<const char*, PixFmt>>{
                                   {"yuy2", PixFmt::YUY2}, {"nv12", PixFmt::NV12}, {"p010", PixFmt::P010}, {"rgb24", PixFmt::RGB24}, {"rgb32", PixFmt::RGB32}})},
        {"capture", "WxH@HZ", kCapture, "capture mode: frame size and signal rate", CaptureCodec()},
        {"matrix", "auto|bt709|bt601|bt2020", kCapture, "YCbCr matrix (auto: what the card reports, else BT.709; BT.2020 for P010)",
         Choice(ColorMatrixField, std::vector<std::pair<const char*, std::optional<ColorMatrix>>>{
                                      {"auto", std::nullopt}, {"bt709", ColorMatrix::Bt709}, {"bt601", ColorMatrix::Bt601}, {"bt2020", ColorMatrix::Bt2020}})},
        {"range", "auto|limited|full", kCapture, "signal range (auto: what the card reports, else limited for YUV and full for RGB)",
         Choice(ColorRangeField, std::vector<std::pair<const char*, std::optional<bool>>>{{"auto", std::nullopt}, {"limited", false}, {"full", true}})},
        {"synthetic", kNoneArg, kCapture, "built-in moving test pattern instead of a capture device", Switch(&Options::synthetic)},
        {"synthetic-content-rate", "auto|FPS", kCapture, "synthetic source: content frame rate inside the signal (30: every frame twice; auto: the signal rate)",
         OptionalNumber(&Options::syntheticContentFps, 1, kMaxRateHz, "auto")},

        {"fg", kNoneArg, kFrameGeneration, "Maxine Video Frame Generation", Switch(&Options::fg)},
        {"fg-quality", "low|medium|high", kFrameGeneration, "frame generation quality", Quality(&Options::fgQuality, FindFgQuality, FgQualityName)},
        {"fg-multiplier", "N", kFrameGeneration, "VRR output frames per content frame below fg-threshold: 2 = 30 -> 60 fps",
         Integer(&Options::fgMultiplier, 1, kMaxFgMultiplier)},
        {"fg-threshold", "FPS", kFrameGeneration,
         "content at or above FPS is shown as it is, slower content gets x fg-multiplier (0: always; the refresh rate and the GPU time still cap it)",
         Number(&Options::fgThresholdFps, 0, kMaxRateHz)},
        {"detect-repeats", kNoneArg, kFrameGeneration,
         "detect repeated frames (a 30 fps game in a 60 Hz signal) and generate frames between the real content frames", Switch(&Options::detectRepeats)},
        {"content-rates", "auto|FPS,FPS,...", kFrameGeneration,
         "content frame rates the source really produces, e.g. 60,30 for a PS5 at 60 Hz (auto: 60, 48, 45, 40, 30, 24, 20, 15 in a 60 Hz signal)",
         ContentRatesCodec()},
        {"repeat-threshold", "auto|LEVELS", kFrameGeneration,
         "repeat detection: per-sample change in 8-bit levels that counts as new content (auto: 6 for YUV, 8 for RGB with its conversion noise)",
         OptionalInteger(&Options::repeatThreshold, 0, 254, "auto")},
        {"repeat-max-changed", "FRACTION", kFrameGeneration, "repeat detection: largest fraction of changed samples in a repeated frame",
         Number(&Options::repeatMaxChanged, 0, 1)},
        {"repeat-max-tile", "off|N", kFrameGeneration, "repeat detection: most changed samples in any 32x16 tile of a repeated frame",
         OptionalInteger(&Options::repeatMaxTile, 0, 100000, "off")},

        {"sr", kNoneArg, kSuperResolution, "Maxine Video Super Resolution", Switch(&Options::sr)},
        {"sr-quality", "LEVEL", kSuperResolution,
         "super resolution level: low|medium|high|ultra, hb-low..hb-ultra (clean sources), streaming-medium|streaming-ultra, denoise-*, deblur-*, bicubic",
         Quality(&Options::srQuality, FindSrQuality, SrQualityName)},
        {"hdr", kNoneArg, kSuperResolution, "Maxine TrueHDR (SDR to HDR); needs Windows HDR on", Switch(&Options::hdr)},
        {"hdr-after-sr", kNoneArg, kSuperResolution, "run TrueHDR at the output resolution instead of before super resolution", Switch(&Options::hdrAfterSr)},
        {"out", "auto|WxH", kSuperResolution, "processing output size (auto: the display size)", OutCodec()},
        {"sdr-white", "NITS", kSuperResolution, "SDR paper white when composing into HDR10", Number(&Options::sdrWhiteNits, 40, 1000)},

        {"audio", kNoneArg, kAudio, "play the capture card's HDMI audio", Switch(&Options::audio)},
        {"audio-in", "NAME", kAudio, "audio capture endpoint name substring (empty: the capture card's own HDMI audio)", WideText(&Options::audioIn)},
        {"audio-out", "NAME", kAudio, "audio output endpoint name substring (empty: the Windows default output)", WideText(&Options::audioOut)},
        {"audio-volume", "PERCENT", kAudio, "volume (m mutes)", Number(&Options::audioVolumePercent, 0, 200)},
        {"audio-buffer", "MS", kAudio, "audio jitter buffer on top of the device periods", Number(&Options::audioBufferMs, 0, 200)},
        {"audio-offset", "MS", kAudio, "audio delay added to the automatic video sync", Number(&Options::audioOffsetMs, 0, 500)},
        {"audio-sync", kNoneArg, kAudio, "delay the audio to match the measured video latency", Switch(&Options::audioSync)},
        {"audio-tone", kNoneArg, kAudio, "diagnosis: play a quiet 440 Hz tone instead of the captured audio", Switch(&Options::audioTone)},

        {"mode", "auto|passthrough|fixed|vrr", kPacing, "output pacing (auto: probe the display for VRR, then vrr or fixed)",
         Choice(&Options::mode, std::vector<std::pair<const char*, std::optional<PaceMode>>>{
                                    {"auto", std::nullopt}, {"passthrough", PaceMode::Passthrough}, {"fixed", PaceMode::Fixed}, {"vrr", PaceMode::Vrr}})},
        {"fixed-late-latch", kNoneArg, kPacing, "fixed refresh: render as late as possible before the vblank instead of right after it",
         Switch(&Options::fixedLateLatch)},
        {"lead", "MS", kPacing, "initial render lead (it adapts to the frame cost)", Number(&Options::leadMs, 0.2, 50)},
        {"base-delay", "MS", kPacing, "pipeline delay besides the wait for the next content frame (start value, then adapts)", Number(&Options::baseDelayMs, 0, 80)},
        {"margin", "MS", kPacing, "pacing slack margin", Number(&Options::marginMs, 0, 20)},
        {"snap", "FRACTION", kPacing, "show a real frame instead of a generated one when the position is within FRACTION of it", Number(&Options::snap, 0, 0.5)},
        {"passthrough-sync", "N", kPacing, "passthrough: present sync interval (0: immediate, tearing)", Integer(&Options::passthroughSync, 0, 4)},
        {"windowed", kNoneArg, kPacing, "window instead of borderless fullscreen (VRR usually needs fullscreen)", Switch(&Options::windowed)},
        {"monitor", "N", kPacing, "display output index", Integer(&Options::monitor, 0, 16)},

        {"ui", "auto|tui|plain", kLogging, "terminal dashboard or plain log lines (auto: the dashboard on an interactive console)",
         Choice(&Options::ui, std::vector<std::pair<const char*, UiMode>>{{"auto", UiMode::Auto}, {"tui", UiMode::Tui}, {"plain", UiMode::Plain}})},
        {"log-level", "debug|info|warn|error", kLogging, "console and file log level",
         Choice(&Options::logLevel, std::vector<std::pair<const char*, LogLevel>>{
                                        {"debug", LogLevel::Debug}, {"info", LogLevel::Info}, {"warn", LogLevel::Warn}, {"error", LogLevel::Error}})},
        {"log", "FILE", kLogging, "also write the log to FILE", Text(&Options::log)},
        {"report", "SEC", kLogging, "plain console: log the status every SEC seconds (0: only at exit)", Number(&Options::reportSec, 0, 3600)},
        {"report-detail", kNoneArg, kLogging, "print the per-stage p50/p99 table with every report", Switch(&Options::reportDetail)},
        {"warmup", "SEC", kLogging, "reset the statistics once after SEC seconds", Number(&Options::warmupSec, 0, 3600)},
        {"duration", "SEC", kLogging, "run for SEC seconds, then exit (0: until esc)", Number(&Options::durationSec, 0, kMaxSeconds)},
        {"key-script", "SEC:STEP,...", kLogging,
         "SEC seconds after the start press a key (esc, m), run an action (print-stats, reset-stats, save) or set a popup setting, "
         "e.g. 10:fg-quality=medium,20:format=nv12,30:esc",
         KeyScriptCodec()},
        {"csv", "FILE", kLogging, "per-output-frame timing log", Text(&Options::csv)},
        {"timecode", kNoneArg, kLogging, "draw the loopback timecode band (HDMI out -> capture in tests)", Switch(&Options::timecode)},
        {"decode-timecode", kNoneArg, kLogging, "decode the timecode band from captured frames", Switch(&Options::decodeTimecode)},
        {"marker", kNoneArg, kLogging, "flashing corner square for photodiode or slow-motion camera tests", Switch(&Options::marker)},
        {"d3d-debug", kNoneArg, kLogging, "enable the D3D11 debug layer", Switch(&Options::d3dDebug)},
    };
    return table;
}

const OptionDef* Find(std::string_view key) {
    for (const OptionDef& d : Table())
        if (key == d.name) return &d;
    return nullptr;
}

bool IsSwitch(const OptionDef& d) { return d.arg == kNoneArg; }
bool IsConfigSelection(const OptionDef& d) { return &d == Find("config") || &d == Find("no-config"); }

std::string RangeText(const Range& r) { return Fmt(r.lo) + ".." + Fmt(r.hi); }

std::string Expected(const OptionDef& d) {
    if (IsSwitch(d)) return "true|false";
    return d.codec.range ? std::string(d.arg) + " in " + RangeText(*d.codec.range) : std::string(d.arg);
}

struct Arg {
    const OptionDef* def;
    std::string value;
};

std::optional<std::vector<Arg>> SplitArgs(const std::vector<std::string>& args) {
    std::vector<Arg> out;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "-h" || a == "/?") {
            out.push_back({Find("help"), "true"});
            continue;
        }
        if (!a.starts_with("--")) {
            LOGE("unexpected argument '%s' (options start with --)", a.c_str());
            return std::nullopt;
        }
        const size_t eq = a.find('=');
        const bool hasValue = eq != std::string::npos;
        const std::string key = NormKey(std::string_view(a).substr(2, hasValue ? eq - 2 : std::string::npos));
        std::string value = hasValue ? a.substr(eq + 1) : std::string();
        const OptionDef* d = Find(key);
        bool negated = false;
        if (!d && key.starts_with("no-")) {
            d = Find(std::string_view(key).substr(3));
            negated = d && IsSwitch(*d);
            if (!negated) d = nullptr;
        }
        if (!d) {
            LOGE("unknown option --%s (see --help)", key.c_str());
            return std::nullopt;
        }
        if (IsSwitch(*d)) {
            if (!hasValue) value = "true";
            if (negated) {
                const std::optional<bool> b = ParseBool(Trim(value));
                if (!b) {
                    LOGE("invalid value '%s' for --%s (expected true|false)", value.c_str(), key.c_str());
                    return std::nullopt;
                }
                value = Fmt(!*b);
            }
        } else if (!hasValue) {
            if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
                LOGE("missing value for --%s (%s)", d->name, d->arg);
                return std::nullopt;
            }
            value = args[++i];
        }
        out.push_back({d, value});
    }
    return out;
}

bool ApplyArg(const Arg& arg, Target target) {
    if (arg.def->codec.set(target, arg.value)) return true;
    LOGE("invalid value '%s' for --%s (expected %s)", arg.value.c_str(), arg.def->name, Expected(*arg.def).c_str());
    return false;
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t b[MAX_PATH];
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, b, nullptr);
    return n && n < MAX_PATH ? std::wstring(b, n) : p;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<std::string> ReadFileUtf8(const std::wstring& path) {
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.starts_with(kUtf8Bom)) text.erase(0, kUtf8Bom.size());
    return text;
}

bool IsInertLine(std::string_view trimmed) { return trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '['; }

std::optional<std::string> LineKey(std::string_view line) {
    const std::string_view s = Trim(line);
    const size_t eq = s.find('=');
    if (IsInertLine(s) || eq == std::string_view::npos) return std::nullopt;
    return NormKey(s.substr(0, eq));
}

std::optional<std::string> LineValue(std::string_view line) {
    std::string_view v = Trim(line.substr(line.find('=') + 1));
    if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
        const size_t close = v.find(v[0], 1);
        if (close == std::string_view::npos) return std::nullopt;
        return std::string(v.substr(1, close - 1));
    }
    for (size_t i = 1; i < v.size(); i++)
        if ((v[i] == '#' || v[i] == ';') && (v[i - 1] == ' ' || v[i - 1] == '\t')) return std::string(Trim(v.substr(0, i)));
    return std::string(v);
}

int ApplyConfigText(const std::string& text, const std::string& fileName, Target target) {
    std::istringstream in(text);
    int lineNo = 0, problems = 0;
    for (std::string line; std::getline(in, line);) {
        lineNo++;
        const std::string trimmed(Trim(line));
        if (IsInertLine(trimmed)) continue;
        const std::optional<std::string> key = LineKey(line);
        const std::optional<std::string> value = key ? LineValue(line) : std::nullopt;
        const OptionDef* d = key ? Find(*key) : nullptr;
        if (!key) LOGW("%s:%d: expected 'key = value': %s", fileName.c_str(), lineNo, trimmed.c_str());
        else if (!value) LOGW("%s:%d: missing closing quote: %s", fileName.c_str(), lineNo, trimmed.c_str());
        else if (!d) LOGW("%s:%d: unknown option '%s' (ignored)", fileName.c_str(), lineNo, key->c_str());
        else if (d->commandLineOnly) LOGW("%s:%d: '%s' works only on the command line (ignored)", fileName.c_str(), lineNo, key->c_str());
        else if (!d->codec.set(target, *value))
            LOGW("%s:%d: invalid value '%s' for %s (expected %s; ignored)", fileName.c_str(), lineNo, value->c_str(), d->name, Expected(*d).c_str());
        else continue;
        problems++;
    }
    return problems;
}

std::string IniValue(const OptionDef& d, const Options& o) {
    const std::string v = d.codec.get(o);
    const bool quote = !v.empty() && (v.find_first_of("#;=\"'") != std::string::npos || v.front() == ' ' || v.back() == ' ');
    return quote ? "\"" + v + "\"" : v;
}

std::string IniLine(const OptionDef& d, const Options& o) { return std::string(d.name) + " = " + IniValue(d, o); }

std::string HelpText(const OptionDef& d, const Options& defaults) {
    std::string notes;
    if (d.codec.range) notes += RangeText(*d.codec.range);
    if (!d.commandLineOnly) {
        const std::string v = d.codec.get(defaults);
        if (!v.empty()) notes += (notes.empty() ? "default: " : ", default: ") + v;
    }
    if (IsSettingName(d.name)) notes += notes.empty() ? "also in the settings popup" : "; also in the settings popup";
    return notes.empty() ? std::string(d.help) : std::string(d.help) + style::Dim(" (" + notes + ")");
}

void PrintUsage() {
    constexpr std::pair<const char*, const char*> kKeys[] = {{"s", "settings (terminal dashboard)"}, {"m", "mute"}, {"esc", "quit"}};
    const Options defaults;
    std::string out;
    out += style::Bold(style::Accent("CameraVFX")) + " - capture -> frame generation -> super resolution -> HDR -> low-latency display\n\n";
    out += style::Bold("usage:") + " CameraVFX [--option value | --option=value | --switch | --no-switch] ...\n";
    out += style::Dim(std::string("Options can also be written into ") + kConfigFileName +
                      " as 'option = value'; the command line overrides the file. --print-config shows the effective settings.") + "\n";
    const char* group = "";
    for (const OptionDef& d : Table()) {
        if (strcmp(group, d.group)) {
            group = d.group;
            out += "\n" + style::Bold(style::Accent(std::string("[") + group + "]")) + "\n";
        }
        const std::string arg = IsSwitch(d) ? std::string() : std::string(" ") + d.arg;
        const size_t plainWidth = strlen("  --") + strlen(d.name) + arg.size();
        const std::string left = "  " + style::Bold(std::string("--") + d.name) + arg;
        out += left + (plainWidth < kHelpColumn ? std::string(kHelpColumn - plainWidth, ' ') : "\n" + std::string(kHelpColumn, ' ')) +
               HelpText(d, defaults) + "\n";
    }
    out += "\n" + style::Bold(style::Accent("keys:"));
    for (const auto& [key, action] : kKeys) out += " " + style::Bold(key) + " " + action + style::Dim(" |");
    out.resize(out.size() - style::Dim(" |").size());
    out += "\n";
    fputs(out.c_str(), stdout);
    fflush(stdout);
}

void PrintConfig(const Options& o, bool loaded) {
    printf("# CameraVFX effective configuration%s\n", loaded ? (" (file: " + o.config + ")").c_str() : "");
    const char* group = "";
    for (const OptionDef& d : Table()) {
        if (d.commandLineOnly) continue;
        if (strcmp(group, d.group)) {
            group = d.group;
            printf("\n[%s]\n", group);
        }
        printf("%s\n", IniLine(d, o).c_str());
    }
}

}

ParseResult LoadOptions(int argc, wchar_t** argv, Options& o) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(WideToUtf8(argv[i]));
    const std::optional<std::vector<Arg>> split = SplitArgs(args);
    if (!split) return ParseResult::Error;
    ParseActions actions;
    const Target target{o, actions};
    for (const Arg& arg : *split)
        if (IsConfigSelection(*arg.def) && !ApplyArg(arg, target)) return ParseResult::Error;

    const bool explicitFile = !o.config.empty();
    const std::wstring path = FullPath(explicitFile ? Utf8ToWide(o.config.c_str()) : ExeDir() + L"\\" + Utf8ToWide(kConfigFileName));
    o.config = WideToUtf8(path.c_str());
    const bool exists = FileExists(path);
    if (explicitFile && !exists && !actions.noConfig) {
        LOGE("configuration file not found: %s", o.config.c_str());
        return ParseResult::Error;
    }
    const bool loaded = exists && !actions.noConfig;
    if (loaded) {
        const std::optional<std::string> text = ReadFileUtf8(path);
        if (!text) {
            LOGE("cannot read %s", o.config.c_str());
            return ParseResult::Error;
        }
        if (ApplyConfigText(*text, o.config, target)) LOGW("config: %s has problems (see above)", o.config.c_str());
    }

    for (const Arg& arg : *split)
        if (!IsConfigSelection(*arg.def) && !ApplyArg(arg, target)) return ParseResult::Error;
    if (actions.help) {
        PrintUsage();
        return ParseResult::Exit;
    }
    if (actions.printConfig) {
        PrintConfig(o, loaded);
        return ParseResult::Exit;
    }
    if (o.synthetic && o.syntheticContentFps > o.capture.rateHz) {
        LOGE("synthetic-content-rate %s fps is faster than the %s Hz capture signal", Fmt(*o.syntheticContentFps).c_str(), Fmt(o.capture.rateHz).c_str());
        return ParseResult::Error;
    }
    if (loaded) LOGI("config: %s", o.config.c_str());
    return ParseResult::Run;
}

bool SaveOptions(const Options& o, const std::vector<std::string>& keys) {
    const std::wstring path = Utf8ToWide(o.config.c_str());
    const std::string text = ReadFileUtf8(path).value_or("");
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    for (const std::string& key : keys) {
        const OptionDef* d = Find(key);
        const std::string line = IniLine(*d, o);
        const auto existing = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return LineKey(l) == key; });
        if (existing != lines.end()) {
            *existing = line;
            continue;
        }
        const std::string header = std::string("[") + d->group + "]";
        const auto section = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return Trim(l) == header; });
        if (section == lines.end()) {
            if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
            lines.push_back(header);
            lines.push_back(line);
            continue;
        }
        auto insertAfter = section;
        for (auto it = std::next(section); it != lines.end() && !Trim(*it).starts_with("["); ++it)
            if (!IsInertLine(Trim(*it))) insertAfter = it;
        lines.insert(std::next(insertAfter), line);
    }
    std::string out;
    for (const std::string& l : lines) out += l + newline;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

}
