// Terminal dashboard: model (live window, 60 s history, per-setting segments for the CSV record),
// the FTXUI view and the low-priority UI thread. See dashboard.h.
//
// FTXUI headers come before <windows.h> (pulled in by dashboard.h): wingdi.h defines an RGB()
// macro that would break ftxui::Color::RGB. Colours below use the Color(r, g, b) constructor.
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "dashboard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iterator>
#include <thread>
#include <vector>

namespace camss {
namespace {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

// ------------------------------------------------------------------ constants
constexpr double kWindowSec = 10.0;        // live statistics window
constexpr double kBucketSec = 0.25;        // history resolution (4 buckets per second)
constexpr int kHistBuckets = 240;          // 60 s of history
constexpr double kSettleSec = 2.0;         // ignored at the start of a segment
constexpr double kMinSegSec = 3.0;         // shorter segments are not kept / written
constexpr size_t kMaxLogLines = 300;
constexpr double kFpsChange = 0.10;        // content fps change that starts a new segment
constexpr double kFpsConfirmSec = 1.0;     // ... when it persists this long

// ------------------------------------------------------------------ theme
// Calm palette in the spirit of Claude Code: default foreground for values, grey labels, one warm
// accent. RGB colours are mapped to the nearest 256/16-colour entry by FTXUI when the terminal
// does not do true colour (the mapping happens at construction, hence the lazy static).
struct Theme {
    Color accent, good, warn, bad, muted, line, band;
};
const Theme& Th() {
    static const Theme t{
        Color(217, 119, 87),   // accent (warm orange)
        Color(98, 180, 110),   // good
        Color(214, 163, 56),   // warn (amber, readable on light and dark)
        Color(224, 88, 78),    // bad
        Color(136, 136, 136),  // muted labels
        Color(98, 98, 98),     // borders, axes
        Color(150, 100, 82),   // p99 band (dim accent)
    };
    return t;
}

// ------------------------------------------------------------------ text helpers
int Wid(const std::string& s) { return ftxui::string_width(s); }

std::string Fmt(const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    return b;
}
// Number with `prec` decimals, "-" when unknown (negative / NaN).
std::string Num(double v, int prec = 1) {
    if (!(v >= 0) || !std::isfinite(v)) return "-";
    return Fmt("%.*f", prec, v);
}
// Adaptive precision for small values (0.21 / 2.2 / 12.8 / 123).
std::string NumA(double v) {
    if (!(v >= 0) || !std::isfinite(v)) return "-";
    if (v < 1) return Fmt("%.2f", v);
    if (v < 100) return Fmt("%.1f", v);
    return Fmt("%.0f", v);
}
std::string Clock(double sec) {
    if (!(sec >= 0)) sec = 0;
    const long s = (long)sec;
    if (s >= 3600) return Fmt("%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    return Fmt("%ld:%02ld", s / 60, s % 60);
}
std::string Res(unsigned w, unsigned h) {
    if (!w || !h) return "-";
    if (w * 9 == h * 16) return Fmt("%up", h);
    return Fmt("%ux%u", w, h);
}
std::string Truncate(const std::string& s, int w) {
    if (w <= 0) return {};
    if (Wid(s) <= w) return s;
    std::string out;
    int cur = 0;
    for (const auto& g : ftxui::Utf8ToGlyphs(s)) {
        const int gw = Wid(g);
        if (cur + gw > w - 1) break;
        out += g; cur += gw;
    }
    return out + "…";
}
std::string PadR(const std::string& s, int w) { std::string t = Truncate(s, w); return t + std::string((size_t)std::max(0, w - Wid(t)), ' '); }
std::string PadL(const std::string& s, int w) { std::string t = Truncate(s, w); return std::string((size_t)std::max(0, w - Wid(t)), ' ') + t; }
std::string Repeat(const char* s, int n) { std::string r; for (int i = 0; i < n; i++) r += s; return r; }

std::string IsoLocal(time_t t) {
    tm lt{}; localtime_s(&lt, &t);
    tm g = lt;
    const long off = (long)(_mkgmtime(&g) - t);
    char b[40]; strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &lt);
    const long a = off < 0 ? -off : off;
    return std::string(b) + Fmt("%c%02ld:%02ld", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
}
// ------------------------------------------------------------------ spans: styled text runs
enum : uint8_t { kPlain = 0, kBold = 1, kMuted = 2, kColor = 4 };
struct Span {
    std::string s;
    uint8_t style = kPlain;
    Color c{};
};
using Spans = std::vector<Span>;

Span S(std::string s) { return {std::move(s), kPlain, {}}; }
Span B(std::string s) { return {std::move(s), kBold, {}}; }
Span M(std::string s) { return {std::move(s), kMuted, {}}; }
Span C(std::string s, Color c) { return {std::move(s), kColor, c}; }
Span CB(std::string s, Color c) { return {std::move(s), (uint8_t)(kColor | kBold), c}; }

int Wid(const Spans& v) { int w = 0; for (const auto& s : v) w += Wid(s.s); return w; }
Spans& operator+=(Spans& a, const Spans& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

Spans TruncateSpans(const Spans& in, int w) {
    if (Wid(in) <= w) return in;
    Spans out;
    int cur = 0;
    for (const auto& sp : in) {
        const int sw = Wid(sp.s);
        if (cur + sw <= w - 1) { out.push_back(sp); cur += sw; continue; }
        Span t = sp; t.s.clear();
        for (const auto& g : ftxui::Utf8ToGlyphs(sp.s)) {
            const int gw = Wid(g);
            if (cur + gw > w - 1) break;
            t.s += g; cur += gw;
        }
        t.s += "…";
        out.push_back(t);
        break;
    }
    return out;
}

Element ToEl(const Span& sp) {
    Element e = ftxui::text(sp.s);
    if (sp.style & kMuted) e = e | ftxui::color(Th().muted);
    if (sp.style & kColor) e = e | ftxui::color(sp.c);
    if (sp.style & kBold) e = e | ftxui::bold;
    return e;
}
// One line of exactly width w (truncated with an ellipsis, padded with blanks).
Element Line(const Spans& v, int w) {
    Spans t = TruncateSpans(v, w);
    Elements els;
    for (const auto& sp : t) els.push_back(ToEl(sp));
    const int rest = w - Wid(t);
    if (rest > 0) els.push_back(ftxui::text(std::string((size_t)rest, ' ')));
    return ftxui::hbox(std::move(els));
}
// Picks the first variant that fits in w (else the last one, truncated).
Element Fit(const std::vector<Spans>& variants, int w) {
    for (const auto& v : variants) if (Wid(v) <= w) return Line(v, w);
    return Line(variants.back(), w);
}
// Right-aligned tail on the same line: left part truncated first (at least one blank between).
Element LineLR(const Spans& left, const Spans& right, int w) {
    const int rw = Wid(right);
    if (rw + 2 > w) return Line(left, w);
    Elements els{Line(left, w - rw - 1), ftxui::text(" ")};
    for (const auto& sp : right) els.push_back(ToEl(sp));
    return ftxui::hbox(std::move(els));
}

// Rounded panel with a grey border and the title in the top edge. w x h are the outer size; the
// body gets (w - 4) x (h - 2) and is clipped to it.
Element Panel(const Spans& title, Element body, int w, int h) {
    const Theme& T = Th();
    Spans tt = TruncateSpans(title, std::max(0, w - 6));
    const int tw = Wid(tt);
    Elements top{ftxui::text("╭─ ") | ftxui::color(T.line)};
    for (const auto& sp : tt) top.push_back(ToEl(sp));
    top.push_back(ftxui::text(" " + Repeat("─", std::max(0, w - 5 - tw)) + "╮") | ftxui::color(T.line));
    auto sideBar = [&] {   // a node can only have one parent: build each side separately
        Elements side;
        for (int i = 0; i < h - 2; i++) side.push_back(ftxui::text("│"));
        return ftxui::vbox(std::move(side)) | ftxui::color(T.line);
    };
    Element l = sideBar(), r = sideBar();
    Element mid = ftxui::hbox({
        l, ftxui::text(" "),
        body | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, std::max(0, w - 4)) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, std::max(0, h - 2)) | ftxui::frame,
        ftxui::text(" "), r,
    });
    Element bottom = ftxui::text("╰" + Repeat("─", std::max(0, w - 2)) + "╯") | ftxui::color(T.line);
    return ftxui::vbox({ftxui::hbox(std::move(top)), mid, bottom}) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, w) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, h);
}
Element Rows(Elements rows, int h) {
    h = std::max(h, 0);                       // (int)0 > -1 would pop an empty vector
    while ((int)rows.size() > h) rows.pop_back();
    return ftxui::vbox(std::move(rows));
}
Element Blank(int w, int h) {
    Elements rows;
    for (int i = 0; i < h; i++) rows.push_back(ftxui::text(std::string((size_t)std::max(0, w), ' ')));
    return ftxui::vbox(std::move(rows));
}

