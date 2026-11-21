#include "config.h"
#include <functional>
#include <cwctype>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <sstream>
#include <charconv>

namespace camss {

// ------------------------------------------------------------------ value parsing
static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
static bool ParseBool(const std::string& v, bool& out) {
    std::string s = Lower(Trim(v));
    if (s == "1" || s == "true" || s == "on" || s == "yes") { out = true; return true; }
    if (s == "0" || s == "false" || s == "off" || s == "no") { out = false; return true; }
    return false;
}
static bool ParseDouble(const std::string& v, double& out, double lo, double hi) {
    std::string s = Trim(v);
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    double d = strtod(s.c_str(), &end);
    if (errno || !end || *end || !std::isfinite(d) || d < lo || d > hi) return false;
    out = d;
    return true;
}
static bool ParseInt(const std::string& v, int& out, int lo, int hi) {
    double d;
    if (!ParseDouble(v, d, lo, hi) || d != std::floor(d)) return false;
    out = (int)d;
    return true;
}
static std::string Fmt(double v) {
    char b[64];
    auto r = std::to_chars(b, b + sizeof(b), v);   // shortest text that reads back to the same value
    return std::string(b, r.ptr);
}
// "WxH" / "WxH@FPS" with plain decimal sizes (no sign, no trailing text), sizes 16..16384; spaces allowed.
static bool ParseSize(const std::string& in, UINT& w, UINT& h, double* fps) {
    std::string s;
    for (char c : Lower(Trim(in))) if (c != ' ' && c != '\t') s += c;
    size_t x = s.find('x'), at = s.find('@');
    if (x == std::string::npos || (at != std::string::npos && at < x) || (!fps && at != std::string::npos)) return false;
    auto num = [](const std::string& t, uint64_t& v) {
        if (t.empty() || t.size() > 6) return false;
        for (char c : t) if (c < '0' || c > '9') return false;
        v = std::stoull(t);
        return true;
    };
    uint64_t W, H;
    if (!num(s.substr(0, x), W) || !num(s.substr(x + 1, at == std::string::npos ? std::string::npos : at - x - 1), H)) return false;
    if (W < 16 || H < 16 || W > 16384 || H > 16384) return false;
    if (at != std::string::npos) {
        double f;
        if (!ParseDouble(s.substr(at + 1), f, 1, 1000)) return false;
        *fps = f;
    }
    w = (UINT)W; h = (UINT)H;
    return true;
}
static std::string Fmt(int v) { return std::to_string(v); }
static std::string Fmt(bool v) { return v ? "true" : "false"; }

// ------------------------------------------------------------------ quality names
static const std::pair<const char*, int> kSrLevels[] = {
    {"bicubic", 0}, {"low", 1}, {"medium", 2}, {"high", 3}, {"ultra", 4},
    {"denoise-low", 8}, {"denoise-medium", 9}, {"denoise-high", 10}, {"denoise-ultra", 11},
    {"deblur-low", 12}, {"deblur-medium", 13}, {"deblur-high", 14}, {"deblur-ultra", 15},
    {"hb-low", 16}, {"hb-medium", 17}, {"hb-high", 18}, {"hb-ultra", 19},
    {"streaming-medium", 21}, {"streaming-ultra", 23},
};
static const char* kFgNames[] = {"low", "medium", "high"};

const char* FgQualityName(int m) { return m >= 0 && m <= 2 ? kFgNames[m] : "?"; }
const char* SrQualityName(int level) {
    for (auto& kv : kSrLevels) if (kv.second == level) return kv.first;
    return "?";
}
int NextSrQuality(int level) {
    static const std::vector<std::vector<int>> families = {
        {0}, {1, 2, 3, 4}, {8, 9, 10, 11}, {12, 13, 14, 15}, {16, 17, 18, 19}, {21, 23}};
    for (auto& f : families)
        for (size_t i = 0; i < f.size(); i++)
            if (f[i] == level) return f[(i + 1) % f.size()];
    return level;
}
static bool ParseSrQuality(const std::string& v, int& out) {
    std::string s = Lower(Trim(v));
    if (s.rfind("highbitrate-", 0) == 0) s = "hb-" + s.substr(12);
    for (auto& kv : kSrLevels) if (s == kv.first) { out = kv.second; return true; }
    int n;
    if (!ParseInt(s, n, 0, 23)) return false;
    for (auto& kv : kSrLevels) if (kv.second == n) { out = n; return true; }
    return false;
}
static bool ParseFgQuality(const std::string& v, int& out) {
    std::string s = Lower(Trim(v));
    for (int i = 0; i < 3; i++) if (s == kFgNames[i]) { out = i; return true; }
    return ParseInt(s, out, 0, 2);
}

// ------------------------------------------------------------------ option table
struct OptDef {
    const char* name;
    const char* arg;        // nullptr = switch (true/false)
    const char* group;
    const char* help;
    std::function<bool(Options&, const std::string&)> set;
    std::function<std::string(const Options&)> get;   // nullptr = command line only (actions)
    const char* alias = nullptr;
};

static const char* PixFmtArg(PixFmt f) {
    switch (f) {
    case PixFmt::NV12: return "nv12";
    case PixFmt::P010: return "p010";
    case PixFmt::RGB24: return "rgb24";
    case PixFmt::RGB32: return "rgb32";
    default: return "yuy2";
    }
}

#define SW(field) [](Options& o, const std::string& v) { return ParseBool(v, o.field); }, [](const Options& o) { return Fmt(o.field); }
#define DBL(field, lo, hi) [](Options& o, const std::string& v) { return ParseDouble(v, o.field, lo, hi); }, [](const Options& o) { return Fmt(o.field); }
#define INT(field, lo, hi) [](Options& o, const std::string& v) { return ParseInt(v, o.field, lo, hi); }, [](const Options& o) { return Fmt(o.field); }
#define STR(field) [](Options& o, const std::string& v) { o.field = v; return true; }, [](const Options& o) { return o.field; }
#define WSTR(field) [](Options& o, const std::string& v) { o.field = Utf8ToWide(v.c_str()); return true; }, [](const Options& o) { return WideToUtf8(o.field.c_str()); }
#define ONEOF(field, ...) [](Options& o, const std::string& v) { std::string s = Lower(Trim(v)); for (const char* c : {__VA_ARGS__}) if (s == c) { o.field = s; return true; } return false; }, [](const Options& o) { return o.field; }
#define ACTION(body) [](Options& o, const std::string& v) { (void)v; body; return true; }, nullptr

static const std::vector<OptDef>& Table() {
    static const std::vector<OptDef> t = {
        // ---- actions (command line only)
        {"help", nullptr, "actions", "show this help", ACTION(o.help = true)},
        {"list", nullptr, "actions", "list video capture devices, their native formats and the audio endpoints", ACTION(o.list = true)},
        {"selftest", nullptr, "actions", "offline quality / timing self test of the processing stages", ACTION(o.selftest = true)},
        {"print-config", nullptr, "actions", "print the effective options in configuration-file syntax and exit", ACTION(o.printConfig = true)},
        {"config", "FILE", "actions", "configuration file (default: camss.ini in the current directory, then next to camss.exe)", ACTION((void)0)},
        {"no-config", nullptr, "actions", "do not read a configuration file", ACTION((void)0)},

        // ---- capture
        {"device", "NAME|INDEX", "capture", "video capture device: substring of its name, or index from --list (default: the first device)",
         [](Options& o, const std::string& v) {
             std::string s = Trim(v);
             int idx;
             if (!s.empty() && s.size() < 3 && ParseInt(s, idx, 0, 99)) { o.deviceIndex = idx; o.device.clear(); }
             else { o.device = Utf8ToWide(s.c_str()); o.deviceIndex = -1; }
             return true;
         },
         [](const Options& o) { return o.deviceIndex >= 0 ? Fmt(o.deviceIndex) : WideToUtf8(o.device.c_str()); }},
        {"format", "yuy2|nv12|p010|rgb24|rgb32", "capture", "preferred native capture format",
         [](Options& o, const std::string& v) {
             std::string s = Lower(Trim(v));
             if (s == "yuy2") o.format = PixFmt::YUY2; else if (s == "nv12") o.format = PixFmt::NV12;
             else if (s == "p010") o.format = PixFmt::P010; else if (s == "rgb24") o.format = PixFmt::RGB24;
             else if (s == "rgb32" || s == "xrgb") o.format = PixFmt::RGB32; else return false;
             return true;
         },
         [](const Options& o) { return std::string(PixFmtArg(o.format)); }},
        {"capture", "WxH@FPS", "capture", "capture mode",
         [](Options& o, const std::string& v) {
             UINT w = 0, h = 0; double f = o.capFps;
             if (!ParseSize(v, w, h, &f)) return false;
             o.capW = w; o.capH = h; o.capFps = f;
             return true;
         },
         [](const Options& o) { return std::to_string(o.capW) + "x" + std::to_string(o.capH) + "@" + Fmt(o.capFps); }},
        {"range", "limited|full", "capture", "YCbCr range",
         [](Options& o, const std::string& v) {
             std::string s = Lower(Trim(v));
             if (s != "limited" && s != "full") return false;
             o.color.fullRange = s == "full";
             return true;
         },
         [](const Options& o) { return std::string(o.color.fullRange ? "full" : "limited"); }},
        {"bt601", nullptr, "capture", "BT.601 matrix (SD sources) instead of BT.709", SW(color.bt601)},
        {"synthetic", nullptr, "capture", "built-in moving test pattern instead of a capture device", SW(synthetic)},
        {"synthetic-content", "FPS", "capture", "synthetic source: content frame rate inside the signal (e.g. 30 = every frame twice; 0 = signal rate)", DBL(syntheticContentFps, 0, 1000)},

        // ---- frame generation
        {"interp", "none|blend|nvof|vfx", "frame generation", "frame generation (vfx = Maxine Video Frame Generation)", ONEOF(interp, "none", "blend", "nvof", "vfx")},
        {"fg-quality", "low|medium|high", "frame generation", "Maxine frame generation quality (key F cycles at run time)",
         [](Options& o, const std::string& v) { return ParseFgQuality(v, o.vfgMode); },
         [](const Options& o) { return std::string(FgQualityName(o.vfgMode)); }, "vfg-mode"},
        {"fg-multiplier", "N", "frame generation", "output frames per content frame with VRR: 2 = 60->120 fps and 30->60 fps", INT(fgMultiplier, 1, 8), "k"},
        {"fg-target-fps", "FPS", "frame generation", "instead of a fixed multiplier: pick it so the output is about FPS (0 = off)", DBL(fgTargetFps, 0, 1000)},
        {"fg-adaptive", nullptr, "frame generation", "detect repeated frames (e.g. a 30 fps game in a 60 Hz signal) and interpolate the real content frames", SW(fgAdaptive)},
        {"repeat-threshold", "LEVELS", "frame generation", "repeat detection: per-sample change (8-bit levels) that counts as new content", INT(repeatThreshold, 0, 255)},
        {"repeat-max-changed", "FRACTION", "frame generation", "repeat detection: max fraction of changed samples for a repeated frame", DBL(repeatMaxChanged, 0, 1)},
        {"repeat-max-tile", "N", "frame generation", "repeat detection: max changed samples in any ~32x16 px tile (-1 = no local test)", INT(repeatMaxTile, -1, 100000)},
        {"nvof-quality", "0|1|2", "frame generation", "NVOF interpolator quality", INT(nvofQuality, 0, 2)},
        {"snap", "EPS", "frame generation", "snap |t| < EPS to a real frame", DBL(snapEps, 0, 0.5)},

        // ---- super resolution / HDR
        {"sr", "none|rtx|vfx", "super resolution / HDR", "super resolution (rtx = driver RTX VSR, vfx = Maxine VSR)", ONEOF(sr, "none", "rtx", "vfx")},
        {"sr-quality", "LEVEL", "super resolution / HDR",
         "Maxine VSR level: low|medium|high|ultra, hb-low..hb-ultra (clean sources), streaming-medium|streaming-ultra, "
         "denoise-*, deblur-*, bicubic, or the number (key Q cycles within the family)",
         [](Options& o, const std::string& v) { return ParseSrQuality(v, o.srQuality); },
         [](const Options& o) { return std::string(SrQualityName(o.srQuality)); }},
        {"hdr", "none|rtx|vfx", "super resolution / HDR", "SDR->HDR (TrueHDR); needs Windows HDR on for correct display", ONEOF(hdr, "none", "rtx", "vfx")},
        {"hdr-after-sr", nullptr, "super resolution / HDR", "VFX: run TrueHDR at output resolution (default: before VSR)",
         [](Options& o, const std::string& v) { bool b; if (!ParseBool(v, b)) return false; o.hdrBeforeSr = !b; return true; },
         [](const Options& o) { return Fmt(!o.hdrBeforeSr); }},
        {"out", "WxH", "super resolution / HDR", "processing output size (default: display size)",
         [](Options& o, const std::string& v) {
             std::string s = Lower(Trim(v));
             if (s == "auto" || s == "0" || s.empty()) { o.outW = o.outH = 0; return true; }
             UINT w = 0, h = 0;
             if (!ParseSize(s, w, h, nullptr)) return false;
             o.outW = w; o.outH = h;
             return true;
         },
         [](const Options& o) { if (!o.outW) return std::string("auto"); char b[32]; snprintf(b, sizeof(b), "%ux%u", o.outW, o.outH); return std::string(b); }},
        {"white", "NITS", "super resolution / HDR", "SDR paper white when composing into HDR10",
         [](Options& o, const std::string& v) { double d; if (!ParseDouble(v, d, 40, 1000)) return false; o.sdrWhiteNits = (float)d; return true; },
         [](const Options& o) { return Fmt((double)o.sdrWhiteNits); }},

        // ---- audio
        {"audio", nullptr, "audio", "play the capture card's HDMI audio (--no-audio to disable)", SW(audio)},
        {"audio-device", "NAME", "audio", "audio capture endpoint (substring; default: the capture card's own HDMI audio)", WSTR(audioDevice)},
        {"audio-out", "NAME", "audio", "audio output endpoint (substring; default: the Windows default output)", WSTR(audioOut)},
        {"audio-volume", "PERCENT", "audio", "volume 0..200 (keys + / - at run time, M mutes)", DBL(audioVolume, 0, 200)},
        {"audio-buffer", "MS", "audio", "audio jitter buffer on top of the device periods", DBL(audioBufferMs, 0, 200)},
        {"audio-delay", "MS", "audio", "extra audio delay", DBL(audioDelayMs, 0, 500)},
        {"audio-sync", nullptr, "audio", "delay audio automatically to match the measured video latency", SW(audioSync)},
        {"audio-tone", nullptr, "audio", "diagnosis: play a quiet 440 Hz tone instead of the captured audio", SW(audioTone)},

        // ---- pacing / display
        {"mode", "auto|passthrough|fixed|vrr", "pacing / display", "output pacing (auto: probe the display for VRR)",
         [](Options& o, const std::string& v) {
             std::string s = Lower(Trim(v));
             if (s == "auto") { o.autoMode = true; o.mode = PaceMode::Fixed; }
             else if (s == "passthrough") { o.autoMode = false; o.mode = PaceMode::Passthrough; }
             else if (s == "fixed") { o.autoMode = false; o.mode = PaceMode::Fixed; }
             else if (s == "vrr") { o.autoMode = false; o.mode = PaceMode::Vrr; }
             else return false;
             return true;
         },
         [](const Options& o) { return std::string(o.autoMode ? "auto" : o.mode == PaceMode::Passthrough ? "passthrough" : o.mode == PaceMode::Vrr ? "vrr" : "fixed"); }},
        {"latch", nullptr, "pacing / display", "fixed refresh: render as late as possible before the vblank (--no-latch: right after it)", SW(lateLatch)},
        {"lead", "MS", "pacing / display", "initial render lead", DBL(leadMs, 0.2, 50)},
        {"delay", "MS", "pacing / display", "initial pipeline delay (0 = estimated, then adapts)", DBL(initialDelayMs, 0, 80)},
        {"margin", "MS", "pacing / display", "pacing slack margin", DBL(marginMs, 0, 20)},
        {"sync", "0..4", "pacing / display", "passthrough: present interval (0 = immediate, tearing)", INT(passSync, 0, 4)},
        {"windowed", nullptr, "pacing / display", "window instead of borderless fullscreen (VRR usually needs fullscreen)", SW(windowed)},
        {"monitor", "N", "pacing / display", "display output index", INT(monitor, 0, 16)},

        // ---- terminal UI
        {"ui", "auto|tui|plain", "logging / measurement", "terminal dashboard (auto: when the console is interactive) or plain log lines", ONEOF(ui, "auto", "tui", "plain")},
        {"history", "FILE", "logging / measurement", "append each setting's measured latency to this CSV (empty = off)", STR(historyPath)},
        // ---- logging / measurement
        {"log-level", "debug|info|warn|error", "logging / measurement", "console / file log level", ONEOF(logLevel, "debug", "info", "warn", "error")},
        {"log", "FILE", "logging / measurement", "also write the log to FILE", STR(logPath)},
        {"report", "SEC", "logging / measurement", "status line interval (0 = off)", DBL(reportSec, 0, 3600)},
        {"report-detail", nullptr, "logging / measurement", "print the full per-stage p50/p99 table with every report (key L prints it once)", SW(reportDetail)},
        {"warmup", "SEC", "logging / measurement", "reset statistics once after SEC seconds", DBL(warmupSec, 0, 3600)},
        {"duration", "SEC", "logging / measurement", "run for SEC seconds then exit (0 = until ESC)", DBL(durationSec, 0, 1e7)},
        {"csv", "FILE", "logging / measurement", "per-output-frame timing log", STR(csvPath)},
        {"timecode", nullptr, "logging / measurement", "draw the loopback timecode band (HDMI out -> capture in tests; key T)", SW(timecode)},
        {"decode-timecode", nullptr, "logging / measurement", "decode the timecode band from captured frames", SW(decodeTimecode)},
        {"marker", nullptr, "logging / measurement", "flashing corner square for photodiode / slow-motion camera tests", SW(marker)},
        {"debug", nullptr, "logging / measurement", "D3D11 debug layer and debug log level", SW(debug)},
        {"stats-debug", nullptr, "logging / measurement", "dump raw DXGI frame statistics for the first frames", SW(statsDebug)},
    };
    return t;
}

static std::string NormKey(std::string k) {
    k = Lower(Trim(k));
    for (auto& c : k) if (c == '_') c = '-';
    return k;
}

static const OptDef* Find(const std::string& key) {
    for (auto& d : Table()) if (key == d.name || (d.alias && key == d.alias)) return &d;
    return nullptr;
}

// ------------------------------------------------------------------ configuration file
static bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
    return true;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ExeDir() {
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring s(b, n);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

static std::wstring FullPath(const std::wstring& p) {
    wchar_t b[MAX_PATH];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, b, nullptr);
    return n && n < MAX_PATH ? std::wstring(b, n) : p;
}

// Applies "key = value" lines. Bad lines and unknown keys are warnings, not errors.
static int ApplyConfigText(const std::string& text, const std::string& fileName, Options& o) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0, problems = 0;
    while (std::getline(in, line)) {
        lineNo++;
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) { LOGW("%s:%d: expected 'key = value': %s", fileName.c_str(), lineNo, s.c_str()); problems++; continue; }
        std::string key = NormKey(s.substr(0, eq)), val = Trim(s.substr(eq + 1));
        if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
            // Quoted value: taken literally up to the matching quote; a comment may follow it.
            size_t close = val.find(val[0], 1);
            if (close == std::string::npos) { LOGW("%s:%d: missing closing quote: %s", fileName.c_str(), lineNo, s.c_str()); problems++; continue; }
            val = val.substr(1, close - 1);
        } else {
            // Inline comment: " #" or " ;" (names may contain '#' without a leading space).
            for (size_t i = 1; i < val.size(); i++)
                if ((val[i] == '#' || val[i] == ';') && (val[i - 1] == ' ' || val[i - 1] == '\t')) { val = Trim(val.substr(0, i)); break; }
        }
        const OptDef* d = Find(key);
        if (!d || !d->get) { LOGW("%s:%d: unknown option '%s' (ignored)", fileName.c_str(), lineNo, key.c_str()); problems++; continue; }
        if (!d->set(o, val)) { LOGW("%s:%d: invalid value '%s' for %s (expected %s; ignored)", fileName.c_str(), lineNo, val.c_str(), d->name, d->arg ? d->arg : "true|false"); problems++; }
    }
    return problems;
}

