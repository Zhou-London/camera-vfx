#include "ui/dashboard.h"
#include "util/common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace cameravfx;

namespace {

const char* const kFgQualities[] = {"low", "medium", "high"};
const double kFgCostMs[] = {1.1, 2.5, 6.9};
const char* const kSrQualities[] = {"hb-medium", "hb-high", "hb-ultra", "streaming-ultra"};
const double kSrCostMs[] = {0.6, 0.9, 1.6, 3.6};
constexpr double kOtherGpuMs = 0.3;

struct SimApp {
    bool fgOn = true, srOn = true, muted = false, timecode = false;
    int fgQuality = 0, srQuality = 2;
    float volume = 0.8f;
    double contentFps = 60, signalHz = 59.94, dispHz = 120;
    uint64_t statsEpoch = 1;
    bool warm = false;
    std::string loading;
    double loadingUntil = -1;
    uint64_t late = 0, drops = 0, missed = 0;
    double uptime = 0;
    std::mt19937 rng{12345};
    std::normal_distribution<double> n01{0.0, 1.0};
    std::uniform_real_distribution<double> u01{0.0, 1.0};
    double lateBurstUntil = -1;
    int format = 1, captureMode = 0, matrix = 0, range = 0, fgMultiplierChoice = 1, volumeStep = 8;

    bool Loading(double t) {
        if (!loading.empty() && t >= loadingUntil) loading.clear();
        return !loading.empty();
    }
    double OutFps() const { return fgOn ? std::min(dispHz, contentFps * 2) : contentFps; }
    double GpuMs() const { return (fgOn ? kFgCostMs[fgQuality] : 0) + (srOn ? kSrCostMs[srQuality] : 0) + kOtherGpuMs; }

    FrameSample Frame(int64_t qpc, double tSec) {
        FrameSample f;
        f.qpc = qpc;
        const double period = 1000.0 / contentFps;
        const double base = fgOn ? period / 2 + 3.2 + GpuMs() * 0.45 : 0.35 + GpuMs() * 0.3;
        double lat = base + n01(rng) * 0.3;
        if (u01(rng) < 0.006) lat += 2 + 3 * u01(rng);
        f.latencyMs = (float)std::max(0.1, lat);
        f.contentMs = f.latencyMs + 9.8f + (float)(n01(rng) * 0.4);
        f.delayMs = (float)((fgOn ? 4.1 : 0.6) + n01(rng) * 0.15);
        f.gpuMs = (float)(GpuMs() * (1 + n01(rng) * 0.04));
        const bool burst = tSec < lateBurstUntil;
        if (u01(rng) < (burst ? 0.05 : 0.0008)) { f.flags |= FrameSample::kLate; late++; }
        if (contentFps < 45 && u01(rng) < 0.03) f.flags |= FrameSample::kHold;
        return f;
    }