// ------------------------------------------------------------------ big digits (3 rows, thin rounded strokes)
const char* const kBigDigits[10][3] = {
    {"╭─╮", "│ │", "╰─╯"}, {" ╷ ", " │ ", " ╵ "}, {"╶─╮", "╭─╯", "╰─╴"}, {"╶─╮", " ─┤", "╶─╯"},
    {"╷ ╷", "╰─┤", "  ╵"}, {"╭─╴", "╰─╮", "╶─╯"}, {"╭─╴", "├─╮", "╰─╯"}, {"╶─╮", "  │", "  ╵"},
    {"╭─╮", "├─┤", "╰─╯"}, {"╭─╮", "╰─┤", "╶─╯"},
};
std::array<std::string, 3> BigText(const std::string& s) {
    std::array<std::string, 3> r;
    for (size_t i = 0; i < s.size(); i++) {
        const char ch = s[i];
        if (ch >= '0' && ch <= '9') {
            for (int k = 0; k < 3; k++) r[k] += kBigDigits[ch - '0'][k];
            if (i + 1 < s.size() && s[i + 1] != '.') for (auto& x : r) x += " ";
        } else if (ch == '.') {
            r[0] += "  "; r[1] += "  "; r[2] += "╸ ";
        } else {   // '-' and anything else
            r[0] += "   "; r[1] += "╶─╴"; r[2] += "   ";
            if (i + 1 < s.size()) for (auto& x : r) x += " ";
        }
    }
    return r;
}

// Eighth blocks (▁▂▃▅▆▇) are missing from the classic console fonts (Consolas, 新宋体): outside
// Windows Terminal / known modern terminals the graphs use half blocks only (▄ █).
bool FineBlocks() {
    static const bool fine = [] {
        if (const char* e = getenv("CAMSS_TUI_BLOCKS")) return strcmp(e, "half") != 0;
        return getenv("WT_SESSION") != nullptr || getenv("TERM_PROGRAM") != nullptr;
    }();
    return fine;
}

