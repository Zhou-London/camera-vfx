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
#include "../util/stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace cameravfx {
namespace {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

constexpr double kLiveWindowSec = 10.0;
constexpr size_t kMaxWindowFrames = 20000;
constexpr double kBucketSec = 0.25;
constexpr int kHistoryBuckets = 240;
constexpr size_t kMaxLogLines = 300;
constexpr double kNoFramesSec = 1.0;
constexpr double kMinFpsSpanSec = 0.2;
constexpr double kDefaultContentFps = 60.0;
constexpr double kLatencyMarginMs = 4.0;
constexpr size_t kLateWarnMinFrames = 100;
constexpr double kLateWarnFraction = 0.01;
constexpr double kPausingChangeDebounceMs = 700;
constexpr auto kRefreshPeriod = std::chrono::milliseconds(100);

struct Palette {
    Color accent, good, warn, bad, muted, line, band;
};
const Palette& Colors() {
    static const Palette p{
        Color(217, 119, 87),
        Color(98, 180, 110),
        Color(214, 163, 56),
        Color(224, 88, 78),
        Color(136, 136, 136),
        Color(98, 98, 98),
        Color(150, 100, 82),
    };
    return p;
}

int DisplayWidth(const std::string& s) { return ftxui::string_width(s); }

bool Known(double v) { return v >= 0 && std::isfinite(v); }
std::string FormatNumber(double v, int decimals) { return Known(v) ? std::format("{:.{}f}", v, decimals) : "-"; }
std::string FormatAdaptive(double v) {
    if (!Known(v)) return "-";
    if (v < 1) return std::format("{:.2f}", v);
    if (v < 100) return std::format("{:.1f}", v);
    return std::format("{:.0f}", v);
}
std::string FormatClock(double sec) {
    const long s = (long)std::max(sec, 0.0);
    if (s >= 3600) return std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
    return std::format("{}:{:02}", s / 60, s % 60);
}
std::string FormatResolution(FrameSize size) {
    if (!size.width || !size.height) return "-";
    if (size.width * 9 == size.height * 16) return std::format("{}p", size.height);
    return std::format("{}x{}", size.width, size.height);
}
std::string Repeat(const char* s, int n) { std::string r; for (int i = 0; i < n; i++) r += s; return r; }

struct SpanStyle {
    static constexpr uint8_t kPlain = 0, kBold = 1, kMuted = 2, kColored = 4;
};
struct Span {
    std::string s;
    uint8_t style = SpanStyle::kPlain;
    Color c{};
};
using Spans = std::vector<Span>;

Span Plain(std::string s) { return {std::move(s), SpanStyle::kPlain, {}}; }
Span Bold(std::string s) { return {std::move(s), SpanStyle::kBold, {}}; }
Span Muted(std::string s) { return {std::move(s), SpanStyle::kMuted, {}}; }
Span Colored(std::string s, Color c) { return {std::move(s), SpanStyle::kColored, c}; }
Span ColoredBold(std::string s, Color c) { return {std::move(s), (uint8_t)(SpanStyle::kColored | SpanStyle::kBold), c}; }

int DisplayWidth(const Spans& v) { int w = 0; for (const auto& s : v) w += DisplayWidth(s.s); return w; }
Spans& operator+=(Spans& a, const Spans& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

Spans TruncateSpans(const Spans& in, int w) {
    if (DisplayWidth(in) <= w) return in;
    Spans out;
    int cur = 0;
    for (const auto& sp : in) {
        const int sw = DisplayWidth(sp.s);
        if (cur + sw <= w - 1) { out.push_back(sp); cur += sw; continue; }
        Span t = sp;
        t.s.clear();
        for (const auto& g : ftxui::Utf8ToGlyphs(sp.s)) {
            const int gw = DisplayWidth(g);
            if (cur + gw > w - 1) break;
            t.s += g;
            cur += gw;
        }
        t.s += "…";
        out.push_back(t);
        break;
    }
    return out;
}

std::string Truncate(const std::string& s, int w) {
    if (w <= 0) return {};
    const Spans t = TruncateSpans({Plain(s)}, w);
    return t.empty() ? std::string() : t.front().s;
}
std::string PadRight(const std::string& s, int w) { std::string t = Truncate(s, w); return t + std::string((size_t)std::max(0, w - DisplayWidth(t)), ' '); }
std::string PadLeft(const std::string& s, int w) { std::string t = Truncate(s, w); return std::string((size_t)std::max(0, w - DisplayWidth(t)), ' ') + t; }

Element ToElement(const Span& sp) {
    Element e = ftxui::text(sp.s);
    if (sp.style & SpanStyle::kMuted) e = e | ftxui::color(Colors().muted);
    if (sp.style & SpanStyle::kColored) e = e | ftxui::color(sp.c);
    if (sp.style & SpanStyle::kBold) e = e | ftxui::bold;
    return e;
}
Element Line(const Spans& v, int w) {
    Spans t = TruncateSpans(v, w);
    Elements els;
    for (const auto& sp : t) els.push_back(ToElement(sp));
    const int rest = w - DisplayWidth(t);
    if (rest > 0) els.push_back(ftxui::text(std::string((size_t)rest, ' ')));
    return ftxui::hbox(std::move(els));
}
Element FirstFitting(const std::vector<Spans>& variants, int w) {
    for (const auto& v : variants) if (DisplayWidth(v) <= w) return Line(v, w);
    return Line(variants.back(), w);
}
Element LineLeftRight(const Spans& left, const Spans& right, int w) {
    const int rw = DisplayWidth(right);
    if (rw + 2 > w) return Line(left, w);
    Elements els{Line(left, w - rw - 1), ftxui::text(" ")};
    for (const auto& sp : right) els.push_back(ToElement(sp));
    return ftxui::hbox(std::move(els));
}

constexpr int kPanelBorderRows = 2;
constexpr int kPanelBorderColumns = 4;

Element Panel(const Spans& title, Element body, int w, int h) {
    const Palette& colors = Colors();
    const Spans shownTitle = TruncateSpans(title, std::max(0, w - 6));
    Elements top{ftxui::text("╭─ ") | ftxui::color(colors.line)};
    for (const auto& sp : shownTitle) top.push_back(ToElement(sp));
    top.push_back(ftxui::text(" " + Repeat("─", std::max(0, w - 5 - DisplayWidth(shownTitle))) + "╮") | ftxui::color(colors.line));
    auto sideBorder = [&] {
        Elements side;
        for (int i = 0; i < h - kPanelBorderRows; i++) side.push_back(ftxui::text("│"));
        return ftxui::vbox(std::move(side)) | ftxui::color(colors.line);
    };
    Element mid = ftxui::hbox({
        sideBorder(), ftxui::text(" "),
        body | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, std::max(0, w - kPanelBorderColumns)) |
            ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, std::max(0, h - kPanelBorderRows)) | ftxui::frame,
        ftxui::text(" "), sideBorder(),
    });
    Element bottom = ftxui::text("╰" + Repeat("─", std::max(0, w - 2)) + "╯") | ftxui::color(colors.line);
    return ftxui::vbox({ftxui::hbox(std::move(top)), mid, bottom}) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, w) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, h);
}
Element ClipRows(Elements rows, int h) {
    rows.resize(std::min(rows.size(), (size_t)std::max(h, 0)));
    return ftxui::vbox(std::move(rows));
}
Element Blank(int w, int h) {
    Elements rows;
    for (int i = 0; i < h; i++) rows.push_back(ftxui::text(std::string((size_t)std::max(0, w), ' ')));
    return ftxui::vbox(std::move(rows));
}