    AppStatus Status(double outFpsMeasured) const {
        AppStatus s;
        s.statsEpoch = statsEpoch;
        s.uptimeSec = uptime;
        s.warmupDone = warm;
        s.configPath = "C:\\Users\\me\\CameraVFX\\CameraVFX.ini";
        s.captureName = "UGREEN 25173";
        s.captureFormat = "YUY2 1920x1080@60";
        s.sourceSize = {1920, 1080};
        s.outputSize = srOn ? FrameSize{3840, 2160} : FrameSize{1920, 1080};
        s.displaySize = {3840, 2160};
        s.displayHz = dispHz;
        s.vrrActive = true;
        s.mode = "vrr";
        s.fg = true; s.fgQuality = kFgQualities[fgQuality];
        s.sr = true; s.srQuality = kSrQualities[srQuality];
        s.hdr = true;
        s.fgOn = fgOn; s.srOn = srOn;
        s.timecode = timecode;
        s.loading = loading;
        s.fgLoading = loading.starts_with("补帧");
        s.srLoading = loading.starts_with("超分");
        s.fgActive = fgOn;
        s.detectRepeats = true;
        s.fgMultiplier = contentFps >= 58 ? 1 : 2;
        s.fgMultiplierRequested = 2;
        s.fgThresholdFps = 60;
        s.fgMultiplierLimit = contentFps >= 58 ? MultiplierLimit::Threshold : MultiplierLimit::None;
        s.signalHz = signalHz; s.contentFps = contentFps; s.outFps = outFpsMeasured;
        s.leadMs = 3.0;
        s.gpuBudgetMs = 1000.0 / OutFps();
        s.late = late; s.drops = drops; s.missed = missed;
        s.audioEnabled = true; s.audioRunning = true; s.audioMuted = muted;
        s.audioVolume = volume; s.audioLatencyMs = 45; s.audioSyncMs = 3;
        s.audioIn = "HDMI (UGREEN 25173)"; s.audioOut = "LG TV (HDMI)";
        s.stages = {9.8, 0.21, fgOn ? kFgCostMs[fgQuality] : kUnknownMs, srOn ? kSrCostMs[srQuality] : kUnknownMs, kOtherGpuMs};
        if (GpuMs() > s.gpuBudgetMs) s.notice = "GPU 耗时超过帧预算，正在跳帧：降低补帧或超分档位";
        return s;
    }

    std::vector<SettingItem> Items() const {
        using Kind = SettingItem::Kind;
        std::vector<SettingItem> v;
        auto add = [&](SettingId id, Kind k, const char* group, const char* label, std::vector<std::string> choices, int value,
                       bool pausesPlayback = false, std::vector<std::string> choiceNotes = {}, std::string note = {}) {
            SettingItem it;
            it.id = id; it.kind = k; it.group = group; it.label = label; it.choices = std::move(choices); it.value = value;
            it.pausesPlayback = pausesPlayback; it.choiceNotes = std::move(choiceNotes); it.note = std::move(note);
            v.push_back(std::move(it));
        };
        add(SettingId::CapFormat, Kind::Choice, "采集", "格式", {"NV12 4:2:0", "YUY2 4:2:2", "RGB24 4:4:4", "P010 10-bit"}, format, true,
            {"交付≈7.5 ms", "交付 9.9 ms（实测）", "交付≈15.0 ms", "交付≈15.0 ms"});
        add(SettingId::CapMode, Kind::Choice, "采集", "分辨率", {"1920×1080 @ 60", "1920×1080 @ 50", "1280×720 @ 60", "640×480 @ 60"}, captureMode,
            true, {}, "重建处理链");
        add(SettingId::Matrix, Kind::Choice, "采集", "矩阵", {"自动（BT.709）", "BT.709", "BT.601", "BT.2020"}, matrix);
        add(SettingId::Range, Kind::Choice, "采集", "范围", {"自动（有限）", "有限 16–235", "全 0–255"}, range);
        add(SettingId::Fg, Kind::Toggle, "补帧", "开启", {}, fgOn);
        add(SettingId::FgQuality, Kind::Choice, "补帧", "档位", {"low", "medium", "high"}, fgQuality, true, {}, "加载约 0.2 s");
        add(SettingId::FgMultiplier, Kind::Choice, "补帧", "倍数", {"×1", "×2"}, fgMultiplierChoice, false, {"输出 60 fps", "输出 120 fps"});
        add(SettingId::Sr, Kind::Toggle, "超分", "开启", {}, srOn);
        add(SettingId::SrQuality, Kind::Choice, "超分", "档位", {kSrQualities[0], kSrQualities[1], kSrQualities[2], kSrQualities[3]}, srQuality, true,
            {}, "加载约 1.5 s");
        add(SettingId::Mute, Kind::Toggle, "声音", "静音", {}, muted);
        std::vector<std::string> volumes;
        for (int i = 0; i <= 20; i++) volumes.push_back(std::to_string(i * 10) + "%");
        add(SettingId::AudioVolume, Kind::Choice, "声音", "音量", volumes, volumeStep);
        add(SettingId::Timecode, Kind::Toggle, "其他", "时间码", {}, timecode, false, {}, "回环测延迟用");
        add(SettingId::PrintStats, Kind::Action, "其他", "打印统计", {}, 0, false, {}, "详细分段耗时写进日志");
        add(SettingId::ResetStats, Kind::Action, "其他", "重置统计", {}, 0);
        add(SettingId::Save, Kind::Action, "其他", "保存为默认", {}, 0, false, {}, "写入 CameraVFX.ini");
        return v;
    }