// ------------------------------------------------------------------ bar graph node
// Column bars with eighth-block resolution; `lo` drawn in colour a, the band up to `hi` (optional)
// in colour b. Values above `warnAbove` are drawn in colour w; a dotted reference line at `ref`.
// The node fills its box; the newest data is at the right edge.
struct GraphData {
    std::vector<float> lo, hi;   // per bucket, <0 = no data
    float vmin = 0, vmax = 1;
    Color a, b, w;
    float warnAbove = -1, ref = -1;
};
class BarGraph : public ftxui::Node {
public:
    BarGraph(GraphData d, bool fineBlocks) : d_(std::move(d)), fineBlocks_(fineBlocks) {}
    void ComputeRequirement() override {
        requirement_.min_x = 1; requirement_.min_y = 1;
        requirement_.flex_grow_x = 1; requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_x = 1; requirement_.flex_shrink_y = 1;
    }
    void Render(ftxui::Screen& screen) override {
        static const char* const kEighth[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        const int W = box_.x_max - box_.x_min + 1, H = box_.y_max - box_.y_min + 1;
        if (W <= 0 || H <= 0) return;
        // The W columns always span the full 60 s timeline (kHistBuckets buckets, newest at the
        // right edge); column c covers buckets [c*240/W, (c+1)*240/W) of it. The data may cover
        // only the most recent part of the timeline (shortly after start).
        const int n = (int)d_.lo.size();
        const int off = kHistBuckets - n;          // timeline position of d_.lo[0]
        const float span = std::max(1e-6f, d_.vmax - d_.vmin);
        auto toEighths = [&](float v) { return (int)std::lround(std::clamp((v - d_.vmin) / span, 0.f, 1.f) * (float)(H * 8)); };
        const int refE = d_.ref >= 0 ? toEighths(d_.ref) : -1;
        const bool fine = fineBlocks_;
        auto quant = [&](int e) { return fine || e == 0 ? e : (e <= 4 ? 4 : 8); };
        for (int c = 0; c < W; c++) {
            float lo = -1, hi = -1; int cnt = 0; double sum = 0;
            const int b0 = (int)((int64_t)c * kHistBuckets / W);
            const int b1 = std::max(b0 + 1, (int)((int64_t)(c + 1) * kHistBuckets / W));
            for (int b = b0; b < b1; b++) {
                const int i = b - off;
                if (i < 0 || i >= n) continue;
                if (d_.lo[i] >= 0) { sum += d_.lo[i]; cnt++; }
                if (i < (int)d_.hi.size() && d_.hi[i] > hi) hi = d_.hi[i];
            }
            if (cnt) lo = (float)(sum / cnt);
            const int le = lo >= 0 ? std::max(1, toEighths(lo)) : 0;
            const int he = hi >= 0 ? std::max(le, toEighths(hi)) : 0;
            const Color ca = (d_.warnAbove >= 0 && lo > d_.warnAbove) ? d_.w : d_.a;
            for (int r = 0; r < H; r++) {          // r = 0 bottom row
                const int base = r * 8;
                const int lf = quant(std::clamp(le - base, 0, 8)), hf = quant(std::clamp(he - base, 0, 8));
                auto& px = screen.PixelAt(box_.x_min + c, box_.y_max - r);
                if (lf > 0) {
                    px.character = kEighth[lf];
                    px.foreground_color = ca;
                    if (lf < 8 && hf > lf) px.background_color = d_.b;
                } else if (hf > 0) {
                    px.character = kEighth[hf];
                    px.foreground_color = d_.b;
                } else if (refE >= 0 && refE > base && refE <= base + 8) {
                    px.character = "┄";
                    px.foreground_color = Th().line;
                } else {
                    px.character = " ";
                }
            }
        }
    }
private:
    GraphData d_;
    bool fineBlocks_;
};

// ------------------------------------------------------------------ fixed-bin histogram (segment statistics)
class Hist {
public:
    static constexpr double kStep = 0.02;  // ms
    static constexpr int kBins = 12500;    // 0 .. 250 ms, last bin = overflow
    void Add(double v) {
        if (bins_.empty()) bins_.assign(kBins + 1, 0);
        int i = v <= 0 ? 0 : (int)(v / kStep);
        if (i > kBins) i = kBins;
        bins_[i]++; n_++;
        if (v > max_) max_ = v;
    }
    double Q(double q) const {
        if (!n_) return -1;
        // Same rank as RollingStat::QuantileOf (k = round(q * (n - 1)), 1-based target k + 1), so the
        // record table agrees with the status line also for bimodal data (x2 frame generation alternates
        // between "new frame half shown" and "new frame fully shown").
        uint64_t target = (uint64_t)std::floor(q * (double)(n_ - 1) + 0.5) + 1;
        if (target < 1) target = 1;
        uint64_t c = 0;
        for (int i = 0; i <= kBins; i++) {
            c += bins_[i];
            if (c >= target) return i == kBins ? max_ : std::min(max_, (i + 0.5) * kStep);
        }
        return max_;
    }
    uint64_t N() const { return n_; }
    double Max() const { return n_ ? max_ : -1; }
private:
    std::vector<uint32_t> bins_;
    uint64_t n_ = 0;
    double max_ = 0;
};

float QuantileOf(std::vector<float>& v, double q) {
    if (v.empty()) return -1;
    size_t k = (size_t)std::clamp<double>(q * (double)(v.size() - 1) + 0.5, 0.0, (double)(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}

// ------------------------------------------------------------------ model types
struct Settings {
    uint64_t generation = 0;
    std::string mode, interp, fgQuality, sr, srQuality, hdr, capture, metric;
    bool interpOn = false, srOn = false, adaptive = false, vrr = false;
    int multiplier = 1;
    double targetFps = 0, contentFps = 0, signalHz = 0, dispHz = 0;
    unsigned srcW = 0, srcH = 0, outW = 0, outH = 0;
};
Settings SettingsOf(const AppStatus& st) {
    Settings s;
    s.generation = st.generation; s.mode = st.mode; s.interp = st.interp; s.fgQuality = st.fgQuality;
    s.sr = st.sr; s.srQuality = st.srQuality; s.hdr = st.hdr; s.capture = st.captureFormat; s.metric = st.latencyMetric;
    s.interpOn = st.interpOn; s.srOn = st.srOn; s.adaptive = st.adaptive; s.vrr = st.vrrActive;
    s.multiplier = st.multiplier; s.targetFps = st.targetFps; s.contentFps = st.contentFps;
    s.signalHz = st.signalHz; s.dispHz = st.dispHz;
    s.srcW = st.srcW; s.srcH = st.srcH; s.outW = st.outW; s.outH = st.outH;
    return s;
}
bool FgActive(const Settings& s) { return s.interpOn && !s.interp.empty() && s.interp != "none"; }
bool SrActive(const Settings& s) { return s.srOn && !s.sr.empty() && s.sr != "none"; }
std::string Q(const std::string& q) { return (q.empty() || q == "-") ? std::string() : " " + q; }
std::string FgText(const Settings& s) {
    if (s.interp.empty() || s.interp == "none") return "-";
    if (!s.interpOn) return "关";
    std::string r = s.interp + Q(s.fgQuality);
    if (s.targetFps > 0) r += Fmt(" →%.0f", s.targetFps);
    else if (s.multiplier > 1) r += Fmt(" ×%d", s.multiplier);
    return r;
}
std::string SrText(const Settings& s) {
    if (s.sr.empty() || s.sr == "none") return "-";
    if (!s.srOn) return "关";
    return s.sr + Q(s.srQuality);
}
// Output fps the setting is supposed to deliver (for the "best" mark); 0 = unknown.
double TargetFps(const Settings& s) {
    double t;
    if (s.mode.rfind("fixed", 0) == 0) t = s.dispHz;
    else if (FgActive(s)) t = s.targetFps > 0 ? s.targetFps : s.contentFps * std::max(1, s.multiplier);
    else t = s.contentFps;
    if (s.dispHz > 0 && t > s.dispHz) t = s.dispHz;
    return t;
}

struct SegRow {
    Settings s;
    time_t startT = 0, endT = 0;
    double durSec = 0, measSec = 0;
    uint64_t frames = 0, late = 0, hold = 0;
    double outFps = -1, latP50 = -1, latP99 = -1, latMax = -1, contP50 = -1, dP50 = -1, gpuP50 = -1, gpuP99 = -1;
};

struct SegAcc {
    bool active = false;
    Settings s;
    int64_t startQpc = 0, settleUntil = 0;
    time_t startT = 0;
    int64_t prevQpc = 0, activeTicks = 0;
    uint64_t frames = 0, runs = 0, late = 0, hold = 0;
    Hist lat, cont, d, gpu;
    bool paused = false;
    int64_t fpsPendingSince = 0;
};

struct Bucket {
    float p50 = -1, p99 = -1, d = -1, gpu = -1, fps = -1;
};

struct WinStats {
    size_t n = 0;
    double fps = 0;               // over the window
    double fpsNow = 0;            // over the last second
    float latP50 = -1, latP99 = -1, latMax = -1, contP50 = -1, contP99 = -1, dP50 = -1, gpuP50 = -1, gpuP99 = -1;
    uint64_t late = 0, hold = 0;
};

struct LogLine {
    LogLevel lvl;
    double t;
    std::string text;
};

// ------------------------------------------------------------------ CSV
std::string Csv(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string r = "\"";
    for (char c : s) { if (c == '"') r += '"'; r += c; }
    return r + "\"";
}
std::string CsvNum(double v, int prec = 3) { return v >= 0 && std::isfinite(v) ? Fmt("%.*f", prec, v) : std::string(); }

const char* kCsvHeader =
    "start,end,duration_s,measured_s,frames,out_fps,target_fps,lat_p50_ms,lat_p99_ms,lat_max_ms,content_p50_ms,"
    "d_p50_ms,gpu_p50_ms,gpu_p99_ms,late,hold,mode,vrr,interp,fg_quality,interp_on,multiplier,fg_target_fps,"
    "adaptive,sr,sr_quality,sr_on,hdr,content_fps,signal_hz,display_hz,capture,src,out,latency_metric,generation\n";

bool AppendCsv(const std::string& path, const SegRow& r) {
    if (path.empty()) return true;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(Utf8ToWide(path.c_str()));
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const bool isNew = !fs::exists(p, ec) || fs::file_size(p, ec) == 0;
    FILE* f = _wfopen(p.c_str(), L"ab");
    if (!f) return false;
    std::string line;
    // UTF-8 BOM on a new file: Excel (zh-CN) otherwise decodes device names / "→" as GBK
    if (isNew) line += "\xEF\xBB\xBF" + std::string(kCsvHeader);
    const Settings& s = r.s;
    line += IsoLocal(r.startT) + "," + IsoLocal(r.endT) + "," + CsvNum(r.durSec, 1) + "," + CsvNum(r.measSec, 1) + "," +
            std::to_string(r.frames) + "," + CsvNum(r.outFps, 2) + "," + CsvNum(TargetFps(s), 2) + "," +
            CsvNum(r.latP50) + "," + CsvNum(r.latP99) + "," + CsvNum(r.latMax) + "," + CsvNum(r.contP50) + "," +
            CsvNum(r.dP50) + "," + CsvNum(r.gpuP50) + "," + CsvNum(r.gpuP99) + "," +
            std::to_string(r.late) + "," + std::to_string(r.hold) + "," +
            Csv(s.mode) + "," + (s.vrr ? "1" : "0") + "," + Csv(s.interp) + "," + Csv(s.fgQuality) + "," +
            (s.interpOn ? "1" : "0") + "," + std::to_string(s.multiplier) + "," + CsvNum(s.targetFps, 2) + "," +
            (s.adaptive ? "1" : "0") + "," + Csv(s.sr) + "," + Csv(s.srQuality) + "," + (s.srOn ? "1" : "0") + "," +
            Csv(s.hdr) + "," + CsvNum(s.contentFps, 3) + "," + CsvNum(s.signalHz, 3) + "," + CsvNum(s.dispHz, 3) + "," +
            Csv(s.capture) + "," + Fmt("%ux%u", s.srcW, s.srcH) + "," + Fmt("%ux%u", s.outW, s.outH) + "," +
            Csv(s.metric) + "," + std::to_string(s.generation) + "\n";
    const bool ok = fwrite(line.data(), 1, line.size(), f) == line.size();
    fclose(f);
    return ok;
}

// ------------------------------------------------------------------ the model
class Model {
public:
    void SetOptions(const DashboardOptions& o) { opt_ = o; }
    void SetFineBlocks(bool fine) { fineBlocks_ = fine; }

    // UI thread: drain telemetry, update statistics.
    // boostStatusRead: Status() copies the struct under the mutex that the render thread
    // (THREAD_PRIORITY_HIGHEST) takes in PublishStatus. A below-normal thread preempted while
    // holding it would make the render thread wait for a scheduler slot (priority inversion;
    // SRW locks do not boost the owner), so the UI thread runs at HIGHEST for that copy.
    void Update(Telemetry* tel, int64_t now, bool boostStatusRead = false) {
        const int64_t t0 = Qpc::Now();
        uint64_t seq = 0;
        const HANDLE self = GetCurrentThread();
        const int prio = boostStatusRead ? GetThreadPriority(self) : 0;
        const bool boost = boostStatusRead && prio != THREAD_PRIORITY_ERROR_RETURN && prio < THREAD_PRIORITY_HIGHEST;
        if (boost) SetThreadPriority(self, THREAD_PRIORITY_HIGHEST);
        AppStatus st = tel->Status(&seq);
        if (boost) SetThreadPriority(self, prio);
        if (seq) { st_ = std::move(st); haveStatus_ = true; SegmentCheck(now); }
        FrameSample f;
        while (tel->PopFrame(f)) {
            if (!f.qpc) f.qpc = now;
            if (!firstQpc_) firstQpc_ = f.qpc;
            lastQpc_ = std::max(lastQpc_, f.qpc);
            if (f.qpc >= winStart_) win_.push_back(f);   // not frames queued before a window reset
            AddBucket(f);
            AddSeg(f);
            framesSeenLocal_++;
        }
        const int64_t wt = Qpc::FromMs(kWindowSec * 1000);
        while (!win_.empty() && win_.front().qpc < now - wt) win_.pop_front();
        while (win_.size() > 20000) win_.pop_front();
        AdvanceBuckets(now);
        ComputeWindow(now);
        ringDropped_ = tel->DroppedFrames();
        now_ = now;
        const double ms = Qpc::ToMs(Qpc::Now() - t0);
        refreshes_.fetch_add(1, std::memory_order_relaxed);
        framesSeen_.store(framesSeenLocal_, std::memory_order_relaxed);
        updMsSum_.store(updMsSum_.load(std::memory_order_relaxed) + ms, std::memory_order_relaxed);
        if (ms > updMsMax_.load(std::memory_order_relaxed)) updMsMax_.store(ms, std::memory_order_relaxed);
    }
    // Ends the current segment (Stop / test).
    void Finish(int64_t now) {
        if (seg_.active) EndSeg(now);
    }
    void ResetWindow() { win_.clear(); winStart_ = now_; }

    // ---- view (UI thread)
    Element Build(int W, int H, const std::vector<LogLine>& logs, bool help, bool quitting) const;

    // perf counters (read by Dashboard::Perf from any thread)
    std::atomic<uint64_t> refreshes_{0}, framesSeen_{0};
    std::atomic<double> updMsSum_{0}, updMsMax_{0};

private:
    // ---------- segments
    void BeginSeg(const AppStatus& st, int64_t at) {
        seg_ = SegAcc{};
        seg_.active = true;
        seg_.s = SettingsOf(st);
        seg_.startQpc = at;
        seg_.settleUntil = at + Qpc::FromMs(kSettleSec * 1000);
        seg_.startT = WallAt(at);
    }
    void EndSeg(int64_t at) {
        const SegRow r = RowOf(seg_, at);
        seg_.active = false;
        if (r.durSec >= kMinSegSec && r.frames > 0) AppendCsv(opt_.historyPath, r);
    }
    void SegmentCheck(int64_t now) {
        const AppStatus& st = st_;
        if (!st.warmupDone) { if (seg_.active) EndSeg(now); return; }
        if (!seg_.active) { BeginSeg(st, now); return; }
        if (st.generation != seg_.s.generation) {
            // new settings: the live numbers restart so they describe the new setting only
            EndSeg(now); BeginSeg(st, now);
            win_.clear(); winStart_ = now;
            return;
        }
        // content frame rate: static / missing content pauses the statistics, a lasting change
        // of more than 10 % starts a new segment (back-dated to when it was first seen)
        const double cur = st.contentFps, ref = seg_.s.contentFps;
        if (st.contentStatic || cur < 1) { seg_.paused = true; seg_.fpsPendingSince = 0; return; }
        if (ref < 1) { seg_.s.contentFps = cur; seg_.paused = false; return; }
        if (std::fabs(cur - ref) > kFpsChange * ref) {
            if (!seg_.fpsPendingSince) seg_.fpsPendingSince = now;
            seg_.paused = true;
            if (now - seg_.fpsPendingSince >= Qpc::FromMs(kFpsConfirmSec * 1000)) {
                const int64_t at = seg_.fpsPendingSince;
                EndSeg(at);
                BeginSeg(st, at);
            }
        } else {
            seg_.fpsPendingSince = 0;
            seg_.paused = false;
        }
    }
    void AddSeg(const FrameSample& f) {
        SegAcc& s = seg_;
        if (!s.active || s.paused || f.qpc < s.settleUntil) { s.prevQpc = 0; return; }
        if (s.prevQpc && f.qpc > s.prevQpc && f.qpc - s.prevQpc < Qpc::FromMs(500)) s.activeTicks += f.qpc - s.prevQpc;
        else s.runs++;
        s.prevQpc = f.qpc;
        s.frames++;
        const bool hold = (f.flags & FrameSample::kHold) != 0;
        if (f.flags & FrameSample::kLate) s.late++;
        if (hold) s.hold++;
        if (!hold && f.latencyMs >= 0) s.lat.Add(f.latencyMs);
        if (!hold && f.contentMs >= 0) s.cont.Add(f.contentMs);
        s.d.Add(f.delayMs);
        if (f.gpuMs > 0) s.gpu.Add(f.gpuMs);
    }
    SegRow RowOf(const SegAcc& s, int64_t at) const {
        SegRow r;
        r.s = s.s;
        r.startT = s.startT; r.endT = WallAt(at);
        r.durSec = std::max(0.0, Qpc::ToSec(at - s.startQpc));
        r.measSec = Qpc::ToSec(s.activeTicks);
        r.frames = s.frames; r.late = s.late; r.hold = s.hold;
        if (r.measSec > 0 && s.frames > s.runs) r.outFps = (double)(s.frames - s.runs) / r.measSec;
        r.latP50 = s.lat.Q(0.5); r.latP99 = s.lat.Q(0.99); r.latMax = s.lat.Max();
        r.contP50 = s.cont.Q(0.5); r.dP50 = s.d.Q(0.5);
        r.gpuP50 = s.gpu.Q(0.5); r.gpuP99 = s.gpu.Q(0.99);
        return r;
    }
    // Wall clock of a QPC time (QPC times may be simulated: relative to the real "now").
    time_t WallAt(int64_t qpc) const {
        const int64_t realNow = Qpc::Now();
        return time(nullptr) - (time_t)std::llround(Qpc::ToSec(realNow - qpc));
    }

    // ---------- history buckets
    void AddBucket(const FrameSample& f) {
        const int64_t bt = Qpc::FromMs(kBucketSec * 1000);
        const int64_t idx = f.qpc / bt;
        if (curIdx_ < 0) curIdx_ = idx;
        if (idx > curIdx_) {
            FinishBucket();
            PushEmpty(idx - curIdx_ - 1);
            curIdx_ = idx;
        }
        bN_++;
        if (!(f.flags & FrameSample::kHold) && f.latencyMs >= 0) bLat_.push_back(f.latencyMs);
        bD_.push_back(f.delayMs);
        if (f.gpuMs > 0) bGpu_.push_back(f.gpuMs);
    }
    void AdvanceBuckets(int64_t now) {
        const int64_t bt = Qpc::FromMs(kBucketSec * 1000);
        const int64_t nowIdx = now / bt;
        if (curIdx_ < 0) { curIdx_ = nowIdx; return; }
        if (nowIdx > curIdx_ + 1) {            // current bucket is complete (> 250 ms ago)
            FinishBucket();
            PushEmpty(nowIdx - curIdx_ - 2);
            curIdx_ = nowIdx - 1;
        }
    }
    void FinishBucket() {
        Bucket b;
        if (bN_ > 0) {
            b.fps = (float)(bN_ / kBucketSec);
            b.p50 = QuantileOf(bLat_, 0.5);
            b.p99 = QuantileOf(bLat_, 0.99);
            if (!bD_.empty()) { double s = 0; for (float v : bD_) s += v; b.d = (float)(s / bD_.size()); }
            b.gpu = QuantileOf(bGpu_, 0.5);
        }
        hist_.push_back(b);
        while (hist_.size() > (size_t)kHistBuckets) hist_.pop_front();
        bN_ = 0; bLat_.clear(); bD_.clear(); bGpu_.clear();
    }
    void PushEmpty(int64_t n) {
        n = std::min<int64_t>(n, kHistBuckets);
        for (int64_t i = 0; i < n; i++) hist_.push_back(Bucket{});
        while (hist_.size() > (size_t)kHistBuckets) hist_.pop_front();
    }

    // ---------- live window
    void ComputeWindow(int64_t now) {
        WinStats w;
        sLat_.clear(); sCont_.clear(); sD_.clear(); sGpu_.clear();
        for (const auto& f : win_) {
            const bool hold = (f.flags & FrameSample::kHold) != 0;
            if (f.flags & FrameSample::kLate) w.late++;
            if (hold) w.hold++;
            if (!hold && f.latencyMs >= 0) sLat_.push_back(f.latencyMs);
            if (!hold && f.contentMs >= 0) sCont_.push_back(f.contentMs);
            sD_.push_back(f.delayMs);
            if (f.gpuMs > 0) sGpu_.push_back(f.gpuMs);
        }
        w.n = win_.size();
        if (!sLat_.empty()) w.latMax = *std::max_element(sLat_.begin(), sLat_.end());
        w.latP50 = QuantileOf(sLat_, 0.5); w.latP99 = QuantileOf(sLat_, 0.99);
        w.contP50 = QuantileOf(sCont_, 0.5); w.contP99 = QuantileOf(sCont_, 0.99); w.dP50 = QuantileOf(sD_, 0.5);
        w.gpuP50 = QuantileOf(sGpu_, 0.5); w.gpuP99 = QuantileOf(sGpu_, 0.99);
        if (firstQpc_) {
            const double span = std::min(kWindowSec, Qpc::ToSec(now - std::max(firstQpc_, winStart_)));
            if (span > 0.2) w.fps = (double)w.n / span;
            const int64_t oneSec = now - Qpc::Freq();
            size_t n1 = 0;
            for (auto it = win_.rbegin(); it != win_.rend() && it->qpc >= oneSec; ++it) n1++;
            const double span1 = std::min(1.0, Qpc::ToSec(now - std::max(firstQpc_, winStart_)));
            if (span1 > 0.2) w.fpsNow = (double)n1 / span1;
        }
        ws_ = w;
    }

    // ---------- view helpers (view.cpp-like section below)
    struct Health { Color c; std::string label; int level; };
    Health HealthOf() const;
    Element Header(int W) const;
    Elements LatencyRows(int iw, bool compact) const;
    Elements SettingsRows(int iw, bool compact) const;
    Element HistoryPanel(int w, int h) const;
    Element LogPanel(int w, int h, const std::vector<LogLine>& logs) const;
    Element Footer(int W, bool quitting) const;
    Element Compact(int W, int h) const;
    Element Help(int W, int H) const;
    Color LatColor(double v) const;
    double ContentPeriodMs() const { return st_.contentFps > 1 ? 1000.0 / st_.contentFps : 1000.0 / 60.0; }

    DashboardOptions opt_;
    bool fineBlocks_ = true;
    AppStatus st_;
    bool haveStatus_ = false;
    int64_t now_ = 0, firstQpc_ = 0, lastQpc_ = 0;
    uint64_t ringDropped_ = 0, framesSeenLocal_ = 0;

    std::deque<FrameSample> win_;
    WinStats ws_;
    int64_t winStart_ = 0;
    std::vector<float> sLat_, sCont_, sD_, sGpu_;

    std::deque<Bucket> hist_;
    int64_t curIdx_ = -1;
    int bN_ = 0;
    std::vector<float> bLat_, bD_, bGpu_;

    SegAcc seg_;
};

// ------------------------------------------------------------------ view
Model::Health Model::HealthOf() const {
    const Theme& T = Th();
    if (!haveStatus_) return {T.warn, "等待状态", 1};
    const double age = lastQpc_ ? Qpc::ToSec(now_ - lastQpc_) : 1e9;
    if (st_.warmupDone && age > 1.0) return {T.bad, "无画面", 2};
    if (st_.gpuBudgetMs > 0 && ws_.gpuP50 > st_.gpuBudgetMs) return {T.bad, "GPU 超预算", 2};
    if (!st_.warmupDone) return {T.warn, "预热中", 1};
    if (st_.mode.find("prob") != std::string::npos) return {T.warn, "探测中", 1};
    if (ws_.n > 100 && ws_.late * 100 > ws_.n) return {T.warn, "迟到偏多", 1};
    return {T.good, "运行中", 0};
}

Color Model::LatColor(double v) const {
    const Theme& T = Th();
    if (v < 0) return T.muted;
    const double p = ContentPeriodMs();
    if (v <= p + 4) return T.good;
    if (v <= 2 * p + 4) return T.warn;
    return T.bad;
}
Element Model::Header(int W) const {
    const Theme& T = Th();
    const Health h = HealthOf();
    Spans brand{C(" ✻ ", T.accent), CB("camss", T.accent), S("  "), C("●", h.c), S(" " + h.label)};
    const double out = ws_.fpsNow > 0 ? ws_.fpsNow : st_.outFps;
    Spans rates;
    if (st_.signalHz > 0) { rates.push_back(M("输入 ")); rates.push_back(S(Fmt("%.2f Hz", st_.signalHz))); rates.push_back(M(" → ")); }
    rates.push_back(M("内容 ")); rates.push_back(S(st_.contentStatic ? "静止" : Num(st_.contentFps, 1) + " fps"));
    rates.push_back(M(" → 输出 ")); rates.push_back(S(Num(out, 1) + " fps"));
    const Settings s = SettingsOf(st_);
    if (FgActive(s)) {
        if (s.targetFps > 0) rates.push_back(C(Fmt(" →%.0f", s.targetFps), T.accent));
        else if (s.multiplier > 1) rates.push_back(C(Fmt(" ×%d", s.multiplier), T.accent));
    }
    Spans res;
    if (st_.srcH) {
        res.push_back(S(Res(st_.srcW, st_.srcH)));
        if (st_.outH && st_.outH != st_.srcH) { res.push_back(M(" → ")); res.push_back(S(Res(st_.outW, st_.outH))); }
        if (!st_.hdr.empty() && st_.hdr != "none") res.push_back(C(" HDR", T.accent));
    }
    const Spans right{M("up "), S(Clock(st_.uptimeSec)), S(" ")};

    auto join = [&](std::initializer_list<const Spans*> parts) {
        Spans r = brand;
        for (const Spans* p : parts) if (!p->empty()) { r.push_back(M("   ")); r += *p; }
        return r;
    };
    Spans shortRates{M("内容 "), S(st_.contentStatic ? "静止" : Num(st_.contentFps, 1)), M(" → 输出 "), S(Num(out, 1) + " fps")};
    if (rates.size() && rates.back().style & kColor) shortRates.push_back(rates.back());
    std::vector<Spans> v{join({&rates, &res}), join({&rates}), join({&shortRates}), brand};
    for (auto& x : v) if (Wid(x) + Wid(right) + 2 <= W) return LineLR(x, right, W);
    return Line(brand, W);
}

// Latency panel. The big number is capture card -> screen (content latency, p50); the bar under it
// shows what it is made of, the second bar the GPU time per output frame against its budget.
// Counters appear only when something went wrong.
namespace {
struct BarSeg { std::string name; double v; Color c; };

// One proportional bar (`scale` ms = bw columns) and, under it, each segment's name and value
// starting at the segment (skipped when it would collide with the previous label).
void SegBar(const std::vector<BarSeg>& segs, double scale, int bw, const Spans& lead, const Spans& tail, const Theme& T,
            Elements& rows, int iw) {
    Spans bar = lead, lab{S(std::string((size_t)Wid(lead), ' '))};
    int col = 0, labEnd = 0;
    double acc = 0;
    for (const auto& g : segs) {
        if (g.v <= 0) continue;
        const int x0 = (int)std::lround(acc / scale * bw);
        acc += g.v;
        const int x1 = std::min(bw, (int)std::lround(acc / scale * bw));
        if (x1 > x0) bar.push_back(C(Repeat("█", x1 - x0), g.c));
        col = std::max(col, x1);
        const std::string t = g.name + " " + NumA(g.v);
        // under the segment; a short segment near the end gets its label right-aligned to the bar
        int at = x0;
        if (at + Wid(t) + 1 > bw + Wid(tail)) at = bw + Wid(tail) - Wid(t) - 1;
        if (x1 > x0 && at >= labEnd) {
            if (at > labEnd) lab.push_back(S(std::string((size_t)(at - labEnd), ' ')));
            const int x0 = at;
            lab.push_back(C(g.name + " ", T.muted));
            lab.push_back(S(NumA(g.v)));
            labEnd = x0 + Wid(t) + 1;
            lab.push_back(S(" "));
        }
    }
    if (col < bw) bar.push_back(C(Repeat("░", bw - col), T.line));
    bar += tail;
    rows.push_back(Line(bar, iw));
    rows.push_back(Line(lab, iw));
}
} // namespace

Elements Model::LatencyRows(int iw, bool compact) const {
    const Theme& T = Th();
    Elements rows;
    const double cap = (st_.captureMs > 0 ? st_.captureMs : 0) + (st_.uploadMs > 0 ? st_.uploadMs : 0);
    const double total = ws_.contP50 >= 0 ? ws_.contP50 : ws_.latP50;
    const double p99 = ws_.contP50 >= 0 ? ws_.contP99 : ws_.latP99;
    const Color ct = total < 0 ? T.muted : LatColor(total - cap);
    {   // big p50 + small p99
        auto big = BigText(NumA(total));
        for (int k = 0; k < 3; k++) {
            Spans l{CB(big[k], ct)};
            if (k == 2) l.push_back(M(" ms"));
            Spans r;
            if (k == 1 && p99 >= 0) { r.push_back(M("p99 ")); r.push_back(CB(NumA(p99), LatColor(p99 - cap))); }
            rows.push_back(LineLR(l, r, iw));
        }
    }
    if (!compact) rows.push_back(Line({}, iw));
    if (total > 0) {   // composition: capture card | waiting for the next frame | GPU | margin
        const double lead = std::max(0.0, st_.leadMs);
        const double gpu = ws_.gpuP50 > 0 ? std::min((double)ws_.gpuP50, lead) : 0;
        const double wait = std::max(0.0, total - cap - lead);
        std::vector<BarSeg> segs{{"采集", cap, T.muted}, {"等待", wait, T.band}};
        if (gpu > 0) { segs.push_back({"GPU", gpu, T.good}); segs.push_back({"余量", std::max(0.0, lead - gpu), T.line}); }
        else segs.push_back({"渲染", lead, T.good});
        double sum = 0;
        for (auto& g : segs) sum += g.v;
        SegBar(segs, std::max(sum, total), iw, {}, {}, T, rows, iw);
    }
    if (!compact) rows.push_back(Line({}, iw));
    if (ws_.gpuP50 > 0) {   // GPU per output frame, split by stage, against the frame budget
        const double g50 = ws_.gpuP50, budget = st_.gpuBudgetMs;
        const bool over = budget > 0 && g50 > budget;
        std::vector<BarSeg> segs;
        double known = 0;
        auto add = [&](const char* n, double v, Color c) { if (v > 0) { segs.push_back({n, v, c}); known += v; } };
        add("补帧", st_.fgMs, T.accent);
        add("超分", st_.srMs, T.good);
        add("HDR", st_.hdrMs, T.warn);
        if (g50 - known > 0.05) segs.push_back({"其他", g50 - known, T.muted});
        Spans lead{M("GPU ")};
        Spans tail{S(" "), over ? CB(NumA(g50), T.bad) : S(NumA(g50))};
        if (budget > 0) { tail.push_back(M(" / ")); tail.push_back(S(NumA(budget))); }
        tail.push_back(M(" ms"));
        const int bw = std::max(4, iw - Wid(lead) - Wid(tail));
        SegBar(segs, budget > 0 ? std::max(budget, g50) : g50, bw, lead, tail, T, rows, iw);
    }
    {   // only what went wrong
        Spans r;
        auto add = [&](const char* n, uint64_t v) {
            if (!v) return;
            if (!r.empty()) r.push_back(M(" · "));
            r.push_back(M(std::string(n) + " "));
            r.push_back(C(std::to_string(v), T.warn));
        };
        add("迟到", st_.late); add("丢帧", st_.drops); add("错过", st_.missed);
        if (!r.empty()) rows.push_back(Line(r, iw));
    }
    return rows;
}

// Settings panel. The switches come first, one per row in a fixed grid: key, name, state, what
// is switched and, on the right, the key that tunes it. Below a rule: the read-only facts.
// compact = no rule (short terminals).
Elements Model::SettingsRows(int iw, bool compact) const {
    const Theme& T = Th();
    const Settings s = SettingsOf(st_);
    Elements rows;
    enum { kNa = -1, kOff = 0, kOn = 1 };
    auto toggle = [&](const char* key, const char* name, int state, Spans what, const char* key2 = nullptr,
                      const char* what2 = nullptr) {
        Spans l{CB(PadR(key, 3), T.accent), S(PadR(name, 7))};
        if (state == kOn) l.push_back(C("● 开   ", T.good));
        else if (state == kOff) l.push_back(M("○ 关   "));
        else l.push_back(M("-      "));
        const int fixed = Wid(l);
        if (state != kOn) for (auto& sp : what) if (!(sp.style & kColor)) sp.style = kMuted;
        l += what;
        Spans r;
        if (key2) r = {CB(key2, T.accent), M(std::string(" ") + what2)};
        // the tuning key stays visible; the description is shortened first
        if (!r.empty() && fixed + 8 + 2 + Wid(r) <= iw) rows.push_back(LineLR(l, r, iw));
        else rows.push_back(Line(l, iw));
    };
    {
        const bool avail = !s.interp.empty() && s.interp != "none";
        Spans w;
        if (!avail) w.push_back(S("未配置"));
        else {
            w.push_back(S(s.interp + Q(s.fgQuality)));
            if (s.targetFps > 0) w.push_back(S(Fmt(" · 目标 %.0f fps", s.targetFps)));
            else w.push_back(S(Fmt(" · ×%d", std::max(1, s.multiplier))));
            if (s.adaptive) w.push_back(S(" · 自适应"));
        }
        toggle("i", "补帧", avail ? (s.interpOn ? kOn : kOff) : kNa, w, avail && !Q(s.fgQuality).empty() ? "f" : nullptr, "档位");
    }
    {
        const bool avail = !s.sr.empty() && s.sr != "none";
        toggle("s", "超分", avail ? (s.srOn ? kOn : kOff) : kNa, {S(avail ? s.sr + Q(s.srQuality) : "未配置")},
               avail && !Q(s.srQuality).empty() ? "q" : nullptr, "档位");
    }
    if (!st_.audioEnabled) toggle("m", "声音", kNa, {S("未启用")});
    else if (!st_.audioRunning) toggle("m", "声音", st_.audioMuted ? kOff : kOn, {C("设备未就绪", T.warn)}, "+/-", "音量");
    else toggle("m", "声音", st_.audioMuted ? kOff : kOn, {S(Fmt("音量 %.0f%%", st_.audioVolume * 100))}, "+/-", "音量");
    toggle("t", "时间码", st_.timecode ? kOn : kOff, {S("回环测延迟用")});

    if (!compact) rows.push_back(Line({C(Repeat("─", iw), T.line)}, iw));
    auto info = [&](const char* label, Spans v) {
        Spans l{S("   "), M(PadR(label, 7))};
        l += v;
        rows.push_back(Line(l, iw));
    };
    {
        Spans v{S(st_.mode.empty() ? "-" : st_.mode)};
        if (st_.vrrActive) { v.push_back(M(" · ")); v.push_back(C("VRR 生效", T.good)); }
        if (st_.dispW) { v.push_back(M(" · ")); v.push_back(S(Fmt("%ux%u @ %.0f Hz", st_.dispW, st_.dispH, st_.dispHz))); }
        if (!st_.hdr.empty() && st_.hdr != "none") { v.push_back(M(" · HDR ")); v.push_back(S(st_.hdr)); }
        info("输出", v);
    }
    {
        Spans v{S(st_.captureName.empty() ? "-" : st_.captureName)};
        if (!st_.captureFormat.empty()) { v.push_back(M(" · ")); v.push_back(S(st_.captureFormat)); }
        info("采集", v);
    }
    if (st_.audioEnabled) {
        Spans v{S(st_.audioIn.empty() ? "-" : st_.audioIn), M(" → "), S(st_.audioOut.empty() ? "-" : st_.audioOut)};
        v.push_back(M(" · 延迟 ")); v.push_back(S(Num(st_.audioLatencyMs, 0))); v.push_back(M(" ms"));
        v.push_back(M(" · 同步 ")); v.push_back(S(Fmt("%+.0f", st_.audioSyncMs)));
        if (st_.audioUnderruns) { v.push_back(M(" · 欠载 ")); v.push_back(C(std::to_string(st_.audioUnderruns), T.warn)); }
        info("音频", v);
    }
    info("配置", {S(st_.configPath.empty() ? "-" : st_.configPath)});
    return rows;
}

Element Model::HistoryPanel(int w, int h) const {
    const Theme& T = Th();
    const int iw = w - 4, ih = h - 2;
    std::vector<float> p50, p99, gpu, d;
    for (const auto& b : hist_) { p50.push_back(b.p50); p99.push_back(b.p99); gpu.push_back(b.gpu); d.push_back(b.d); }
    auto range = [](const std::vector<float>& a, const std::vector<float>& b, float& mn, float& mx) {
        mn = 1e9f; mx = -1;
        for (float v : a) if (v >= 0) { mn = std::min(mn, v); mx = std::max(mx, v); }
        for (float v : b) if (v >= 0) { mn = std::min(mn, v); mx = std::max(mx, v); }
        return mx >= 0;
    };
    const bool haveGpu = std::any_of(gpu.begin(), gpu.end(), [](float v) { return v > 0; });
    const int gutter = 6;
    const int gw = std::max(1, iw - gutter);
    // rows: [title, graph x gh] x 2 + axis
    const bool two = ih >= 5;
    const int axis = ih >= 7 ? 1 : 0;
    const int ghAll = std::max(1, two ? ih - 2 - axis : ih - 1 - axis);
    const int gh1 = two ? std::max(1, (ghAll + 1) / 2) : ghAll, gh2 = std::max(1, ghAll - gh1);   // latency gets the odd row
    Elements rows;
    auto graph = [&](GraphData g, const std::string& top, const std::string& bottom, int rowsN) {
        Elements lab;
        for (int i = 0; i < rowsN; i++) {
            std::string t = i == 0 ? top : (i == rowsN - 1 ? bottom : "");
            lab.push_back(ftxui::text(PadL(t, gutter - 1) + " ") | ftxui::color(T.muted));
        }
        Element ge = std::make_shared<BarGraph>(std::move(g), fineBlocks_) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, gw) |
                     ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowsN);
        return ftxui::hbox({ftxui::vbox(std::move(lab)), ge});
    };
    {   // latency p50 + p99 band
        float mn, mx;
        const bool ok = range(p50, p99, mn, mx);
        float lo = 0, hi = 1;
        if (ok) { const float pad = std::max(0.5f, (mx - mn) * 0.15f); lo = std::max(0.f, mn - pad); hi = mx + pad * 0.3f; }
        Spans t{B("延迟"), M(" ms  "), C("█", T.accent), M(" p50  "), C("█", T.band), M(" p99")};
        Spans rt;
        if (ok) rt = {M("min "), S(NumA(mn)), M("  max "), S(NumA(mx))};
        rows.push_back(LineLR(t, rt, iw));
        GraphData g{p50, p99, lo, hi, T.accent, T.band, T.bad, -1, -1};
        rows.push_back(graph(std::move(g), ok ? NumA(hi) : "", ok ? (lo > 0 ? NumA(lo) : "0") : "", gh1));
    }
    if (two) {
        if (haveGpu) {
            float mn, mx;
            range(gpu, {}, mn, mx);
            const float budget = (float)st_.gpuBudgetMs;
            const float hi = std::max(budget > 0 ? budget * 1.15f : 0.f, mx * 1.1f);
            Spans t{B("GPU"), M(" ms  "), C("█", T.good), M(" p50")};
            if (budget > 0) { t.push_back(M("  ┄ 预算 ")); t.push_back(S(NumA(budget))); }
            Spans rt{M("min "), S(NumA(mn)), M("  max "), S(NumA(mx))};
            rows.push_back(LineLR(t, rt, iw));
            GraphData g{gpu, {}, 0, hi, T.good, T.band, T.bad, budget > 0 ? budget : -1, budget > 0 ? budget : -1};
            rows.push_back(graph(std::move(g), NumA(hi), "0", gh2));
        } else {
            float mn, mx;
            const bool ok = range(d, {}, mn, mx);
            const float hi = ok ? mx * 1.15f + 0.1f : 1;
            Spans t{B("D"), M(" ms")};
            Spans rt;
            if (ok) rt = {M("min "), S(NumA(mn)), M("  max "), S(NumA(mx))};
            rows.push_back(LineLR(t, rt, iw));
            GraphData g{d, {}, 0, hi, T.good, T.band, T.bad, -1, -1};
            rows.push_back(graph(std::move(g), ok ? NumA(hi) : "", "0", gh2));
        }
    }
    if (axis) {
        std::string ax(gutter, ' ');
        const int seconds = (int)std::lround(kHistBuckets * kBucketSec);   // the graphs always span the full history
        std::string l = "-" + std::to_string(seconds) + "s", mid = "-" + std::to_string(seconds / 2) + "s", r = "现在";
        std::string line = l;
        const int midPos = gw / 2 - Wid(mid) / 2;
        line += std::string((size_t)std::max(1, midPos - Wid(line)), ' ') + mid;
        line += std::string((size_t)std::max(1, gw - Wid(line) - Wid(r)), ' ') + r;
        rows.push_back(Line({M(ax + line)}, iw));
    }
    return Panel({B("历史")}, Rows(std::move(rows), ih), w, h);
}

Element Model::LogPanel(int w, int h, const std::vector<LogLine>& logs) const {
    const Theme& T = Th();
    const int iw = w - 4, ih = h - 2;
    Elements rows;
    const int first = std::max(0, (int)logs.size() - ih);
    for (int i = first; i < (int)logs.size(); i++) {
        const LogLine& l = logs[i];
        Spans r{M(PadL(Clock(l.t), 5) + Fmt(".%d ", (int)(std::fmod(std::max(0.0, l.t), 1.0) * 10)))};
        if (l.lvl == LogLevel::Error) r.push_back(C(l.text, T.bad));
        else if (l.lvl == LogLevel::Warn) r.push_back(C(l.text, T.warn));
        else if (l.lvl == LogLevel::Debug) r.push_back(M(l.text));
        else r.push_back(S(l.text));
        rows.push_back(Line(r, iw));
    }
    return Panel({B("日志")}, Rows(std::move(rows), ih), w, h);
}

Element Model::Footer(int W, bool quitting) const {
    const Theme& T = Th();
    static const char* const kHints[][2] = {{"esc", "退出"}, {"l", "打印统计"}, {"r", "重置统计"}, {"?", "全部按键"}};
    Spans r{S(" ")};
    if (quitting) { r.push_back(C("正在退出…", T.accent)); return Line(r, W); }
    for (size_t i = 0; i < std::size(kHints); i++) {
        Spans item;
        if (i) item.push_back(M(" · "));
        item.push_back(CB(kHints[i][0], T.accent));
        item.push_back(M(std::string(" ") + kHints[i][1]));
        if (Wid(r) + Wid(item) > W) break;
        r += item;
    }
    return Line(r, W);
}

Element Model::Compact(int W, int h) const {
    Elements rows;
    const Color c50 = LatColor(ws_.latP50);
    const bool cont = ws_.contP50 >= 0;
    rows.push_back(Line({S(" "), M("延迟 "), CB(NumA(cont ? ws_.contP50 : ws_.latP50), c50), M("  p99 "),
                         S(NumA(cont ? ws_.contP99 : ws_.latP99)), M(" ms")}, W));
    rows.push_back(Line({S(" "), M("GPU "), S(NumA(ws_.gpuP50)), M(" / "), S(NumA(st_.gpuBudgetMs)), M(" ms")}, W));
    rows.push_back(Line({S(" "), M("补帧 "), S(FgText(SettingsOf(st_))), M("  超分 "), S(SrText(SettingsOf(st_)))}, W));
    rows.push_back(Line({S(" "), M("迟到 "), S(std::to_string(ws_.late)), M("  保持 "), S(std::to_string(ws_.hold)), M("  "), S(Num(ws_.fps, 1)), M(" fps")}, W));
    while ((int)rows.size() < h) rows.push_back(Line({}, W));
    return Rows(std::move(rows), h);
}

Element Model::Help(int W, int H) const {
    const Theme& T = Th();
    struct Group { const char* name; std::vector<std::array<const char*, 2>> keys; };
    static const Group kGroups[] = {
        {"开关", {{"i", "补帧"}, {"s", "超分"}, {"m", "静音"}, {"t", "时间码"}}},
        {"调节", {{"f", "补帧档位 low / medium / high"}, {"q", "超分档位"}, {"+ / -", "音量"}}},
        {"其他", {{"l", "打印详细统计到日志"}, {"r", "重置统计"}, {"? / h", "关闭本帮助"}, {"esc", "退出"}}},
    };
    const int w = std::min(W, 46);
    Elements rows;
    for (const auto& g : kGroups) {
        if (!rows.empty()) rows.push_back(Line({}, w - 4));
        for (size_t i = 0; i < g.keys.size(); i++)
            rows.push_back(Line({M(PadR(i ? "" : g.name, 6)), CB(PadR(g.keys[i][0], 7), T.accent), S(g.keys[i][1])}, w - 4));
    }
    const int h = std::min(H, (int)rows.size() + 2);
    return Panel({B("按键")}, Rows(std::move(rows), h - 2), w, h) | ftxui::clear_under;
}

Element Model::Build(int W, int H, const std::vector<LogLine>& logs, bool help, bool quitting) const {
    W = std::max(W, 1); H = std::max(H, 1);
    const Theme& T = Th();
    Elements rows;
    rows.push_back(Header(W));
    int avail = H - 1 - 1;
    if (avail < 0) avail = 0;
    // Height budget, by priority: header, footer (always), latency, settings, history, record, log.
    if (W < 44 || avail < 9) {
        rows.push_back(Compact(W, avail));
    } else {
        int used = 0;
        const bool side = W >= 76, wide = W >= 100;
        const bool compact = H < 30;           // denser top panels on short terminals
        if (side) {
            const int lw = W / 2, rw = W - lw;
            Elements lat = LatencyRows(lw - 4, compact), set = SettingsRows(rw - 4, compact);
            const int topH = std::min(avail, 2 + (int)std::max(lat.size(), set.size()));
            rows.push_back(ftxui::hbox({
                Panel({B("延迟")}, Rows(std::move(lat), topH - 2), lw, topH),
                Panel({B("设置")}, Rows(std::move(set), topH - 2), rw, topH),
            }));
            used += topH;
        } else {
            Elements lat = LatencyRows(W - 4, compact);
            const int latH = std::min(avail, 2 + (int)lat.size());
            rows.push_back(Panel({B("延迟")}, Rows(std::move(lat), latH - 2), W, latH));
            used += latH;
            Elements set = SettingsRows(W - 4, false);
            if (avail - used < 2 + (int)set.size()) set = SettingsRows(W - 4, true);
            const int setH = 2 + (int)set.size();
            if (avail - used >= setH) {
                rows.push_back(Panel({B("设置")}, Rows(std::move(set), setH - 2), W, setH));
                used += setH;
            }
        }
        int rem = avail - used;
        // history graphs (with the log beside them when wide), else history then log stacked
        const int histMin = 7, histMax = 2 + 2 + 2 * 8 + 1, logMin = 4;
        int histH = 0, logH = 0;
        if (rem >= histMin) {
            if (wide) { histH = rem; rem = 0; }
            else {
                histH = std::min(rem, histMax);
                rem -= histH;
                if (rem >= logMin) { logH = rem; rem = 0; }
                else { histH += rem; rem = 0; }
            }
        }
        if (histH) {
            if (wide) {
                const int hw = (W * 3) / 5, lw = W - hw;
                rows.push_back(ftxui::hbox({HistoryPanel(hw, histH), LogPanel(lw, histH, logs)}));
            } else {
                rows.push_back(HistoryPanel(W, histH));
            }
        }
        if (logH) rows.push_back(LogPanel(W, logH, logs));
        const int left = avail - used - histH - logH;
        if (left > 0) rows.push_back(Blank(W, left));
    }
    rows.push_back(Footer(W, quitting));
    Element root = ftxui::vbox(std::move(rows));
    if (help) root = ftxui::dbox({root, Help(W, H) | ftxui::center});
    return root | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, W) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, H);
}