const char* const kBigDigitGlyphs[10][3] = {
    {"╭─╮", "│ │", "╰─╯"}, {" ╷ ", " │ ", " ╵ "}, {"╶─╮", "╭─╯", "╰─╴"}, {"╶─╮", " ─┤", "╶─╯"},
    {"╷ ╷", "╰─┤", "  ╵"}, {"╭─╴", "╰─╮", "╶─╯"}, {"╭─╴", "├─╮", "╰─╯"}, {"╶─╮", "  │", "  ╵"},
    {"╭─╮", "├─┤", "╰─╯"}, {"╭─╮", "╰─┤", "╶─╯"},
};
std::array<std::string, 3> BigDigits(const std::string& s) {
    std::array<std::string, 3> r;
    for (size_t i = 0; i < s.size(); i++) {
        const char ch = s[i];
        const bool last = i + 1 == s.size();
        if (ch >= '0' && ch <= '9') {
            for (int k = 0; k < 3; k++) r[k] += kBigDigitGlyphs[ch - '0'][k];
            if (!last && s[i + 1] != '.') for (auto& x : r) x += " ";
        } else if (ch == '.') {
            r[0] += "  "; r[1] += "  "; r[2] += "╸ ";
        } else {
            r[0] += "   "; r[1] += "╶─╴"; r[2] += "   ";
            if (!last) for (auto& x : r) x += " ";
        }
    }
    return r;
}

bool TerminalHasEighthBlocks() { return getenv("WT_SESSION") != nullptr || getenv("TERM_PROGRAM") != nullptr; }

struct GraphData {
    std::vector<float> lo, hi;
    float vmin = 0, vmax = 1;
    Color loColor, hiColor, warnColor;
    float warnAbove = kUnknownMs, reference = kUnknownMs;
};
class BarGraph : public ftxui::Node {
public:
    BarGraph(GraphData d, bool eighthBlocks) : d_(std::move(d)), eighthBlocks_(eighthBlocks) {}
    void ComputeRequirement() override {
        requirement_.min_x = 1; requirement_.min_y = 1;
        requirement_.flex_grow_x = 1; requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_x = 1; requirement_.flex_shrink_y = 1;
    }
    void Render(ftxui::Screen& screen) override {
        static const char* const kEighths[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        const int W = box_.x_max - box_.x_min + 1, H = box_.y_max - box_.y_min + 1;
        if (W <= 0 || H <= 0) return;
        const int n = (int)d_.lo.size();
        const int firstBucketOnTimeline = kHistoryBuckets - n;
        const float span = std::max(1e-6f, d_.vmax - d_.vmin);
        auto toEighths = [&](float v) { return (int)std::lround(std::clamp((v - d_.vmin) / span, 0.f, 1.f) * (float)(H * 8)); };
        const int referenceEighths = Known(d_.reference) ? toEighths(d_.reference) : -1;
        auto shown = [&](int eighths) { return eighthBlocks_ || eighths == 0 ? eighths : (eighths <= 4 ? 4 : 8); };
        for (int c = 0; c < W; c++) {
            float lo = kUnknownMs, hi = kUnknownMs;
            int cnt = 0;
            double sum = 0;
            const int b0 = (int)((int64_t)c * kHistoryBuckets / W);
            const int b1 = std::max(b0 + 1, (int)((int64_t)(c + 1) * kHistoryBuckets / W));
            for (int b = b0; b < b1; b++) {
                const int i = b - firstBucketOnTimeline;
                if (i < 0 || i >= n) continue;
                if (Known(d_.lo[i])) { sum += d_.lo[i]; cnt++; }
                if (i < (int)d_.hi.size() && d_.hi[i] > hi) hi = d_.hi[i];
            }
            if (cnt) lo = (float)(sum / cnt);
            const int loEighths = Known(lo) ? std::max(1, toEighths(lo)) : 0;
            const int hiEighths = Known(hi) ? std::max(loEighths, toEighths(hi)) : 0;
            const Color loColor = (Known(d_.warnAbove) && lo > d_.warnAbove) ? d_.warnColor : d_.loColor;
            for (int row = 0; row < H; row++) {
                const int base = row * 8;
                const int lf = shown(std::clamp(loEighths - base, 0, 8)), hf = shown(std::clamp(hiEighths - base, 0, 8));
                auto& px = screen.PixelAt(box_.x_min + c, box_.y_max - row);
                if (lf > 0) {
                    px.character = kEighths[lf];
                    px.foreground_color = loColor;
                    if (lf < 8 && hf > lf) px.background_color = d_.hiColor;
                } else if (hf > 0) {
                    px.character = kEighths[hf];
                    px.foreground_color = d_.hiColor;
                } else if (referenceEighths > base && referenceEighths <= base + 8) {
                    px.character = "┄";
                    px.foreground_color = Colors().line;
                } else {
                    px.character = " ";
                }
            }
        }
    }
private:
    GraphData d_;
    bool eighthBlocks_;
};

float QuantileOrUnknown(std::vector<float>& v, double q) { return v.empty() ? kUnknownMs : Quantile(v, q); }

struct Bucket {
    float p50 = kUnknownMs, p99 = kUnknownMs, delay = kUnknownMs, gpu = kUnknownMs;
};

struct WindowStats {
    size_t frames = 0;
    double fps = 0;
    double fpsLastSecond = 0;
    float latencyP50 = kUnknownMs, latencyP99 = kUnknownMs, contentLatencyP50 = kUnknownMs, contentLatencyP99 = kUnknownMs;
    float gpuP50 = kUnknownMs, gpuP99 = kUnknownMs;
    uint64_t late = 0, hold = 0;
};

struct LogLine {
    LogLevel lvl;
    double t;
    std::string text;
};

struct Popup {
    bool open = false;
    int cursor = 0;
    struct Pending { int value; int64_t changedAt; bool sent; };
    std::map<SettingId, Pending> pending;
    int Value(const SettingItem& it) const {
        auto p = pending.find(it.id);
        return p != pending.end() ? p->second.value : it.value;
    }
    bool Waiting(const SettingItem& it) const {
        auto p = pending.find(it.id);
        return p != pending.end() && !p->second.sent;
    }
};

struct BarSegment { std::string name; double v; Color c; };

void SegmentBar(const std::vector<BarSegment>& segments, double scale, int barWidth, const Spans& lead, const Spans& tail,
                Elements& rows, int iw) {
    const Palette& colors = Colors();
    Spans bar = lead, labels{Plain(std::string((size_t)DisplayWidth(lead), ' '))};
    int filled = 0, labelsEnd = 0;
    double acc = 0;
    const int labelLimit = barWidth + DisplayWidth(tail);
    for (const auto& g : segments) {
        if (g.v <= 0) continue;
        const int x0 = (int)std::lround(acc / scale * barWidth);
        acc += g.v;
        const int x1 = std::min(barWidth, (int)std::lround(acc / scale * barWidth));
        if (x1 > x0) bar.push_back(Colored(Repeat("█", x1 - x0), g.c));
        filled = std::max(filled, x1);
        const int labelWidth = DisplayWidth(g.name + " " + FormatAdaptive(g.v));
        const int at = std::min(x0, labelLimit - labelWidth - 1);
        if (x1 > x0 && at >= labelsEnd) {
            if (at > labelsEnd) labels.push_back(Plain(std::string((size_t)(at - labelsEnd), ' ')));
            labels.push_back(Colored(g.name + " ", colors.muted));
            labels.push_back(Plain(FormatAdaptive(g.v)));
            labels.push_back(Plain(" "));
            labelsEnd = at + labelWidth + 1;
        }
    }
    if (filled < barWidth) bar.push_back(Colored(Repeat("░", barWidth - filled), colors.line));
    bar += tail;
    rows.push_back(Line(bar, iw));
    rows.push_back(Line(labels, iw));
}

class Model {
public:
    void SetEighthBlocks(bool on) { eighthBlocks_ = on; }

    void Update(Telemetry* tel, int64_t now) {
        now_ = now;
        const uint64_t epoch = status_.statsEpoch;
        const bool hadStatus = HaveStatus();
        if (tel->Status(statusSeq_, status_) && hadStatus && status_.statsEpoch != epoch) ResetWindow();
        if (tel->Settings(settingsSeq_, settings_)) settingsChanged_ = true;
        FrameSample f;
        while (tel->PopFrame(f)) {
            lastFrameQpc_ = std::max(lastFrameQpc_, f.qpc);
            if (f.qpc >= windowStart_) window_.push_back(f);
            AddToBucket(f);
        }
        const int64_t windowTicks = Qpc::FromMs(kLiveWindowSec * 1000);
        while (!window_.empty() && window_.front().qpc < now - windowTicks) window_.pop_front();
        while (window_.size() > kMaxWindowFrames) window_.pop_front();
        AdvanceBuckets(now);
        ComputeWindow(now);
    }
    const std::vector<SettingItem>& SettingsList() const { return settings_; }
    bool TakeSettingsChanged() { return std::exchange(settingsChanged_, false); }