// ------------------------------------------------------------------ entry points
ParseResult LoadOptions(int argc, wchar_t** argv, Options& o) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(WideToUtf8(argv[i]));

    // Pass 1: --config / --no-config.
    std::wstring explicitConfig;
    bool noConfig = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].rfind("--", 0) != 0) continue;
        std::string key = args[i].substr(2), val;
        size_t eq = key.find('=');
        const bool hasVal = eq != std::string::npos;
        if (hasVal) { val = key.substr(eq + 1); key = key.substr(0, eq); }
        key = NormKey(key);
        if (key == "no-config") {
            bool b = true;
            if (hasVal && !ParseBool(val, b)) { LOGE("invalid value '%s' for --no-config", val.c_str()); return ParseResult::Error; }
            noConfig = b;
        } else if (key == "config") {
            if (!hasVal) {
                if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) { LOGE("missing value for --config (FILE)"); return ParseResult::Error; }
                val = args[++i];
            }
            if (Trim(val).empty()) { LOGE("missing value for --config (FILE)"); return ParseResult::Error; }
            explicitConfig = Utf8ToWide(val.c_str());
        } else {
            // skip the value of an option that takes one, so it is never mistaken for --config
            const OptDef* d = Find(key);
            if (d && d->arg && !hasVal) i++;
        }
    }
    if (!noConfig) {
        std::wstring path;
        if (!explicitConfig.empty()) {
            if (!FileExists(explicitConfig)) { LOGE("configuration file not found: %s", WideToUtf8(explicitConfig.c_str()).c_str()); return ParseResult::Error; }
            path = explicitConfig;
        } else if (FileExists(L"camss.ini")) path = L"camss.ini";
        else if (FileExists(ExeDir() + L"\\camss.ini")) path = ExeDir() + L"\\camss.ini";
        if (!path.empty()) {
            std::string text;
            std::string name = WideToUtf8(FullPath(path).c_str());
            if (!ReadFileUtf8(path, text)) { LOGE("cannot read %s", name.c_str()); return ParseResult::Error; }
            int problems = ApplyConfigText(text, name, o);
            o.configPath = name;
            if (problems) LOGW("config: %s has problems (see above)", name.c_str());
        }
    }

    // Pass 2: command line (overrides the file).
    for (size_t i = 0; i < args.size(); i++) {
        std::string a = args[i];
        if (a == "-h" || a == "/?") { o.help = true; continue; }
        if (a.rfind("--", 0) != 0) { LOGE("unexpected argument '%s' (options start with --)", a.c_str()); return ParseResult::Error; }
        std::string key = a.substr(2), val;
        bool hasVal = false;
        size_t eq = key.find('=');
        if (eq != std::string::npos) { val = key.substr(eq + 1); key = key.substr(0, eq); hasVal = true; }
        key = NormKey(key);
        if (key == "config") { if (!hasVal) i++; continue; }
        if (key == "no-config") continue;
        const OptDef* d = Find(key);
        bool negate = false;
        if (!d && key.rfind("no-", 0) == 0) {
            d = Find(key.substr(3));
            if (d && !d->arg && d->get) negate = true; else d = nullptr;
        }
        if (!d) { LOGE("unknown option --%s (see --help)", key.c_str()); return ParseResult::Error; }
        if (!d->arg) {
            if (negate) {
                bool b = true;
                if (hasVal && !ParseBool(val, b)) { LOGE("invalid value '%s' for --no-%s (expected true|false)", val.c_str(), d->name); return ParseResult::Error; }
                val = b ? "false" : "true";
            } else if (!hasVal) val = "true";
        } else if (!hasVal) {
            if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
                LOGE("missing value for --%s (%s)", d->name, d->arg);
                return ParseResult::Error;
            }
            val = args[++i];
        }
        if (!d->set(o, val)) { LOGE("invalid value '%s' for --%s (expected %s)", val.c_str(), d->name, d->arg ? d->arg : "true|false"); return ParseResult::Error; }
    }
    if (o.help) { PrintUsage(); return ParseResult::Exit; }
    if (!o.printConfig && !o.configPath.empty()) LOGI("config: %s", o.configPath.c_str());
    if (o.printConfig) { PrintConfig(o); return ParseResult::Exit; }
    return ParseResult::Run;
}