    std::string SetFg(bool on) { fgOn = on; statsEpoch++; return std::string("frame generation ") + (fgOn ? "on" : "off"); }
    std::string SetSr(bool on) { srOn = on; statsEpoch++; return std::string("super resolution ") + (srOn ? "on" : "off"); }
    std::string SetFgQuality(int q) {
        fgQuality = q;
        statsEpoch++;
        loading = std::string("补帧 ") + kFgQualities[fgQuality];
        loadingUntil = uptime + 1.2;
        return std::string("fg quality ") + kFgQualities[fgQuality] + ": loading";
    }
    std::string SetSrQuality(int q) {
        srQuality = q;
        statsEpoch++;
        loading = std::string("超分 ") + kSrQualities[srQuality];
        loadingUntil = uptime + 1.6;
        return std::string("sr quality ") + kSrQualities[srQuality] + ": loading";
    }
    std::string ToggleMute() { muted = !muted; return muted ? "audio muted" : "audio unmuted"; }

    std::string Apply(const SettingCmd& c) {
        switch (c.id) {
        case SettingId::CapFormat: format = c.value; loading = "采集格式"; loadingUntil = uptime + 0.9; return "capture format changed";
        case SettingId::CapMode: captureMode = c.value; loading = "采集模式"; loadingUntil = uptime + 2.0; return "capture mode changed";
        case SettingId::Matrix: matrix = c.value; return "colour matrix changed";
        case SettingId::Range: range = c.value; return "colour range changed";
        case SettingId::Fg: return SetFg(c.value != 0);
        case SettingId::FgQuality: return SetFgQuality(c.value);
        case SettingId::FgMultiplier: fgMultiplierChoice = c.value; statsEpoch++; return "multiplier changed";
        case SettingId::Sr: return SetSr(c.value != 0);
        case SettingId::SrQuality: return SetSrQuality(c.value);
        case SettingId::Mute: muted = c.value != 0; return muted ? "audio muted" : "audio unmuted";
        case SettingId::AudioVolume: volumeStep = c.value; volume = (float)c.value * 0.1f; return "volume " + std::to_string(c.value * 10) + "%";
        case SettingId::Timecode: timecode = c.value != 0; return std::string("timecode ") + (timecode ? "on" : "off");
        case SettingId::PrintStats: return "stats: (detail printed by the app)";
        case SettingId::ResetStats: statsEpoch++; return "stats reset";
        case SettingId::Save: return "settings saved to CameraVFX.ini";
        default: return {};
        }
    }
};

enum class ScriptChange { None, NextFgQuality, NextSrQuality };
struct ScriptEvent { double t; ScriptChange change; double contentFps; double lateBurstSec; };
const ScriptEvent kScript[] = {
    {25, ScriptChange::NextFgQuality, 0, 0},
    {38, ScriptChange::None, 0, 2.0},
    {45, ScriptChange::None, 30, 0},
    {62, ScriptChange::None, 60, 0},
    {64, ScriptChange::NextSrQuality, 0, 0},
    {80, ScriptChange::NextFgQuality, 0, 0},
};

std::string ApplyScriptChange(SimApp& app, ScriptChange change) {
    switch (change) {
    case ScriptChange::NextFgQuality: return app.SetFgQuality((app.fgQuality + 1) % (int)std::size(kFgQualities));
    case ScriptChange::NextSrQuality: return app.SetSrQuality((app.srQuality + 1) % (int)std::size(kSrQualities));
    case ScriptChange::None: return {};
    }
    return {};
}

std::vector<std::pair<int, int>> ParseSizes(const char* s) {
    std::vector<std::pair<int, int>> r;
    const std::string str(s);
    for (size_t pos = 0; pos <= str.size();) {
        size_t e = str.find(',', pos);
        if (e == std::string::npos) e = str.size();
        int w = 0, h = 0;
        if (sscanf(str.substr(pos, e - pos).c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) r.push_back({w, h});
        pos = e + 1;
    }
    return r;
}

void Log(Dashboard& d, LogLevel lvl, double t, const std::string& s) { d.OnLog(lvl, t, s.data(), s.size()); }

double SimulatedSecondsFor(long frames, double step) {
    SimApp a;
    double produced = 0, t = 0;
    size_t ev = 0;
    while (produced < frames && t < 3600) {
        for (; ev < std::size(kScript) && kScript[ev].t <= t; ev++) {
            if (kScript[ev].contentFps > 0) a.contentFps = kScript[ev].contentFps;
            ApplyScriptChange(a, kScript[ev].change);
        }
        produced += a.OutFps() * step;
        t += step;
    }
    return t;
}

int RunRender(const std::vector<std::pair<int, int>>& sizes, long frames, bool ansi, bool popupOpen) {
    constexpr double kStep = 0.1;
    constexpr double kWarmupSec = 2.0;
    const double duration = SimulatedSecondsFor(frames, kStep);
    Telemetry tel;
    Dashboard dash;
    SimApp app;
    const int64_t f = Qpc::Freq();
    const int64_t q0 = Qpc::Now() - (int64_t)(duration * (double)f);
    double t = 0, nextFrame = 0;
    size_t ev = 0;
    long produced = 0;
    Log(dash, LogLevel::Info, 0.0, "CameraVFX 0.9 · config C:\\Users\\me\\CameraVFX\\CameraVFX.ini");
    Log(dash, LogLevel::Info, 0.1, "capture: UGREEN 25173 YUY2 1920x1080@60 (frame server)");
    Log(dash, LogLevel::Info, 0.4, "display: LG TV 3840x2160 @ 120 Hz, VRR on");
    Log(dash, LogLevel::Info, 1.2, "vfx: frame generation low, super resolution hb-ultra, TrueHDR");
    while (t < duration) {
        if (!app.warm && t >= kWarmupSec) { app.warm = true; Log(dash, LogLevel::Info, t, "warm-up done, pacing locked (D 4.1 ms)"); }
        for (; ev < std::size(kScript) && kScript[ev].t <= t; ev++) {
            const ScriptEvent& e = kScript[ev];
            if (e.contentFps > 0) {
                app.contentFps = e.contentFps;
                Log(dash, LogLevel::Info, t, "content cadence " + std::to_string((int)e.contentFps) + " fps");
            }
            if (e.change != ScriptChange::None) Log(dash, LogLevel::Info, t, ApplyScriptChange(app, e.change));
            if (e.lateBurstSec > 0) { app.lateBurstUntil = t + e.lateBurstSec; Log(dash, LogLevel::Warn, t, "late frames: present missed the deadline (GPU busy)"); }
            if (app.GpuMs() > 1000.0 / app.OutFps()) Log(dash, LogLevel::Warn, t, "GPU time over budget, frames will be skipped");
        }
        const double tEnd = t + kStep;
        for (; nextFrame < tEnd; nextFrame += 1.0 / app.OutFps()) {
            const int64_t q = q0 + (int64_t)(nextFrame * (double)f);
            if (app.warm && !app.Loading(nextFrame)) { tel.PushFrame(app.Frame(q, nextFrame)); produced++; }
        }
        t = tEnd;
        app.uptime = t;
        tel.PublishStatus(app.Status(app.OutFps()));
        tel.PublishSettings(app.Items());
        dash.Pump(&tel, q0 + (int64_t)(t * (double)f));
    }
    Log(dash, LogLevel::Error, t, "example error line: audio device lost, retrying");
    dash.Pump(&tel, q0 + (int64_t)(t * (double)f));
    for (const auto& [w, h] : sizes) {
        printf("---- %dx%d (simulated %.1f s, %ld frames)\n", w, h, duration, produced);
        const std::string s = dash.RenderText(w, h, ansi, popupOpen);
        fwrite(s.data(), 1, s.size(), stdout);
    }
    return 0;
}

struct LiveSim {
    Telemetry* tel;
    double hz;
    std::atomic<bool> stop{false}, quit{false};
    std::thread th;
    void Run() {
        constexpr double kWarmupSec = 1.5;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        SimApp app;
        app.dispHz = hz;
        PreciseSleeper sleeper;
        const int64_t t0 = Qpc::Now();
        int64_t next = t0, nextStatus = t0, nextLog = t0 + Qpc::FromMs(3000);
        LOGI("tui_demo live: simulated capture → vfx → display");
        tel->PublishSettings(app.Items());
        while (!stop.load()) {
            const int64_t now = Qpc::Now();
            app.uptime = Qpc::ToSec(now - t0);
            if (!app.warm && app.uptime > kWarmupSec) { app.warm = true; LOGI("warm-up done"); }
            for (int k; (k = tel->PopKey()) != 0;) {
                if (k == VK_ESCAPE) { quit = true; LOGI("quit requested"); }
                else if (k == 'M') LOGI("%s", app.ToggleMute().c_str());
            }
            for (SettingCmd c; tel->PopSetting(c);) {
                const std::string l = app.Apply(c);
                if (!l.empty()) LOGI("%s", l.c_str());
                if (app.GpuMs() > 1000.0 / app.OutFps()) LOGW("GPU time over budget");
                tel->PublishSettings(app.Items());
            }
            if (now >= nextStatus) {
                tel->PublishStatus(app.Status(app.OutFps()));
                nextStatus += Qpc::FromMs(100);
            }
            if (now >= nextLog) {
                LOGI("pacing: D 4.1 ms, slack 1.1 ms, drift +3 ppm");
                nextLog += Qpc::FromMs(4000);
            }
            if (app.warm && !app.Loading(app.uptime)) tel->PushFrame(app.Frame(now, app.uptime));
            next = std::max(next + Qpc::FromMs(1000.0 / app.OutFps()), Qpc::Now());
            sleeper.SleepUntil(next);
        }
    }
};

std::vector<std::pair<double, std::string>> ParseKeys(const std::string& spec) {
    std::vector<std::pair<double, std::string>> r;
    for (size_t pos = 0; pos < spec.size();) {
        size_t e = spec.find(',', pos);
        if (e == std::string::npos) e = spec.size();
        const std::string item = spec.substr(pos, e - pos);
        const size_t c = item.find(':');
        if (c != std::string::npos) r.push_back({atof(item.substr(0, c).c_str()), item.substr(c + 1)});
        pos = e + 1;
    }
    return r;
}

void InjectKey(const std::string& k) {
    WORD vk = 0;
    wchar_t ch = 0;
    DWORD ctrl = 0;
    if (k == "esc") { vk = VK_ESCAPE; ch = 0x1b; }
    else if (k == "ctrl-c") { vk = 'C'; ch = 0x03; ctrl = LEFT_CTRL_PRESSED; }
    else if (!k.empty()) { ch = (wchar_t)k[0]; vk = (WORD)toupper((unsigned char)k[0]); }
    INPUT_RECORD rec[2] = {};
    for (int i = 0; i < 2; i++) {
        rec[i].EventType = KEY_EVENT;
        rec[i].Event.KeyEvent.bKeyDown = i == 0;
        rec[i].Event.KeyEvent.wRepeatCount = 1;
        rec[i].Event.KeyEvent.wVirtualKeyCode = vk;
        rec[i].Event.KeyEvent.uChar.UnicodeChar = ch;
        rec[i].Event.KeyEvent.dwControlKeyState = ctrl;
    }
    DWORD n = 0;
    WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), rec, 2, &n);
}