    Element Build(int W, int H, const std::vector<LogLine>& logs, const Popup& popup, bool quitting) const;

private:
    bool HaveStatus() const { return statusSeq_ != 0; }
    void ResetWindow() { window_.clear(); windowStart_ = now_; }

    void AddToBucket(const FrameSample& f) {
        const int64_t index = f.qpc / Qpc::FromMs(kBucketSec * 1000);
        if (bucketIndex_ < 0) bucketIndex_ = index;
        if (index > bucketIndex_) {
            FinishBucket();
            PushEmptyBuckets(index - bucketIndex_ - 1);
            bucketIndex_ = index;
        }
        if (!(f.flags & FrameSample::kHold) && Known(f.latencyMs)) bucketLat_.push_back(f.latencyMs);
        bucketDelay_.push_back(f.delayMs);
        if (f.gpuMs > 0) bucketGpu_.push_back(f.gpuMs);
    }
    void AdvanceBuckets(int64_t now) {
        const int64_t nowIndex = now / Qpc::FromMs(kBucketSec * 1000);
        if (bucketIndex_ < 0) { bucketIndex_ = nowIndex; return; }
        if (nowIndex > bucketIndex_ + 1) {
            FinishBucket();
            PushEmptyBuckets(nowIndex - bucketIndex_ - 2);
            bucketIndex_ = nowIndex - 1;
        }
    }
    void FinishBucket() {
        Bucket b;
        if (!bucketDelay_.empty()) {
            b.p50 = QuantileOrUnknown(bucketLat_, 0.5);
            b.p99 = QuantileOrUnknown(bucketLat_, 0.99);
            double sum = 0;
            for (float v : bucketDelay_) sum += v;
            b.delay = (float)(sum / bucketDelay_.size());
            b.gpu = QuantileOrUnknown(bucketGpu_, 0.5);
        }
        history_.push_back(b);
        while (history_.size() > (size_t)kHistoryBuckets) history_.pop_front();
        bucketLat_.clear(); bucketDelay_.clear(); bucketGpu_.clear();
    }
    void PushEmptyBuckets(int64_t n) {
        n = std::min<int64_t>(n, kHistoryBuckets);
        for (int64_t i = 0; i < n; i++) history_.push_back(Bucket{});
        while (history_.size() > (size_t)kHistoryBuckets) history_.pop_front();
    }

    void ComputeWindow(int64_t now) {
        WindowStats w;
        winLat_.clear(); winCont_.clear(); winGpu_.clear();
        for (const auto& f : window_) {
            const bool hold = (f.flags & FrameSample::kHold) != 0;
            if (f.flags & FrameSample::kLate) w.late++;
            if (hold) w.hold++;
            if (!hold && Known(f.latencyMs)) winLat_.push_back(f.latencyMs);
            if (!hold && Known(f.contentMs)) winCont_.push_back(f.contentMs);
            if (f.gpuMs > 0) winGpu_.push_back(f.gpuMs);
        }
        w.frames = window_.size();
        w.latencyP50 = QuantileOrUnknown(winLat_, 0.5); w.latencyP99 = QuantileOrUnknown(winLat_, 0.99);
        w.contentLatencyP50 = QuantileOrUnknown(winCont_, 0.5); w.contentLatencyP99 = QuantileOrUnknown(winCont_, 0.99);
        w.gpuP50 = QuantileOrUnknown(winGpu_, 0.5); w.gpuP99 = QuantileOrUnknown(winGpu_, 0.99);
        if (window_.size() >= 2 && Qpc::ToSec(now - window_.back().qpc) < kNoFramesSec) {
            const int64_t last = window_.back().qpc;
            const double span = Qpc::ToSec(last - window_.front().qpc);
            if (span > kMinFpsSpanSec) w.fps = (double)(window_.size() - 1) / span;
            size_t framesLastSecond = 0;
            int64_t firstLastSecond = last;
            for (auto it = window_.rbegin(); it != window_.rend() && it->qpc > last - Qpc::Freq(); ++it) {
                framesLastSecond++;
                firstLastSecond = it->qpc;
            }
            const double spanLastSecond = Qpc::ToSec(last - firstLastSecond);
            if (framesLastSecond >= 2 && spanLastSecond > kMinFpsSpanSec) w.fpsLastSecond = (double)(framesLastSecond - 1) / spanLastSecond;
        }
        windowStats_ = w;
    }

    struct Health { Color c; std::string label; };
    struct Headline { double p50, p99; };
    Health HealthOf() const;
    Headline HeadlineLatency() const;
    double CaptureMs() const;
    Color HeadlineColor(double v) const;
    Spans FgDescription() const;
    Spans SrDescription() const;
    void InfoRows(int iw, Elements& rows) const;
    Element Header(int W) const;
    Elements LatencyRows(int iw, bool compact) const;
    void LatencyComposition(double total, int iw, Elements& rows) const;
    void GpuComposition(int iw, Elements& rows) const;
    void ProblemRows(int iw, Elements& rows) const;
    Elements SettingsRows(int iw, bool compact) const;
    Element HistoryPanel(int w, int h) const;
    Element TimeAxis(int gutter, int graphWidth, int iw) const;
    int TopPanels(int W, int avail, bool compact, Elements& rows) const;
    void BottomPanels(int W, int rem, const std::vector<LogLine>& logs, Elements& rows) const;
    Element LogPanel(int w, int h, const std::vector<LogLine>& logs) const;
    Element Footer(int W, bool quitting) const;
    Element Compact(int W, int h) const;
    Element SettingsPopup(int W, int H, const Popup& popup) const;
    Color LatencyColor(double v) const;
    double ContentPeriodMs() const { return 1000.0 / (status_.contentFps > 1 ? status_.contentFps : kDefaultContentFps); }

    bool eighthBlocks_ = true;
    AppStatus status_;
    uint64_t statusSeq_ = 0;
    int64_t now_ = 0, lastFrameQpc_ = 0;

    std::deque<FrameSample> window_;
    WindowStats windowStats_;
    int64_t windowStart_ = 0;
    std::vector<float> winLat_, winCont_, winGpu_;

    std::deque<Bucket> history_;
    int64_t bucketIndex_ = -1;
    std::vector<float> bucketLat_, bucketDelay_, bucketGpu_;