std::string PlainText(const ftxui::Screen& s) {
    std::string out;
    for (int y = 0; y < s.dimy(); y++) {
        std::string line;
        bool skip = false;
        for (int x = 0; x < s.dimx(); x++) {
            const auto& px = s.PixelAt(x, y);
            if (skip) { skip = false; continue; }
            line += px.character.empty() ? std::string(" ") : px.character;
            skip = Wid(px.character) == 2;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------ shared state / UI thread
struct DashShared {
    Telemetry* tel = nullptr;
    DashboardOptions opt;
    Model model;

    mutable std::mutex logM;
    std::deque<LogLine> logs;

    std::atomic<bool> stopReq{false}, done{false}, tuiActive{false};
    std::mutex wakeM;
    std::condition_variable wakeCv;   // record-only refresh / timer wakeups / done
    bool kick = false;

    std::mutex postM;                 // guards screen / loopAlive (Post from other threads)
    ftxui::ScreenInteractive* screen = nullptr;
    bool loopAlive = false;

    // UI-thread only
    bool help = false, quitting = false;

    // perf (written by the UI thread)
    std::atomic<uint64_t> draws{0}, cycles{0};
    std::atomic<double> buildMsSum{0}, buildMsMax{0};

    std::vector<LogLine> LogSnapshot() const {
        std::lock_guard<std::mutex> lk(logM);
        return {logs.begin(), logs.end()};
    }
    void Update() {
        model.Update(tel, Qpc::Now(), /*boostStatusRead*/ true);
        uint64_t c = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c);
        cycles.store(c, std::memory_order_relaxed);
    }
};

struct DashboardImpl {
    std::shared_ptr<DashShared> sh = std::make_shared<DashShared>();
    std::thread th;
    bool started = false, joined = true;
};

namespace {

bool ConsoleInteractive() {
    DWORD m;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m) && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m);
}

void RecordLoop(DashShared& s) {
    const auto period = std::chrono::duration<double>(1.0 / std::max(0.5, s.opt.refreshHz));
    while (!s.stopReq.load()) {
        {
            std::unique_lock<std::mutex> lk(s.wakeM);
            s.wakeCv.wait_for(lk, std::chrono::duration_cast<std::chrono::nanoseconds>(period), [&] { return s.stopReq.load(); });
        }
        if (s.stopReq.load()) break;
        s.Update();
    }
}

// The typed character as ASCII; full-width forms (U+FF01..U+FF5E, e.g. '？' '＋' 'ｆ' typed with a
// Chinese IME in full-width / Chinese-punctuation mode) map to their ASCII counterparts. 0 = other.
char AsciiOf(const ftxui::Event& e) {
    if (!e.is_character()) return 0;
    const std::string& c = e.character();
    if (c.size() == 1) return (unsigned char)c[0] < 0x80 ? c[0] : 0;
    if (c.size() == 3 && (unsigned char)c[0] == 0xEF) {
        const unsigned cp = (((unsigned char)c[0] & 0x0F) << 12) | (((unsigned char)c[1] & 0x3F) << 6) | ((unsigned char)c[2] & 0x3F);
        if (cp >= 0xFF01 && cp <= 0xFF5E) return (char)(cp - 0xFEE0);
    }
    return 0;
}

int KeyFor(const ftxui::Event& e) {
    const char ch = (char)toupper((unsigned char)AsciiOf(e));
    if (!ch) return 0;
    switch (ch) {
    case 'I': case 'F': case 'S': case 'Q': case 'M': case 'L': case 'R': case 'T': return ch;
    case '+': case '=': return VK_OEM_PLUS;
    case '-': case '_': return VK_OEM_MINUS;
    }
    return 0;
}

// Returns true when the loop ended because of a stop request (false = user quit / error).
bool TuiLoop(DashShared& s) {
    using namespace ftxui;
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);
    screen.ForceHandleCtrlC(false);
    bool registered = false;
    // FTXUI's Install() routes SIGINT to its own exit path (the loop would end behind our back,
    // racing the timer's PostEvent with ExitNow). Keyboard Ctrl+C is a key here anyway; a
    // CTRL_C_EVENT sent by another process should reach the app's console control handler (it
    // quits cleanly through Stop()), so the previous SIGINT handler is put back after Install
    // (FTXUI restores the same one on exit).
    using SigFn = void (*)(int);
    const SigFn prevInt = std::signal(SIGINT, SIG_DFL);
    if (prevInt != SIG_ERR) std::signal(SIGINT, prevInt);

    auto renderer = Renderer([&] {
        if (!registered) {     // first draw: the loop is installed, Post() works from now on
            if (prevInt != SIG_ERR) std::signal(SIGINT, prevInt);
            std::lock_guard<std::mutex> lk(s.postM);
            s.screen = &screen; s.loopAlive = true; registered = true;
        }
        const int64_t t0 = Qpc::Now();
        const Dimensions d = Terminal::Size();
        Element e;
        try {
            e = s.model.Build(d.dimx, d.dimy, s.LogSnapshot(), s.help, s.quitting);
        } catch (const std::exception& ex) {   // a view bug must not take the TUI (or the app) down
            e = text(std::string(" dashboard: ") + ex.what());
        } catch (...) {
            e = text(" dashboard: render error");
        }
        const double ms = Qpc::ToMs(Qpc::Now() - t0);
        s.draws.fetch_add(1, std::memory_order_relaxed);
        s.buildMsSum.store(s.buildMsSum.load() + ms, std::memory_order_relaxed);
        if (ms > s.buildMsMax.load()) s.buildMsMax.store(ms, std::memory_order_relaxed);
        return e;
    });
    auto exitLoop = [&] {
        { std::lock_guard<std::mutex> lk(s.postM); s.loopAlive = false; s.screen = nullptr; }
        screen.Exit();
    };
    auto comp = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Custom) {
            if (s.stopReq.load()) { exitLoop(); return true; }
            try { s.Update(); } catch (...) {}   // e.g. bad_alloc: skip this refresh, keep the TUI
            return true;
        }
        if (s.stopReq.load()) return true;      // stopping: the app may already be gone
        if (e == Event::CtrlC) { s.tel->PushKey(VK_ESCAPE); s.quitting = true; return true; }
        if (e == Event::Escape) {
            if (s.help) s.help = false;
            else { s.tel->PushKey(VK_ESCAPE); s.quitting = true; }
            return true;
        }
        if (const char a = AsciiOf(e); a == '?' || a == 'h' || a == 'H') { s.help = !s.help; return true; }
        if (const int vk = KeyFor(e)) {
            s.tel->PushKey(vk);
            if (vk == 'R') s.model.ResetWindow();
            return true;
        }
        return e.is_character();   // swallow other characters
    });

    // refresh timer: posts a custom event at refreshHz (only while the loop accepts posts)
    bool timerStop = false;
    std::thread timer([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(1.0 / std::max(0.5, s.opt.refreshHz)));
        std::unique_lock<std::mutex> lk(s.wakeM);
        while (!timerStop) {
            s.wakeCv.wait_for(lk, period, [&] { return timerStop || s.kick; });
            if (timerStop) break;
            s.kick = false;
            lk.unlock();
            {
                std::lock_guard<std::mutex> plk(s.postM);
                if (s.loopAlive && s.screen) s.screen->PostEvent(Event::Custom);
            }
            lk.lock();
        }
    });
    struct TimerJoin {
        DashShared& s; bool& stop; std::thread& t;
        ~TimerJoin() {
            { std::lock_guard<std::mutex> lk(s.wakeM); stop = true; }
            s.wakeCv.notify_all();
            if (t.joinable()) t.join();
            std::lock_guard<std::mutex> plk(s.postM);
            s.loopAlive = false; s.screen = nullptr;
        }
    } timerJoin{s, timerStop, timer};

    screen.Loop(comp);
    return s.stopReq.load();
}