int RunLive(double seconds, double hz, const std::string& keys, const std::string& outPath) {
    Telemetry tel;
    Dashboard dash;
    LiveSim sim;
    sim.tel = &tel;
    sim.hz = hz;
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE), hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD inBefore = 0, outBefore = 0, inDuring = 0, inAfter = 0, outAfter = 0;
    GetConsoleMode(hin, &inBefore);
    GetConsoleMode(hout, &outBefore);
    const UINT cpBefore = GetConsoleOutputCP();
    dash.Start(&tel);
    sim.th = std::thread([&] { sim.Run(); });
    const int64_t t0 = Qpc::Now(), end = t0 + Qpc::FromMs(seconds * 1000);
    const auto script = ParseKeys(keys);
    size_t nextKey = 0;
    while (!sim.quit.load() && (seconds <= 0 || Qpc::Now() < end)) {
        const double elapsed = Qpc::ToSec(Qpc::Now() - t0);
        while (nextKey < script.size() && elapsed >= script[nextKey].first) InjectKey(script[nextKey++].second);
        if (!inDuring && elapsed > 1.0) GetConsoleMode(hin, &inDuring);
        Sleep(20);
    }
    const bool quitByKey = sim.quit.load();
    const int64_t s0 = Qpc::Now();
    dash.Stop();
    const double stopMs = Qpc::ToMs(Qpc::Now() - s0);
    sim.stop = true;
    sim.th.join();
    GetConsoleMode(hin, &inAfter);
    GetConsoleMode(hout, &outAfter);
    const bool restored = (inBefore | ENABLE_EXTENDED_FLAGS) == (inAfter | ENABLE_EXTENDED_FLAGS) && outBefore == outAfter;
    char report[512];
    snprintf(report, sizeof(report),
             "console: in 0x%lX -> (tui 0x%lX) -> 0x%lX, out 0x%lX -> 0x%lX, cp %u -> %u (%s)\n"
             "quit by key: %s; Stop() took %.1f ms\n",
             inBefore, inDuring, inAfter, outBefore, outAfter, cpBefore, GetConsoleOutputCP(), restored ? "restored" : "NOT restored",
             quitByKey ? "yes" : "no", stopMs);
    fputs(report, stdout);
    if (!outPath.empty()) {
        if (FILE* f = OpenUtf8(outPath, L"wb")) { fputs(report, f); fclose(f); }
    }
    return 0;
}

}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::pair<int, int>> sizes;
    long frames = 10000;
    int ansi = -1;
    std::string out, keys;
    bool live = false, popupOpen = false;
    double seconds = -1, hz = 120;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--render") sizes = ParseSizes(next());
        else if (a == "--frames") frames = atol(next());
        else if (a == "--plain") ansi = 0;
        else if (a == "--ansi") ansi = 1;
        else if (a == "--popup") popupOpen = true;
        else if (a == "--live") live = true;
        else if (a == "--seconds") seconds = atof(next());
        else if (a == "--hz") hz = atof(next());
        else if (a == "--out") out = next();
        else if (a == "--keys") keys = next();
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (live) return RunLive(std::max(seconds, 0.0), hz, keys, out);
    if (!sizes.empty()) {
        if (ansi < 0) { DWORD m; ansi = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m) ? 1 : 0; }
        return RunRender(sizes, frames, ansi > 0, popupOpen);
    }
    fprintf(stderr, "usage: tui_demo --render 120x36[,80x24] [--frames N] [--plain|--ansi] [--popup]\n"
                    "       tui_demo --live [--seconds N] [--hz 120] [--keys 3:s,5:m,9:esc] [--out report.txt]\n");
    return 2;
}