    std::vector<SettingItem> settings_;
    uint64_t settingsSeq_ = 0;
    bool settingsChanged_ = false;
};

Model::Health Model::HealthOf() const {
    const Palette& colors = Colors();
    if (!HaveStatus()) return {colors.warn, "等待状态"};
    if (!status_.loading.empty()) return {colors.warn, "正在加载 " + status_.loading + "，暂停播放"};
    const double sinceLastFrame = lastFrameQpc_ ? Qpc::ToSec(now_ - lastFrameQpc_) : kNoFramesSec + 1;
    if (status_.warmupDone && sinceLastFrame > kNoFramesSec) return {colors.bad, "无画面"};
    if (status_.gpuBudgetMs > 0 && windowStats_.gpuP50 > status_.gpuBudgetMs) return {colors.bad, "GPU 超预算"};
    if (!status_.warmupDone) return {colors.warn, "预热中"};
    if (status_.probing) return {colors.warn, "探测中"};
    if (windowStats_.frames > kLateWarnMinFrames && (double)windowStats_.late > kLateWarnFraction * (double)windowStats_.frames) return {colors.warn, "迟到偏多"};
    return {colors.good, "运行中"};
}

Model::Headline Model::HeadlineLatency() const {
    const WindowStats& w = windowStats_;
    if (Known(w.contentLatencyP50)) return {w.contentLatencyP50, w.contentLatencyP99};
    return {w.latencyP50, w.latencyP99};
}

double Model::CaptureMs() const { return std::max(status_.stages.captureMs, 0.0) + std::max(status_.stages.uploadMs, 0.0); }

Color Model::HeadlineColor(double v) const { return Known(v) ? LatencyColor(v - CaptureMs()) : Colors().muted; }

Spans Model::FgDescription() const {
    const Palette& colors = Colors();
    if (!status_.fg) return {Plain("未配置")};
    Spans w{Plain(status_.fgQuality)};
    if (status_.fgLoading) {
        w.push_back(Colored("  加载中…", colors.warn));
        return w;
    }
    w.push_back(Plain(std::format(" · ×{}", std::max(1, status_.fgMultiplier))));
    const MultiplierLimit limit = status_.fgMultiplierLimit;
    if (limit == MultiplierLimit::Threshold) w.push_back(Muted(std::format("（到门槛 {:.0f}）", status_.fgThresholdFps)));
    else if (limit == MultiplierLimit::Refresh) w.push_back(Colored(std::format("（刷新率限制，要求 ×{}）", status_.fgMultiplierRequested), colors.warn));
    else if (limit == MultiplierLimit::Gpu) w.push_back(Colored(std::format("（GPU 限制，要求 ×{}）", status_.fgMultiplierRequested), colors.warn));
    else if (status_.fgThresholdFps > 0) w.push_back(Muted(std::format(" · 门槛 {:.0f}", status_.fgThresholdFps)));
    if (status_.detectRepeats) w.push_back(Plain(" · 识别重复帧"));
    return w;
}

Spans Model::SrDescription() const {
    Spans w{Plain(status_.sr ? status_.srQuality : "未配置")};
    if (status_.srLoading) w.push_back(Colored("  加载中…", Colors().warn));
    return w;
}

Color Model::LatencyColor(double v) const {
    const Palette& colors = Colors();
    if (v < 0) return colors.muted;
    const double period = ContentPeriodMs();
    if (v <= period + kLatencyMarginMs) return colors.good;
    if (v <= 2 * period + kLatencyMarginMs) return colors.warn;
    return colors.bad;
}

Element Model::Header(int W) const {
    const Palette& colors = Colors();
    const Health h = HealthOf();
    const Spans brand{Colored(" ✻ ", colors.accent), ColoredBold("CameraVFX", colors.accent), Plain("  "), Colored("●", h.c), Plain(" " + h.label)};
    const double out = windowStats_.fpsLastSecond > 0 ? windowStats_.fpsLastSecond : status_.outFps;
    const std::string content = status_.contentStatic ? "静止" : FormatNumber(status_.contentFps, 1);
    Spans multiplierMark;
    if (status_.fgActive && status_.fgMultiplier > 1) multiplierMark.push_back(Colored(std::format(" ×{}", status_.fgMultiplier), colors.accent));
    Spans rates;
    if (status_.signalHz > 0) rates += {Muted("输入 "), Plain(std::format("{:.2f} Hz", status_.signalHz)), Muted(" → ")};
    rates += {Muted("内容 "), Plain(status_.contentStatic ? content : content + " fps"), Muted(" → 输出 "), Plain(FormatNumber(out, 1) + " fps")};
    rates += multiplierMark;
    Spans shortRates{Muted("内容 "), Plain(content), Muted(" → 输出 "), Plain(FormatNumber(out, 1) + " fps")};
    shortRates += multiplierMark;
    Spans resolution;
    if (status_.sourceSize.height) {
        resolution.push_back(Plain(FormatResolution(status_.sourceSize)));
        if (status_.outputSize.height && status_.outputSize.height != status_.sourceSize.height) resolution += {Muted(" → "), Plain(FormatResolution(status_.outputSize))};
        if (status_.hdr) resolution.push_back(Colored(" HDR", colors.accent));
    }
    const Spans right{Muted("up "), Plain(FormatClock(status_.uptimeSec)), Plain(" ")};

    auto join = [&](std::initializer_list<const Spans*> parts) {
        Spans r = brand;
        for (const Spans* p : parts) if (!p->empty()) { r.push_back(Muted("   ")); r += *p; }
        return r;
    };
    for (const Spans& x : {join({&rates, &resolution}), join({&rates}), join({&shortRates}), brand})
        if (DisplayWidth(x) + DisplayWidth(right) + 2 <= W) return LineLeftRight(x, right, W);
    return Line(brand, W);
}

Elements Model::LatencyRows(int iw, bool compact) const {
    Elements rows;
    const auto [total, p99] = HeadlineLatency();
    const Color totalColor = HeadlineColor(total);
    const auto big = BigDigits(FormatAdaptive(total));
    for (int k = 0; k < 3; k++) {
        Spans l{ColoredBold(big[k], totalColor)};
        if (k == 2) l.push_back(Muted(" ms"));
        Spans r;
        if (k == 1 && Known(p99)) r = {Muted("p99 "), ColoredBold(FormatAdaptive(p99), HeadlineColor(p99))};
        rows.push_back(LineLeftRight(l, r, iw));
    }
    if (!compact) rows.push_back(Line({}, iw));
    if (total > 0) LatencyComposition(total, iw, rows);
    if (!compact) rows.push_back(Line({}, iw));
    if (windowStats_.gpuP50 > 0) GpuComposition(iw, rows);
    ProblemRows(iw, rows);
    return rows;
}

void Model::LatencyComposition(double total, int iw, Elements& rows) const {
    const Palette& colors = Colors();
    const double capture = CaptureMs();
    const double lead = std::max(0.0, status_.leadMs);
    const double gpu = windowStats_.gpuP50 > 0 ? std::min((double)windowStats_.gpuP50, lead) : 0;
    const double wait = std::max(0.0, total - capture - lead);
    std::vector<BarSegment> segments{{"采集", capture, colors.muted}, {"等待", wait, colors.band}};
    if (gpu > 0) { segments.push_back({"GPU", gpu, colors.good}); segments.push_back({"余量", std::max(0.0, lead - gpu), colors.line}); }
    else segments.push_back({"渲染", lead, colors.good});
    double sum = 0;
    for (const auto& g : segments) sum += g.v;
    SegmentBar(segments, std::max(sum, total), iw, {}, {}, rows, iw);
}

void Model::GpuComposition(int iw, Elements& rows) const {
    const Palette& colors = Colors();
    const double gpu50 = windowStats_.gpuP50, budget = status_.gpuBudgetMs;
    const bool over = budget > 0 && gpu50 > budget;
    std::vector<BarSegment> segments;
    double known = 0;
    auto add = [&](const char* name, double v, Color c) { if (v > 0) { segments.push_back({name, v, c}); known += v; } };
    add("补帧", status_.stages.fgMs, colors.accent);
    add("超分", status_.stages.srMs, colors.good);
    add("HDR", status_.stages.hdrMs, colors.warn);
    if (gpu50 - known > 0.05) segments.push_back({"其他", gpu50 - known, colors.muted});
    const Spans lead{Muted("GPU ")};
    Spans tail{Plain(" "), over ? ColoredBold(FormatAdaptive(gpu50), colors.bad) : Plain(FormatAdaptive(gpu50))};
    if (budget > 0) tail += {Muted(" / "), Plain(FormatAdaptive(budget))};
    tail.push_back(Muted(" ms"));
    const int barWidth = std::max(4, iw - DisplayWidth(lead) - DisplayWidth(tail));
    SegmentBar(segments, budget > 0 ? std::max(budget, gpu50) : gpu50, barWidth, lead, tail, rows, iw);
}

void Model::ProblemRows(int iw, Elements& rows) const {
    const Palette& colors = Colors();
    Spans problems;
    auto addProblem = [&](const char* name, uint64_t v) {
        if (!v) return;
        if (!problems.empty()) problems.push_back(Muted(" · "));
        problems += {Muted(std::string(name) + " "), Colored(std::to_string(v), colors.warn)};
    };
    addProblem("迟到", status_.late);
    addProblem("丢帧", status_.drops);
    addProblem("错过", status_.missed);
    if (!problems.empty()) rows.push_back(Line(problems, iw));
    if (!status_.notice.empty()) rows.push_back(Line({Colored("! " + status_.notice, colors.warn)}, iw));
}

enum class SwitchState { Unavailable, Off, On };

Elements Model::SettingsRows(int iw, bool compact) const {
    const Palette& colors = Colors();
    constexpr int kNameColumns = 8;
    constexpr int kMinDescriptionColumns = 8;
    Elements rows;
    auto switchRow = [&](const char* name, SwitchState state, Spans what, const char* key = nullptr, const char* keyAction = nullptr) {
        Spans l{Plain(" "), Plain(PadRight(name, kNameColumns))};
        if (state == SwitchState::On) l.push_back(Colored("● 开   ", colors.good));
        else if (state == SwitchState::Off) l.push_back(Muted("○ 关   "));
        else l.push_back(Muted("-      "));
        const int fixedWidth = DisplayWidth(l);
        if (state != SwitchState::On) for (auto& sp : what) if (!(sp.style & SpanStyle::kColored)) sp.style = SpanStyle::kMuted;
        l += what;
        Spans r;
        if (key) r = {ColoredBold(key, colors.accent), Muted(std::string(" ") + keyAction)};
        if (!r.empty() && fixedWidth + kMinDescriptionColumns + 2 + DisplayWidth(r) <= iw) rows.push_back(LineLeftRight(l, r, iw));
        else rows.push_back(Line(l, iw));
    };
    auto stateOf = [](bool available, bool on) { return !available ? SwitchState::Unavailable : on ? SwitchState::On : SwitchState::Off; };
    switchRow("补帧", stateOf(status_.fg, status_.fgOn), FgDescription());
    switchRow("超分", stateOf(status_.sr, status_.srOn), SrDescription());
    const SwitchState audio = stateOf(status_.audioEnabled, !status_.audioMuted);
    if (!status_.audioEnabled) switchRow("声音", audio, {Plain("未启用")});
    else if (!status_.audioRunning) switchRow("声音", audio, {Colored("设备未就绪", colors.warn)}, "m", "静音");
    else switchRow("声音", audio, {Plain(std::format("音量 {:.0f}%", status_.audioVolume * 100))}, "m", "静音");
    switchRow("时间码", stateOf(true, status_.timecode), {Plain("回环测延迟用")});

    if (!compact) rows.push_back(Line({Colored(Repeat("─", iw), colors.line)}, iw));
    InfoRows(iw, rows);
    return rows;
}

void Model::InfoRows(int iw, Elements& rows) const {
    const Palette& colors = Colors();
    constexpr int kNameColumns = 8;
    auto info = [&](const char* label, const Spans& v) {
        Spans l{Plain(" "), Muted(PadRight(label, kNameColumns))};
        l += v;
        rows.push_back(Line(l, iw));
    };
    {
        Spans v{Plain(status_.mode.empty() ? "-" : status_.mode)};
        if (status_.vrrActive) v += {Muted(" · "), Colored("VRR 生效", colors.good)};
        if (status_.displaySize.width) v += {Muted(" · "), Plain(std::format("{}x{} @ {:.0f} Hz", status_.displaySize.width, status_.displaySize.height, status_.displayHz))};
        if (status_.hdr) v += {Muted(" · "), Plain("HDR")};
        info("输出", v);
    }
    {
        Spans v{Plain(status_.captureName.empty() ? "-" : status_.captureName)};
        if (!status_.captureFormat.empty()) v += {Muted(" · "), Plain(status_.captureFormat)};
        info("采集", v);
    }
    if (status_.audioEnabled) {
        Spans v{Plain(status_.audioIn.empty() ? "-" : status_.audioIn), Muted(" → "), Plain(status_.audioOut.empty() ? "-" : status_.audioOut)};
        v += {Muted(" · 延迟 "), Plain(FormatNumber(status_.audioLatencyMs, 0)), Muted(" ms")};
        v += {Muted(" · 同步 "), Plain(std::format("{:+.0f}", status_.audioSyncMs))};
        if (status_.audioUnderruns) v += {Muted(" · 欠载 "), Colored(std::to_string(status_.audioUnderruns), colors.warn)};
        info("音频", v);
    }
    info("配置", {Plain(status_.configPath.empty() ? "-" : status_.configPath)});
}

Element Model::HistoryPanel(int w, int h) const {
    const Palette& colors = Colors();
    constexpr int kGutter = 6;
    const int iw = w - kPanelBorderColumns, ih = h - kPanelBorderRows;
    std::vector<float> p50, p99, gpu, delay;
    for (const auto& b : history_) { p50.push_back(b.p50); p99.push_back(b.p99); gpu.push_back(b.gpu); delay.push_back(b.delay); }
    auto range = [](std::initializer_list<const std::vector<float>*> series, float& mn, float& mx) {
        mn = 1e9f; mx = kUnknownMs;
        for (const auto* s : series) for (float v : *s) if (Known(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
        return Known(mx);
    };
    const bool haveGpu = std::any_of(gpu.begin(), gpu.end(), [](float v) { return v > 0; });
    const int graphWidth = std::max(1, iw - kGutter);
    const bool twoGraphs = ih >= 5;
    const int axisRows = ih >= 7 ? 1 : 0;
    const int graphRows = std::max(1, (twoGraphs ? ih - 2 : ih - 1) - axisRows);
    const int latencyRows = twoGraphs ? std::max(1, (graphRows + 1) / 2) : graphRows, secondRows = std::max(1, graphRows - latencyRows);
    Elements rows;
    auto graph = [&](GraphData g, const std::string& top, const std::string& bottom, int rowsN) {
        Elements labels;
        for (int i = 0; i < rowsN; i++) {
            const std::string t = i == 0 ? top : (i == rowsN - 1 ? bottom : "");
            labels.push_back(ftxui::text(PadLeft(t, kGutter - 1) + " ") | ftxui::color(colors.muted));
        }
        Element ge = std::make_shared<BarGraph>(std::move(g), eighthBlocks_) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, graphWidth) |
                     ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowsN);
        return ftxui::hbox({ftxui::vbox(std::move(labels)), ge});
    };
    auto minMax = [](float mn, float mx) { return Spans{Muted("min "), Plain(FormatAdaptive(mn)), Muted("  max "), Plain(FormatAdaptive(mx))}; };
    {
        float mn, mx;
        const bool ok = range({&p50, &p99}, mn, mx);
        float lo = 0, hi = 1;
        if (ok) { const float pad = std::max(0.5f, (mx - mn) * 0.15f); lo = std::max(0.f, mn - pad); hi = mx + pad * 0.3f; }
        rows.push_back(LineLeftRight({Bold("延迟"), Muted(" ms  "), Colored("█", colors.accent), Muted(" p50  "), Colored("█", colors.band), Muted(" p99")},
                                     ok ? minMax(mn, mx) : Spans{}, iw));
        GraphData g{p50, p99, lo, hi, colors.accent, colors.band, colors.bad};
        rows.push_back(graph(std::move(g), ok ? FormatAdaptive(hi) : "", ok ? (lo > 0 ? FormatAdaptive(lo) : "0") : "", latencyRows));
    }
    if (twoGraphs && haveGpu) {
        float mn, mx;
        range({&gpu}, mn, mx);
        const float budget = (float)status_.gpuBudgetMs;
        const float hi = std::max(budget > 0 ? budget * 1.15f : 0.f, mx * 1.1f);
        Spans t{Bold("GPU"), Muted(" ms  "), Colored("█", colors.good), Muted(" p50")};
        if (budget > 0) t += {Muted("  ┄ 预算 "), Plain(FormatAdaptive(budget))};
        rows.push_back(LineLeftRight(t, minMax(mn, mx), iw));
        const float budgetLine = budget > 0 ? budget : kUnknownMs;
        GraphData g{gpu, {}, 0, hi, colors.good, colors.band, colors.bad, budgetLine, budgetLine};
        rows.push_back(graph(std::move(g), FormatAdaptive(hi), "0", secondRows));
    } else if (twoGraphs) {
        float mn, mx;
        const bool ok = range({&delay}, mn, mx);
        const float hi = ok ? mx * 1.15f + 0.1f : 1;
        rows.push_back(LineLeftRight({Bold("D"), Muted(" ms")}, ok ? minMax(mn, mx) : Spans{}, iw));
        GraphData g{delay, {}, 0, hi, colors.good, colors.band, colors.bad};
        rows.push_back(graph(std::move(g), ok ? FormatAdaptive(hi) : "", "0", secondRows));
    }
    if (axisRows) rows.push_back(TimeAxis(kGutter, graphWidth, iw));
    return Panel({Bold("历史")}, ClipRows(std::move(rows), ih), w, h);
}

Element Model::TimeAxis(int gutter, int graphWidth, int iw) const {
    const int seconds = (int)std::lround(kHistoryBuckets * kBucketSec);
    const std::string mid = "-" + std::to_string(seconds / 2) + "s", now = "现在";
    std::string line = "-" + std::to_string(seconds) + "s";
    const int midPos = graphWidth / 2 - DisplayWidth(mid) / 2;
    line += std::string((size_t)std::max(1, midPos - DisplayWidth(line)), ' ') + mid;
    line += std::string((size_t)std::max(1, graphWidth - DisplayWidth(line) - DisplayWidth(now)), ' ') + now;
    return Line({Muted(std::string(gutter, ' ') + line)}, iw);
}

Element Model::LogPanel(int w, int h, const std::vector<LogLine>& logs) const {
    const Palette& colors = Colors();
    const int iw = w - kPanelBorderColumns, ih = h - kPanelBorderRows;
    Elements rows;
    for (int i = std::max(0, (int)logs.size() - ih); i < (int)logs.size(); i++) {
        const LogLine& l = logs[i];
        const int tenths = (int)(std::fmod(std::max(0.0, l.t), 1.0) * 10);
        Spans r{Muted(PadLeft(FormatClock(l.t), 5) + std::format(".{} ", tenths))};
        if (l.lvl == LogLevel::Error) r.push_back(Colored(l.text, colors.bad));
        else if (l.lvl == LogLevel::Warn) r.push_back(Colored(l.text, colors.warn));
        else if (l.lvl == LogLevel::Debug) r.push_back(Muted(l.text));
        else r.push_back(Plain(l.text));
        rows.push_back(Line(r, iw));
    }
    return Panel({Bold("日志")}, ClipRows(std::move(rows), ih), w, h);
}

Element Model::Footer(int W, bool quitting) const {
    const Palette& colors = Colors();
    static const char* const kHints[][2] = {{"s", "设置"}, {"m", "静音"}, {"esc", "退出"}};
    Spans r{Plain(" ")};
    if (quitting) { r.push_back(Colored("正在退出…", colors.accent)); return Line(r, W); }
    for (size_t i = 0; i < std::size(kHints); i++) {
        Spans item;
        if (i) item.push_back(Muted(" · "));
        item += {ColoredBold(kHints[i][0], colors.accent), Muted(std::string(" ") + kHints[i][1])};
        if (DisplayWidth(r) + DisplayWidth(item) > W) break;
        r += item;
    }
    return Line(r, W);
}

Element Model::Compact(int W, int h) const {
    Elements rows;
    const auto [total, p99] = HeadlineLatency();
    rows.push_back(Line({Plain(" "), Muted("延迟 "), ColoredBold(FormatAdaptive(total), HeadlineColor(total)), Muted("  p99 "), Plain(FormatAdaptive(p99)),
                         Muted(" ms")}, W));
    rows.push_back(Line({Plain(" "), Muted("GPU "), Plain(FormatAdaptive(windowStats_.gpuP50)), Muted(" / "), Plain(FormatAdaptive(status_.gpuBudgetMs)), Muted(" ms")}, W));
    auto summary = [](bool available, bool on, Spans description) { return !available ? Spans{Plain("-")} : !on ? Spans{Plain("关")} : description; };
    Spans fg{Plain(" "), Muted("补帧 ")}, sr{Plain(" "), Muted("超分 ")};
    fg += summary(status_.fg, status_.fgOn, FgDescription());
    sr += summary(status_.sr, status_.srOn, SrDescription());
    rows.push_back(Line(fg, W));
    rows.push_back(Line(sr, W));
    rows.push_back(Line({Plain(" "), Muted("迟到 "), Plain(std::to_string(windowStats_.late)), Muted("  保持 "), Plain(std::to_string(windowStats_.hold)), Muted("  "),
                         Plain(FormatNumber(windowStats_.fps, 1)), Muted(" fps")}, W));
    while ((int)rows.size() < h) rows.push_back(Line({}, W));
    return ClipRows(std::move(rows), h);
}

Element Model::SettingsPopup(int W, int H, const Popup& popup) const {
    const Palette& colors = Colors();
    constexpr int kMaxWidth = 84, kGroupColumns = 6, kLabelColumns = 12, kChromeRows = 4, kMinHeight = 5;
    const int w = std::min(W, kMaxWidth), iw = w - kPanelBorderColumns;
    Elements body;
    int cursorRow = 0;
    int groups = 0;
    for (int i = 0; i < (int)settings_.size(); i++) groups += i == 0 || settings_[i - 1].group != settings_[i].group;
    const bool gapsBetweenGroups = (int)settings_.size() + groups - 1 + kChromeRows <= H - kPanelBorderRows;
    for (int i = 0; i < (int)settings_.size(); i++) {
        const SettingItem& it = settings_[i];
        const bool firstOfGroup = i == 0 || settings_[i - 1].group != it.group;
        if (firstOfGroup && i && gapsBetweenGroups) body.push_back(Line({}, iw));
        const bool selected = i == popup.cursor, disabled = !it.enabled;
        const int v = popup.Value(it);
        Spans l{firstOfGroup ? ColoredBold(PadRight(it.group, kGroupColumns), colors.accent) : Plain(std::string(kGroupColumns, ' ')),
                selected ? ColoredBold("› ", colors.accent) : Plain("  ")};
        const std::string label = PadRight(it.label, kLabelColumns);
        l.push_back(disabled ? Muted(label) : selected ? Bold(label) : Plain(label));
        std::string note = it.note;
        if (it.kind == SettingItem::Kind::Toggle) {
            l.push_back(disabled ? Muted(v ? "● 开" : "○ 关") : v ? Colored("● 开", colors.good) : Muted("○ 关"));
        } else if (it.kind == SettingItem::Kind::Choice && !it.choices.empty()) {
            const int k = std::clamp(v, 0, (int)it.choices.size() - 1);
            const std::string& c = it.choices[k];
            l.push_back(Muted(k > 0 && !disabled ? "‹ " : "  "));
            if (disabled) l.push_back(Muted(c));
            else if (popup.Waiting(it)) l.push_back(ColoredBold(c, colors.warn));
            else l.push_back(selected ? Bold(c) : Plain(c));
            l.push_back(Muted(k + 1 < (int)it.choices.size() && !disabled ? " ›" : "  "));
            if (k < (int)it.choiceNotes.size() && !it.choiceNotes[k].empty()) note = it.choiceNotes[k];
        } else if (it.kind == SettingItem::Kind::Action) {
            l.push_back(selected ? Colored("⏎ 执行", colors.accent) : Muted("⏎"));
        }
        if (selected) cursorRow = (int)body.size();
        body.push_back(note.empty() ? Line(l, iw) : LineLeftRight(l, {Muted(note)}, iw));
    }
    if (settings_.empty()) body.push_back(Line({Muted("  等待程序发布可调项…")}, iw));
    Spans hint;
    auto key = [&](const char* k, const char* what) {
        if (!hint.empty()) hint.push_back(Muted(" · "));
        hint += {ColoredBold(k, colors.accent), Muted(std::string(" ") + what)};
    };
    key("j/k", "移动"); key("h/l", "改值"); key("空格", "开关"); key("⏎", "执行"); key("s/esc", "关闭");
    const Spans shortHint{ColoredBold("j/k", colors.accent), Muted(" "), ColoredBold("h/l", colors.accent), Muted(" "),
                          ColoredBold("空格", colors.accent), Muted(" "), ColoredBold("s", colors.accent)};
    const int h = std::max(kMinHeight, std::min(H - kPanelBorderRows, (int)body.size() + kChromeRows));
    const int visibleRows = h - kChromeRows;
    const int total = (int)body.size();
    const int first = total > visibleRows ? std::clamp(cursorRow - visibleRows / 2, 0, total - visibleRows) : 0;
    Elements rows(std::make_move_iterator(body.begin() + first), std::make_move_iterator(body.begin() + std::min(total, first + visibleRows)));
    while ((int)rows.size() < visibleRows) rows.push_back(Line({}, iw));
    rows.push_back(Line({Colored(Repeat("─", iw), colors.line)}, iw));
    rows.push_back(FirstFitting({hint, shortHint}, iw));
    Spans title{Bold("设置")};
    if (total > visibleRows) title.push_back(Muted(std::format(" · {}/{}", std::min(total, first + visibleRows), total)));
    return Panel(title, ftxui::vbox(std::move(rows)), w, h) | ftxui::clear_under;
}

Element Model::Build(int W, int H, const std::vector<LogLine>& logs, const Popup& popup, bool quitting) const {
    constexpr int kHeaderRows = 1, kFooterRows = 1;
    constexpr int kMinFullWidth = 44, kMinFullHeight = 9, kRoomyHeight = 30;
    W = std::max(W, 1);
    H = std::max(H, 1);
    Elements rows;
    rows.push_back(Header(W));
    const int avail = std::max(0, H - kHeaderRows - kFooterRows);
    if (W < kMinFullWidth || avail < kMinFullHeight) {
        rows.push_back(Compact(W, avail));
    } else {
        const int used = TopPanels(W, avail, H < kRoomyHeight, rows);
        BottomPanels(W, avail - used, logs, rows);
    }
    rows.push_back(Footer(W, quitting));
    Element root = ftxui::vbox(std::move(rows));
    if (popup.open) root = ftxui::dbox({root, SettingsPopup(W, H, popup) | ftxui::center});
    return root | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, W) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, H);
}