void UiMain(std::shared_ptr<DashShared> sp) {
    DashShared& s = *sp;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"camss ui");
    if (s.tuiActive.load()) {
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE), hout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD inMode = 0, outMode = 0;
        const bool haveIn = GetConsoleMode(hin, &inMode) != 0, haveOut = GetConsoleMode(hout, &outMode) != 0;
        const UINT cpOut = GetConsoleOutputCP(), cpIn = GetConsoleCP();
        // Ctrl+C arrives as a key (forwarded as ESC) instead of a console control event / SIGINT.
        // Quick-edit off: in conhost a stray click would start a selection, which blocks every
        // console write (the UI thread, and Stop() would then time out). Restored below
        // (ENABLE_EXTENDED_FLAGS makes SetConsoleMode apply the quick-edit / insert bits).
        if (haveIn)
            SetConsoleMode(hin, (inMode | ENABLE_EXTENDED_FLAGS) & ~(DWORD)(ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE));
        bool stopped = false, threw = false;
        std::string what;
        try {
            stopped = TuiLoop(s);
        } catch (const std::exception& e) {
            threw = true; what = e.what();
        } catch (...) {
            threw = true;
        }
        if (threw) {   // FTXUI's loop already restored its state while unwinding; make sure
            static const char kReset[] = "\x1b[0m\x1b[?1049l\x1b[?25h\x1b[?7h\r\n";
            fwrite(kReset, 1, sizeof(kReset) - 1, stdout);
            fflush(stdout);
        }
        if (haveIn) SetConsoleMode(hin, inMode | ENABLE_EXTENDED_FLAGS);
        if (haveOut) SetConsoleMode(hout, outMode);
        SetConsoleOutputCP(cpOut);
        SetConsoleCP(cpIn);
        s.tuiActive.store(false);
        if (!stopped && !s.stopReq.load()) {
            if (threw) {
                // A dashboard error must not end the video session: keep recording, tell the
                // user (TuiActive() is false from now on; the app may re-enable console output).
                const std::string m = "\r\ncamss: terminal dashboard stopped (" + (what.empty() ? std::string("error") : what) +
                                      "); video keeps running, Ctrl+C or Esc in the video window quits\r\n";
                fwrite(m.data(), 1, m.size(), stdout);
                fflush(stdout);
            } else {
                // The loop ended on its own without an error (a termination signal): quit the app.
                s.tel->PushKey(VK_ESCAPE);
            }
            RecordLoop(s);
        }
    } else {
        RecordLoop(s);
    }
    // End the current segment here (not in Stop()): it is then also written when Stop() timed
    // out and detached this thread.
    try { s.model.Finish(Qpc::Now()); } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(s.wakeM);
        s.done.store(true);
    }
    s.wakeCv.notify_all();
}

} // namespace