// Run-time keys, as listed by --help (two rows).
static const std::pair<const char*, const char*> kUsageKeys[2][6] = {
    {{"ESC", "quit"}, {"I", "frame generation on/off"}, {"F", "frame generation quality"}, {"S", "super resolution on/off"}, {nullptr, nullptr}, {nullptr, nullptr}},
    {{"Q", "super resolution quality"}, {"M", "mute"}, {"+ / -", "volume"}, {"L", "detailed statistics"}, {"R", "reset statistics"}, {"T", "timecode"}},
};

// --help. On a VT console: accent section headers, bold option names and keys, dim
// defaults / separators. Redirected (or a console without VT): the same plain text as ever
// (the style:: helpers return their input unchanged).
void PrintUsage() {
    const Options def;
    std::string out;
    out += style::Bold(style::Accent("camss")) + " - capture -> frame generation -> super resolution -> HDR -> low-latency display\n\n";
    out += style::Bold("usage:") + " camss [--option value | --switch | --no-switch | --option=value] ...\n";
    out += style::Dim("Options can also be written into camss.ini as 'option = value' (see camss.ini in the source tree);\n"
                      "the command line overrides the file. --print-config shows the effective settings.") + "\n";
    const char* group = "";
    for (auto& d : Table()) {
        if (strcmp(group, d.group)) { group = d.group; out += "\n" + style::Bold(style::Accent(std::string("[") + group + "]")) + "\n"; }
        // Column layout is computed on the plain text; colours are added around the pieces.
        const std::string arg = d.arg ? std::string(" ") + d.arg : std::string();
        const size_t leftLen = 4 + strlen(d.name) + arg.size();   // "  --" name arg
        std::string left = "  " + style::Bold(std::string("--") + d.name) + arg;
        std::string help = d.help;
        if (d.get && !strstr(d.help, "default:")) {
            std::string v = d.get(def);
            help += style::Dim(" (default: " + (v.empty() ? std::string("none") : v) + ")");
        }
        if (d.alias) help += style::Dim(std::string(" [alias --") + d.alias + "]");
        if (leftLen > 34) out += left + "\n" + std::string(35, ' ') + help + "\n";
        else out += left + std::string(35 - leftLen, ' ') + help + "\n";
    }
    out += "\n";
    for (int row = 0; row < 2; row++) {
        out += row == 0 ? style::Bold(style::Accent("keys:")) + " " : std::string(6, ' ');
        for (int k = 0; k < 6 && kUsageKeys[row][k].first; k++) {
            if (k) out += style::Dim(" |") + " ";
            out += style::Bold(kUsageKeys[row][k].first) + " " + kUsageKeys[row][k].second;
        }
        out += "\n";
    }
    fputs(out.c_str(), stdout);
    fflush(stdout);
}

void PrintConfig(const Options& o) {
    printf("# camss effective configuration%s%s\n", o.configPath.empty() ? "" : " (file: ", o.configPath.empty() ? "" : (o.configPath + ")").c_str());
    const char* group = "";
    for (auto& d : Table()) {
        if (!d.get) continue;
        if (strcmp(group, d.group)) { group = d.group; printf("\n[%s]\n", group); }
        std::string v = d.get(o);
        const bool quote = !v.empty() && (v.find_first_of("#;=\"'") != std::string::npos || v.front() == ' ' || v.back() == ' ');
        if (quote) printf("%s = \"%s\"\n", d.name, v.c_str());
        else printf("%s = %s\n", d.name, v.c_str());
    }
}

} // namespace camss