int Model::TopPanels(int W, int avail, bool compact, Elements& rows) const {
    constexpr int kSideBySideWidth = 76;
    const Palette& colors = Colors();
    const Spans settingsTitle{Bold("设置"), Muted(" · "), ColoredBold("s", colors.accent), Muted(" 调整")};
    int used = 0;
    if (W >= kSideBySideWidth) {
        const int lw = W / 2, rw = W - lw;
        Elements lat = LatencyRows(lw - kPanelBorderColumns, compact), set = SettingsRows(rw - kPanelBorderColumns, compact);
        const int topH = std::min(avail, kPanelBorderRows + (int)std::max(lat.size(), set.size()));
        rows.push_back(ftxui::hbox({
            Panel({Bold("延迟")}, ClipRows(std::move(lat), topH - kPanelBorderRows), lw, topH),
            Panel(settingsTitle, ClipRows(std::move(set), topH - kPanelBorderRows), rw, topH),
        }));
        used += topH;
    } else {
        Elements lat = LatencyRows(W - kPanelBorderColumns, compact);
        const int latH = std::min(avail, kPanelBorderRows + (int)lat.size());
        rows.push_back(Panel({Bold("延迟")}, ClipRows(std::move(lat), latH - kPanelBorderRows), W, latH));
        used += latH;
        Elements set = SettingsRows(W - kPanelBorderColumns, false);
        if (avail - used < kPanelBorderRows + (int)set.size()) set = SettingsRows(W - kPanelBorderColumns, true);
        const int setH = kPanelBorderRows + (int)set.size();
        if (avail - used >= setH) {
            rows.push_back(Panel(settingsTitle, ClipRows(std::move(set), setH - kPanelBorderRows), W, setH));
            used += setH;
        }
    }
    return used;
}