// ------------------------------------------------------------------ Dashboard
Dashboard::Dashboard() : p_(std::make_unique<DashboardImpl>()) {}
Dashboard::~Dashboard() { Stop(); }

bool Dashboard::Start(Telemetry* tel, const DashboardOptions& o) {
    if (p_->started || !tel) return false;
    DashShared& s = *p_->sh;
    s.tel = tel;
    s.opt = o;
    s.model.SetOptions(o);
    s.model.SetFineBlocks(FineBlocks());
    s.stopReq.store(false);
    s.done.store(false);
    s.tuiActive.store(o.tui && ConsoleInteractive());
    p_->th = std::thread(UiMain, p_->sh);
    p_->started = true;
    p_->joined = false;
    return true;
}

void Dashboard::Stop() {
    if (!p_ || !p_->started || p_->joined) return;
    DashShared& s = *p_->sh;
    s.stopReq.store(true);
    {
        std::lock_guard<std::mutex> lk(s.wakeM);
        s.kick = true;
    }
    s.wakeCv.notify_all();
    {
        std::lock_guard<std::mutex> lk(s.postM);
        if (s.loopAlive && s.screen) s.screen->PostEvent(ftxui::Event::Custom);
    }
    bool finished;
    {
        std::unique_lock<std::mutex> lk(s.wakeM);
        finished = s.wakeCv.wait_for(lk, std::chrono::seconds(3), [&] { return s.done.load(); });
    }
    if (finished) {
        p_->th.join();                // the UI thread ended the current segment and wrote it
    } else {
        // The UI thread is stuck (e.g. console output paused by a text selection). It keeps its
        // own reference to the shared state and exits by itself once the console resumes.
        p_->th.detach();
    }
    p_->joined = true;
}

