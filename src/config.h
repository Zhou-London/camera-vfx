// Options: one table drives the command line, the configuration file (camss.ini) and the
// help text. Precedence: built-in defaults < configuration file < command line.
//
// Configuration file: UTF-8 "key = value" lines; keys are the long option names without
// "--" ('_' and '-' are equivalent); '#' or ';' start a comment; [sections] are allowed for
// grouping and ignored. Switches take true/false (also on/off, yes/no, 1/0).
// Lookup: --config FILE, else camss.ini in the current directory, else next to camss.exe.
#pragma once
#include "util/common.h"
#include "capture/mf_capture.h"
#include "gfx/color_convert.h"
#include "pacing/pacer.h"
#include <string>
#include <vector>

namespace camss {

struct Options {
    // capture
    std::wstring device;
    int deviceIndex = -1;
    PixFmt format = PixFmt::YUY2;
    UINT capW = 1920, capH = 1080;
    double capFps = 60.0;
    bool synthetic = false;
    double syntheticContentFps = 0;  // synthetic source: content rate inside the signal (0 = signal rate)
    ColorOptions color;
    // pacing
    PaceMode mode = PaceMode::Fixed;
    bool autoMode = true;            // probe the display for VRR, then pick vrr or fixed
    bool lateLatch = true;
    double leadMs = 3.0;             // initial render lead (late latch / VRR)
    double initialDelayMs = 0;       // 0 = estimate from mode
    double marginMs = 1.0;
    double snapEps = 0.06;
    int passSync = 1;                // passthrough: present sync interval
    // frame generation
    std::string interp = "none";     // none | blend | nvof | vfx
    int vfgMode = 0;                 // VFX frame generation quality: 0 low, 1 medium, 2 high
    int nvofQuality = 1;
    int fgMultiplier = 2;            // output frames per content frame (VRR): 60 -> 120, 30 -> 60
    double fgTargetFps = 0;          // > 0: choose the multiplier so output ~ this rate (overrides fgMultiplier)
    bool fgAdaptive = true;          // detect repeated frames (30 fps content in 60 Hz) and interpolate the real content
    int repeatThreshold = 6;         // repeat detection: per-sample difference (8-bit levels) that counts as change
    double repeatMaxChanged = 2e-5;  // repeat detection: max fraction of changed samples for a repeat
    int repeatMaxTile = 16;          // repeat detection: max changed samples in one tile (-1 = off)
    // super resolution / HDR
    std::string sr = "none";         // none | rtx | vfx
    int srQuality = 21;              // VFX VSR QualityLevel (21 = streaming medium)
    std::string hdr = "none";        // none | rtx | vfx
    bool hdrBeforeSr = true;         // VFX chain: TrueHDR at source resolution, then VSR in RGB10A2
    UINT outW = 0, outH = 0;         // processing output size (0 = display size)
    float sdrWhiteNits = 203.f;
    // audio
    bool audio = true;               // monitor the capture card's HDMI audio
    std::wstring audioDevice;        // capture endpoint name substring (empty = the capture card's own endpoint)
    std::wstring audioOut;           // render endpoint name substring (empty = default output)
    double audioVolume = 100;        // percent, 0..200
    double audioBufferMs = 10;       // jitter buffer on top of the device periods (grows by itself after underruns)
    double audioDelayMs = 0;         // extra delay
    bool audioSync = true;           // delay audio to match the measured video latency
    bool audioTone = false;          // diagnosis: test tone instead of the captured audio
    // presentation
    bool windowed = false;
    int monitor = 0;
    // terminal UI
    std::string ui = "auto";         // auto | tui | plain (auto: dashboard when the console is interactive)
    std::string historyPath = "out/history.csv";   // per-setting latency record (CSV, appended; empty = off)
    // logging / measurement
    std::string logLevel = "info";
    std::string logPath;
    double reportSec = 2.0;
    bool reportDetail = false;       // full per-stage table in every report (otherwise one summary line)
    double durationSec = 0;          // 0 = until ESC
    std::string csvPath;
    bool timecode = false;           // draw loopback timecode band
    bool decodeTimecode = false;     // decode timecode band from capture (HDMI loopback)
    bool marker = false;             // flashing photodiode marker square
    double warmupSec = 6.0;          // statistics are reset once after this
    bool debug = false;
    bool statsDebug = false;
    // actions (command line only)
    bool selftest = false;
    bool list = false;
    bool help = false;
    bool printConfig = false;
    std::string configPath;          // configuration file actually loaded (empty = none)
};

enum class ParseResult { Run, Exit, Error };

// Loads the configuration file (unless --no-config) and then the command line into `o`.
ParseResult LoadOptions(int argc, wchar_t** argv, Options& o);
void PrintUsage();
void PrintConfig(const Options& o);     // effective options in configuration-file syntax

// Quality level names.
const char* FgQualityName(int vfgMode);
const char* SrQualityName(int level);
// Next level in the same VSR family (standard 1-4, high-bitrate 16-19, streaming 21/23, ...), wrapping.
int NextSrQuality(int level);

} // namespace camss