void Model::BottomPanels(int W, int rem, const std::vector<LogLine>& logs, Elements& rows) const {
    constexpr int kWideWidth = 100;
    constexpr int kHistoryMinRows = 7, kHistoryMaxRows = kPanelBorderRows + 2 * (1 + 8) + 1, kLogMinRows = 4;
    const bool wide = W >= kWideWidth;
    int histH = 0, logH = 0;
    if (rem >= kHistoryMinRows) {
        if (wide) histH = rem;
        else {
            histH = std::min(rem, kHistoryMaxRows);
            if (rem - histH >= kLogMinRows) logH = rem - histH;
            else histH = rem;
        }
    }
    if (histH && wide) {
        const int hw = (W * 3) / 5;
        rows.push_back(ftxui::hbox({HistoryPanel(hw, histH), LogPanel(W - hw, histH, logs)}));
    } else if (histH) {
        rows.push_back(HistoryPanel(W, histH));
    }
    if (logH) rows.push_back(LogPanel(W, logH, logs));
    const int left = rem - histH - logH;
    if (left > 0) rows.push_back(Blank(W, left));
}

std::string PlainText(const ftxui::Screen& s) {
    std::string out;
    for (int y = 0; y < s.dimy(); y++) {
        std::string line;
        bool coveredByWideGlyph = false;
        for (int x = 0; x < s.dimx(); x++) {
            const auto& px = s.PixelAt(x, y);
            if (coveredByWideGlyph) { coveredByWideGlyph = false; continue; }
            line += px.character.empty() ? std::string(" ") : px.character;
            coveredByWideGlyph = DisplayWidth(px.character) == 2;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    return out;
}

constexpr unsigned kFullWidthFirst = 0xFF01, kFullWidthLast = 0xFF5E, kFullWidthToAscii = 0xFEE0;

char TypedAscii(const ftxui::Event& e) {
    if (!e.is_character()) return 0;
    const std::string& c = e.character();
    if (c.size() == 1) return (unsigned char)c[0] < 0x80 ? c[0] : 0;
    if (c.size() == 3) {
        const unsigned cp = (((unsigned char)c[0] & 0x0F) << 12) | (((unsigned char)c[1] & 0x3F) << 6) | ((unsigned char)c[2] & 0x3F);
        if (cp >= kFullWidthFirst && cp <= kFullWidthLast) return (char)(cp - kFullWidthToAscii);
    }
    return 0;
}

}

struct DashboardState {
    Telemetry* tel = nullptr;
    Model model;

    std::mutex logM;
    std::deque<LogLine> logs;

    std::atomic<bool> stopRequested{false};
    std::mutex screenM;
    ftxui::ScreenInteractive* screen = nullptr;

    bool quitting = false;
    Popup popup;

    std::vector<LogLine> LogSnapshot() {
        std::lock_guard<std::mutex> lk(logM);
        return {logs.begin(), logs.end()};
    }
    void SetScreen(ftxui::ScreenInteractive* s) {
        std::lock_guard<std::mutex> lk(screenM);
        screen = s;
    }
    void PostRefresh() {
        std::lock_guard<std::mutex> lk(screenM);
        if (screen) screen->PostEvent(ftxui::Event::Custom);
    }
    void Update() {
        const int64_t now = Qpc::Now();
        model.Update(tel, now);
        if (model.TakeSettingsChanged()) {
            std::erase_if(popup.pending, [](const auto& p) { return p.second.sent; });
            const int n = (int)model.SettingsList().size();
            popup.cursor = std::clamp(popup.cursor, 0, std::max(0, n - 1));
            if (n && !model.SettingsList()[popup.cursor].enabled) Move(+1);
        }
        FlushPending(now, false);
    }

    void Move(int dir) {
        const auto& items = model.SettingsList();
        for (int k = popup.cursor + dir; k >= 0 && k < (int)items.size(); k += dir)
            if (items[k].enabled) { popup.cursor = k; return; }
    }
    const SettingItem* Current() const {
        const auto& items = model.SettingsList();
        return popup.cursor < (int)items.size() && items[popup.cursor].enabled ? &items[popup.cursor] : nullptr;
    }
    void Set(const SettingItem& it, int v) {
        if (v == popup.Value(it)) return;
        popup.pending[it.id] = {v, Qpc::Now(), false};
        if (!it.pausesPlayback) FlushPending(Qpc::Now(), true);
    }
    void Change(int delta) {
        const SettingItem* it = Current();
        if (!it) return;
        if (it->kind == SettingItem::Kind::Toggle) Set(*it, delta > 0 ? 1 : 0);
        else if (it->kind == SettingItem::Kind::Choice && !it->choices.empty())
            Set(*it, std::clamp(popup.Value(*it) + delta, 0, (int)it->choices.size() - 1));
    }
    void Activate() {
        const SettingItem* it = Current();
        if (!it) return;
        if (it->kind == SettingItem::Kind::Toggle) Set(*it, popup.Value(*it) ? 0 : 1);
        else if (it->kind == SettingItem::Kind::Action) tel->PushSetting({it->id, 0});
        else FlushPending(Qpc::Now(), true);
    }
    void FlushPending(int64_t now, bool force) {
        for (auto& [id, p] : popup.pending) {
            if (p.sent || (!force && Qpc::ToMs(now - p.changedAt) < kPausingChangeDebounceMs)) continue;
            tel->PushSetting({id, p.value});
            p.sent = true;
        }
    }
    void Quit() {
        tel->PushKey(VK_ESCAPE);
        quitting = true;
    }
};

namespace {

bool HandleKey(DashboardState& s, const ftxui::Event& e) {
    using ftxui::Event;
    if (e == Event::CtrlC) { s.Quit(); return true; }
    const char a = (char)tolower((unsigned char)TypedAscii(e));
    if (s.popup.open) {
        if (e == Event::Escape || a == 's') { s.popup.open = false; s.FlushPending(Qpc::Now(), true); }
        else if (a == 'j' || e == Event::ArrowDown) s.Move(+1);
        else if (a == 'k' || e == Event::ArrowUp) s.Move(-1);
        else if (a == 'h' || e == Event::ArrowLeft) s.Change(-1);
        else if (a == 'l' || e == Event::ArrowRight) s.Change(+1);
        else if (a == ' ' || e == Event::Return) s.Activate();
        return true;
    }
    if (e == Event::Escape) { s.Quit(); return true; }
    if (a == 's') { s.popup.open = true; if (!s.Current()) s.Move(+1); return true; }
    if (a == 'm') { s.tel->PushKey('M'); return true; }
    return e.is_character();
}

bool RunTerminalLoop(DashboardState& s) {
    using namespace ftxui;
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);
    screen.ForceHandleCtrlC(false);
    using SignalHandler = void (*)(int);
    const SignalHandler appSigint = std::signal(SIGINT, SIG_DFL);
    if (appSigint != SIG_ERR) std::signal(SIGINT, appSigint);
    bool loopRunning = false;

    auto renderer = Renderer([&] {
        if (!loopRunning) {
            loopRunning = true;
            if (appSigint != SIG_ERR) std::signal(SIGINT, appSigint);
            s.SetScreen(&screen);
        }
        const Dimensions d = Terminal::Size();
        return s.model.Build(d.dimx, d.dimy, s.LogSnapshot(), s.popup, s.quitting);
    });
    auto component = CatchEvent(renderer, [&](Event e) {
        if (s.stopRequested.load()) {
            if (e == Event::Custom) { s.SetScreen(nullptr); screen.Exit(); }
            return true;
        }
        if (e == Event::Custom) { s.Update(); return true; }
        return HandleKey(s, e);
    });
    {
        std::jthread refreshTimer([&s](std::stop_token stop) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            std::mutex m;
            std::condition_variable_any cv;
            std::unique_lock<std::mutex> lk(m);
            while (!cv.wait_for(lk, stop, kRefreshPeriod, [&] { return stop.stop_requested(); })) s.PostRefresh();
        });
        screen.Loop(component);
        s.SetScreen(nullptr);
    }
    return s.stopRequested.load();
}