bool Dashboard::Running() const { return p_->started && !p_->joined && !p_->sh->done.load(); }
bool Dashboard::TuiActive() const { return p_->sh->tuiActive.load(); }

void Dashboard::OnLog(LogLevel lvl, double tSec, const char* msg, size_t len) {
    // multi-line messages (e.g. the detailed statistics report) become one entry per line
    std::vector<std::string> lines;
    size_t a = 0;
    while (a < len) {
        size_t b = a;
        while (b < len && msg[b] != '\n') b++;
        std::string t(msg + a, b - a);
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        for (char& c : t) if (c == '\r' || c == '\t') c = ' ';
        if (!t.empty()) lines.push_back(std::move(t));
        a = b + 1;
    }
    DashShared& s = *p_->sh;
    std::lock_guard<std::mutex> lk(s.logM);
    for (auto& t : lines) s.logs.push_back({lvl, tSec, std::move(t)});
    while (s.logs.size() > kMaxLogLines) s.logs.pop_front();
}

DashboardPerf Dashboard::Perf() const {
    const DashShared& s = *p_->sh;
    DashboardPerf r;
    r.refreshes = s.model.refreshes_.load();
    r.frames = s.model.framesSeen_.load();
    r.updateMsAvg = r.refreshes ? s.model.updMsSum_.load() / (double)r.refreshes : 0;
    r.updateMsMax = s.model.updMsMax_.load();
    r.draws = s.draws.load();
    r.buildMsAvg = r.draws ? s.buildMsSum.load() / (double)r.draws : 0;
    r.buildMsMax = s.buildMsMax.load();
    r.uiCycles = s.cycles.load();
    return r;
}

void Dashboard::Pump(Telemetry* tel, const DashboardOptions& o, int64_t nowQpc) {
    DashShared& s = *p_->sh;
    if (!s.tel) { s.tel = tel; s.opt = o; s.model.SetOptions(o); }
    s.model.Update(tel, nowQpc);
}

void Dashboard::Finish(int64_t nowQpc) { p_->sh->model.Finish(nowQpc); }

std::string Dashboard::RenderText(int width, int height, bool ansi, bool help) const {
    const DashShared& s = *p_->sh;
    width = std::max(width, 1); height = std::max(height, 1);
    Element e = s.model.Build(width, height, s.LogSnapshot(), help, false);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, e);
    return ansi ? screen.ToString() + "\n" : PlainText(screen);
}

} // namespace camss