struct ConsoleModes {
    HANDLE input, output;
    DWORD inputMode = 0, outputMode = 0;
    UINT inputCodePage, outputCodePage;
};

ConsoleModes SaveConsoleModes() {
    ConsoleModes m{GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE)};
    GetConsoleMode(m.input, &m.inputMode);
    GetConsoleMode(m.output, &m.outputMode);
    m.inputCodePage = GetConsoleCP();
    m.outputCodePage = GetConsoleOutputCP();
    return m;
}

void RestoreConsoleModes(const ConsoleModes& m) {
    SetConsoleMode(m.input, m.inputMode | ENABLE_EXTENDED_FLAGS);
    SetConsoleMode(m.output, m.outputMode);
    SetConsoleCP(m.inputCodePage);
    SetConsoleOutputCP(m.outputCodePage);
}

void UiThreadMain(DashboardState& s) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"CameraVFX ui");
    const ConsoleModes saved = SaveConsoleModes();
    const DWORD ctrlCAsKeyNoQuickEdit = (saved.inputMode | ENABLE_EXTENDED_FLAGS) & ~(DWORD)(ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    SetConsoleMode(saved.input, ctrlCAsKeyNoQuickEdit);
    const bool stoppedByApp = RunTerminalLoop(s);
    RestoreConsoleModes(saved);
    if (!stoppedByApp) s.tel->PushKey(VK_ESCAPE);
}

void ForwardLogToDashboard(void* dashboard, LogLevel lvl, double tSec, const char* msg, size_t len) {
    static_cast<Dashboard*>(dashboard)->OnLog(lvl, tSec, msg, len);
}

}

namespace {
constexpr char kLeaveFullscreenSequence[] = "[?1049l[?25h[0m";
std::atomic<bool> g_fullscreenActive{false};
}

void RestoreConsoleAfterCrash() {
    if (!g_fullscreenActive.load()) return;
    fputs(kLeaveFullscreenSequence, stdout);
    fflush(stdout);
}

Dashboard::Dashboard() : state_(std::make_unique<DashboardState>()) {}
Dashboard::~Dashboard() { Stop(); }

void Dashboard::Start(Telemetry* tel) {
    state_->tel = tel;
    state_->model.SetEighthBlocks(TerminalHasEighthBlocks());
    LogFlush();
    SetLogSink(ForwardLogToDashboard, this);
    SetLogConsole(false);
    g_fullscreenActive.store(true);
    uiThread_ = std::thread(UiThreadMain, std::ref(*state_));
}

void Dashboard::Stop() {
    if (!uiThread_.joinable()) return;
    state_->stopRequested.store(true);
    state_->PostRefresh();
    uiThread_.join();
    g_fullscreenActive.store(false);
    SetLogConsole(true);
    SetLogSink(nullptr, nullptr);
}

void Dashboard::OnLog(LogLevel lvl, double tSec, const char* msg, size_t len) {
    std::vector<std::string> lines;
    for (size_t a = 0; a < len;) {
        size_t b = a;
        while (b < len && msg[b] != '\n') b++;
        std::string t(msg + a, b - a);
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        std::replace_if(t.begin(), t.end(), [](char c) { return c == '\r' || c == '\t'; }, ' ');
        if (!t.empty()) lines.push_back(std::move(t));
        a = b + 1;
    }
    std::lock_guard<std::mutex> lk(state_->logM);
    for (auto& t : lines) state_->logs.push_back({lvl, tSec, std::move(t)});
    while (state_->logs.size() > kMaxLogLines) state_->logs.pop_front();
}

void Dashboard::Pump(Telemetry* tel, int64_t nowQpc) {
    state_->tel = tel;
    state_->model.Update(tel, nowQpc);
}

std::string Dashboard::RenderText(int width, int height, bool ansi, bool popupOpen) const {
    width = std::max(width, 1);
    height = std::max(height, 1);
    Popup popup;
    popup.open = popupOpen;
    const auto& items = state_->model.SettingsList();
    while (popup.cursor < (int)items.size() && !items[popup.cursor].enabled) popup.cursor++;
    Element e = state_->model.Build(width, height, state_->LogSnapshot(), popup, false);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, e);
    return ansi ? screen.ToString() + "\n" : PlainText(screen);
}

}
