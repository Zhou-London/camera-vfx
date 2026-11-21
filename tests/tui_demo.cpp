// tui_demo: the terminal dashboard with simulated telemetry (no GPU, no capture card).
//
//   tui_demo --render WxH[,WxH...] [--frames N] [--plain|--ansi] [--history file.csv] [--help-overlay]
//       Feeds a simulated session (warm-up, settings changes, a 30 fps content period, holds, late
//       frames, log lines) into the model and prints one off-screen render per size.
//   tui_demo --live [--seconds N] [--hz 60|120] [--history file.csv]
//       Runs the real full-screen dashboard; a simulated app thread produces frames and status and
//       reacts to the forwarded keys (i/f/s/q/m/t/+/-/esc).
//   tui_demo --bench [--seconds N] [--out file.txt] [--no-tui]
//       Producer-side cost of Telemetry::PushFrame / PublishStatus with the dashboard in record-only
//       mode and with the TUI running, plus the UI thread's cost per refresh.
#include "ui/dashboard.h"
#include "util/common.h"

#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace camss;

namespace {

// ------------------------------------------------------------------ simulated app
const char* const kFgQ[] = {"low", "medium", "high"};
const double kFgCost[] = {1.1, 2.5, 6.9};
const char* const kSrQ[] = {"hb-medium", "hb-high", "hb-ultra", "streaming-ultra"};
const double kSrCost[] = {0.6, 0.9, 1.6, 3.6};

struct SimApp {
    // settings
    bool interpOn = true, srOn = true, muted = false, timecode = false;
    int fgq = 0, srq = 2;
    float volume = 0.8f;
    double contentFps = 60, signalHz = 59.94, dispHz = 120;
    uint64_t generation = 1;
    bool warm = false;
    // counters
    uint64_t late = 0, hold = 0, drops = 0, missed = 0, repeats = 0;
    double uptime = 0;
    std::mt19937 rng{12345};
    std::normal_distribution<double> n01{0.0, 1.0};
    std::uniform_real_distribution<double> u01{0.0, 1.0};
    double lateBurstUntil = -1;

    double OutFps() const { return interpOn ? std::min(dispHz, contentFps * 2) : contentFps; }
    double Gpu() const { return (interpOn ? kFgCost[fgq] : 0) + (srOn ? kSrCost[srq] : 0) + 0.3; }

    FrameSample Frame(int64_t qpc, double tSec) {
        FrameSample f;
        f.qpc = qpc;
        const double period = 1000.0 / contentFps;
        const double base = interpOn ? period / 2 + 3.2 + Gpu() * 0.45 : 0.35 + Gpu() * 0.3;
        double lat = base + n01(rng) * 0.3;
        if (u01(rng) < 0.006) lat += 2 + 3 * u01(rng);
        f.latencyMs = (float)std::max(0.1, lat);
        f.contentMs = f.latencyMs + 9.8f + (float)(n01(rng) * 0.4);
        f.delayMs = (float)((interpOn ? 4.1 : 0.6) + n01(rng) * 0.15);
        f.gpuMs = (float)(Gpu() * (1 + n01(rng) * 0.04));
        f.t = 0.5f;
        if (interpOn) f.flags |= FrameSample::kInterp;
        const bool burst = tSec < lateBurstUntil;
        if (u01(rng) < (burst ? 0.05 : 0.0008)) { f.flags |= FrameSample::kLate; late++; }
        if (contentFps < 45 && u01(rng) < 0.03) { f.flags |= FrameSample::kHold; hold++; repeats++; }
        return f;
    }

    AppStatus Status(double outFpsMeasured) const {
        AppStatus s;
        s.generation = generation;
        s.uptimeSec = uptime;
        s.warmupDone = warm;
        s.configPath = "C:\\Users\\me\\camss\\camss.ini";
        s.captureName = "UGREEN 25173";
        s.captureFormat = "YUY2 1920x1080@60";
        s.srcW = 1920; s.srcH = 1080;
        s.outW = srOn ? 3840 : 1920; s.outH = srOn ? 2160 : 1080;
        s.dispW = 3840; s.dispH = 2160; s.dispHz = dispHz;
        s.vrrActive = true;
        s.mode = "vrr";
        s.interp = "vfx"; s.fgQuality = kFgQ[fgq];
        s.sr = "vfx"; s.srQuality = kSrQ[srq];
        s.hdr = "vfx";
        s.interpOn = interpOn; s.srOn = srOn;
        s.timecode = timecode;
        s.adaptive = true;
        s.multiplier = 2;
        s.targetFps = 0;
        s.signalHz = signalHz; s.contentFps = contentFps; s.outFps = outFpsMeasured;
        s.delayMs = interpOn ? 4.1 : 0.6; s.leadMs = 3.0; s.minSlackMs = 1.1;
        s.gpuBudgetMs = 1000.0 / OutFps();
        s.latencyMetric = "arrival→flip";
        s.late = late; s.hold = hold; s.drops = drops; s.missed = missed; s.repeats = repeats;
        s.audioEnabled = true; s.audioRunning = true; s.audioPlaying = true; s.audioMuted = muted;
        s.audioVolume = volume; s.audioLatencyMs = 45; s.audioBufferMs = 20; s.audioSyncMs = 3;
        s.audioUnderruns = 0;
        s.audioIn = "HDMI (UGREEN 25173)"; s.audioOut = "LG TV (HDMI)";
        s.captureMs = 9.8; s.uploadMs = 0.21;
        s.fgMs = interpOn ? kFgCost[fgq] : -1; s.srMs = srOn ? kSrCost[srq] : -1; s.hdrMs = 0.3;
        if (Gpu() > s.gpuBudgetMs) s.notice = "GPU 耗时超过帧预算，正在跳帧：降低补帧或超分档位";
        return s;
    }

    // Applies a key like the real app; returns a log line ("" = none).
    std::string Key(int vk) {
        switch (vk) {
        case 'I': interpOn = !interpOn; generation++; return std::string("frame generation ") + (interpOn ? "on" : "off");
        case 'S': srOn = !srOn; generation++; return std::string("super resolution ") + (srOn ? "on" : "off");
        case 'F': fgq = (fgq + 1) % 3; generation++; return std::string("fg quality ") + kFgQ[fgq];
        case 'Q': srq = (srq + 1) % 4; generation++; return std::string("sr quality ") + kSrQ[srq];
        case 'M': muted = !muted; return muted ? "audio muted" : "audio unmuted";
        case VK_OEM_PLUS: volume = std::min(1.0f, volume + 0.05f); return "volume " + std::to_string((int)std::lround(volume * 100)) + "%";
        case VK_OEM_MINUS: volume = std::max(0.0f, volume - 0.05f); return "volume " + std::to_string((int)std::lround(volume * 100)) + "%";
        case 'L': return "stats: (detail printed by the app)";
        case 'R': return "stats reset";
        case 'T': timecode = !timecode; return std::string("timecode ") + (timecode ? "on" : "off");
        }
        return {};
    }
};

// Scripted session for --render (simulated seconds).
struct Event { double t; int key; double contentFps; double lateBurst; };
const Event kScript[] = {
    {25, 'F', 0, 0},        // fg quality low -> medium
    {38, 0, 0, 2.0},        // a burst of late frames
    {45, 0, 30, 0},         // content drops to 30 fps (game menu)
    {62, 0, 60, 0},         // back to 60
    {64, 'Q', 0, 0},        // sr quality hb-ultra -> streaming-ultra
    {80, 'F', 0, 0},        // fg medium -> high: GPU over budget
};

std::vector<std::pair<int, int>> ParseSizes(const char* s) {
    std::vector<std::pair<int, int>> r;
    std::string str(s);
    size_t pos = 0;
    while (pos <= str.size()) {
        size_t e = str.find(',', pos);
        if (e == std::string::npos) e = str.size();
        int w = 0, h = 0;
        if (sscanf(str.substr(pos, e - pos).c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) r.push_back({w, h});
        pos = e + 1;
    }
    return r;
}

void Log(Dashboard& d, LogLevel lvl, double t, const std::string& s) { d.OnLog(lvl, t, s.data(), s.size()); }

int RunRender(const std::vector<std::pair<int, int>>& sizes, long frames, int ansiMode, const std::string& history, bool help) {
    const double dt = 0.1;
    // dry run: simulated duration for `frames` output frames (rates follow the script)
    double dur = 0;
    {
        SimApp a; double cnt = 0; size_t ev = 0;
        while (cnt < frames && dur < 3600) {
            while (ev < std::size(kScript) && kScript[ev].t <= dur) {
                if (kScript[ev].contentFps > 0) a.contentFps = kScript[ev].contentFps;
                if (kScript[ev].key) a.Key(kScript[ev].key);
                ev++;
            }
            cnt += a.OutFps() * dt; dur += dt;
        }
    }
    Telemetry tel;
    Dashboard dash;
    DashboardOptions o; o.tui = false; o.historyPath = history;
    SimApp app;
    const int64_t f = Qpc::Freq();
    const int64_t q0 = Qpc::Now() - (int64_t)(dur * (double)f);
    double t = 0, nextFrame = 0;
    size_t ev = 0;
    long produced = 0;
    Log(dash, LogLevel::Info, 0.0, "camss 0.9 · config C:\\Users\\me\\camss\\camss.ini");
    Log(dash, LogLevel::Info, 0.1, "capture: UGREEN 25173 YUY2 1920x1080@60 (frame server)");
    Log(dash, LogLevel::Info, 0.4, "display: LG TV 3840x2160 @ 120 Hz, VRR on");
    Log(dash, LogLevel::Info, 1.2, "vfx: frame generation low, super resolution hb-ultra, TrueHDR");
    while (t < dur) {
        if (!app.warm && t >= 2.0) { app.warm = true; Log(dash, LogLevel::Info, t, "warm-up done, pacing locked (D 4.1 ms)"); }
        while (ev < std::size(kScript) && kScript[ev].t <= t) {
            const Event& e = kScript[ev++];
            if (e.contentFps > 0) {
                app.contentFps = e.contentFps;
                Log(dash, LogLevel::Info, t, "content cadence " + std::to_string((int)e.contentFps) + " fps");
            }
            if (e.key) Log(dash, LogLevel::Info, t, app.Key(e.key));
            if (e.lateBurst > 0) { app.lateBurstUntil = t + e.lateBurst; Log(dash, LogLevel::Warn, t, "late frames: present missed the deadline (GPU busy)"); }
            if (app.Gpu() > 1000.0 / app.OutFps()) Log(dash, LogLevel::Warn, t, "GPU time over budget, frames will be skipped");
        }
        const double tEnd = t + dt;
        while (nextFrame < tEnd) {
            const int64_t q = q0 + (int64_t)(nextFrame * (double)f);
            if (app.warm) { tel.PushFrame(app.Frame(q, nextFrame)); produced++; }
            nextFrame += 1.0 / app.OutFps();
        }
        t = tEnd;
        app.uptime = t;
        tel.PublishStatus(app.Status(app.OutFps()));
        dash.Pump(&tel, o, q0 + (int64_t)(t * (double)f));
    }
    Log(dash, LogLevel::Error, t, "example error line: audio device lost, retrying");
    dash.Pump(&tel, o, q0 + (int64_t)(t * (double)f));
    for (const auto& [w, h] : sizes) {
        const bool ansi = ansiMode > 0;
        printf("---- %dx%d (simulated %.1f s, %ld frames)\n", w, h, dur, produced);
        const std::string s = dash.RenderText(w, h, ansi, help);
        fwrite(s.data(), 1, s.size(), stdout);
    }
    dash.Finish(q0 + (int64_t)(t * (double)f));   // writes the last segment to --history
    return 0;
}

// ------------------------------------------------------------------ live
struct LiveSim {
    Telemetry* tel;
    Dashboard* dash;
    std::atomic<bool> stop{false}, quit{false};
    double hz;
    std::thread th;
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        SimApp app;
        app.dispHz = hz;
        PreciseSleeper sl;
        const int64_t t0 = Qpc::Now();
        int64_t next = t0, nextStatus = t0, nextLog = t0 + Qpc::FromMs(3000);
        auto sec = [&](int64_t q) { return Qpc::ToSec(q - t0); };
        Log(*dash, LogLevel::Info, 0.0, "tui_demo live: simulated capture → vfx → display");
        int64_t frames = 0;
        while (!stop.load()) {
            const int64_t now = Qpc::Now();
            app.uptime = sec(now);
            if (!app.warm && app.uptime > 1.5) { app.warm = true; Log(*dash, LogLevel::Info, app.uptime, "warm-up done"); }
            for (int k; (k = tel->PopKey()) != 0;) {
                if (k == VK_ESCAPE) { quit = true; Log(*dash, LogLevel::Info, app.uptime, "quit requested"); continue; }
                const std::string l = app.Key(k);
                if (!l.empty()) Log(*dash, LogLevel::Info, app.uptime, l);
                if (app.Gpu() > 1000.0 / app.OutFps()) Log(*dash, LogLevel::Warn, app.uptime, "GPU time over budget");
            }
            if (now >= nextStatus) {
                tel->PublishStatus(app.Status(app.OutFps()));
                nextStatus += Qpc::FromMs(100);
            }
            if (now >= nextLog) {
                Log(*dash, LogLevel::Info, app.uptime, "pacing: D " + std::to_string(4.1).substr(0, 3) + " ms, slack 1.1 ms, drift +3 ppm");
                nextLog += Qpc::FromMs(4000);
            }
            if (app.warm) tel->PushFrame(app.Frame(now, app.uptime));
            frames++;
            next += Qpc::FromMs(1000.0 / app.OutFps());
            if (next < Qpc::Now()) next = Qpc::Now();
            sl.SleepUntil(next);
        }
    }
};

// "--keys 3:f,5:q,6:?,7:esc" -> (seconds, key) pairs injected into our own console input buffer,
// so they travel the real path (console -> FTXUI -> Dashboard -> Telemetry::PushKey -> app).
std::vector<std::pair<double, std::string>> ParseKeys(const std::string& spec) {
    std::vector<std::pair<double, std::string>> r;
    size_t pos = 0;
    while (pos < spec.size()) {
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
    WORD vk = 0; wchar_t ch = 0; DWORD ctrl = 0;
    if (k == "esc") { vk = VK_ESCAPE; ch = 0x1b; }
    else if (k == "ctrl-c") { vk = 'C'; ch = 0x03; ctrl = LEFT_CTRL_PRESSED; }
    else if (k == "plus") { vk = VK_OEM_PLUS; ch = '+'; }
    else if (k == "fw?") { vk = VK_OEM_2; ch = 0xFF1F; }          // full-width '？' (Chinese IME)
    else if (k == "fwf") { vk = 'F'; ch = 0xFF46; }               // full-width 'ｆ'
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

int RunLive(double seconds, double hz, const std::string& history, const std::string& keys, const std::string& outPath) {
    Telemetry tel;
    Dashboard dash;
    DashboardOptions o; o.tui = true; o.historyPath = history; o.refreshHz = 10;
    LiveSim sim; sim.tel = &tel; sim.dash = &dash; sim.hz = hz;
    const HANDLE hin = GetStdHandle(STD_INPUT_HANDLE), hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD inBefore = 0, outBefore = 0, inDuring = 0, inAfter = 0, outAfter = 0;
    GetConsoleMode(hin, &inBefore); GetConsoleMode(hout, &outBefore);
    const UINT cpBefore = GetConsoleOutputCP();
    dash.Start(&tel, o);
    sim.th = std::thread([&] { sim.Run(); });
    const int64_t t0 = Qpc::Now(), end = t0 + Qpc::FromMs(seconds * 1000);
    auto script = ParseKeys(keys);
    size_t ki = 0;
    while (!sim.quit.load() && (seconds <= 0 || Qpc::Now() < end)) {
        while (ki < script.size() && Qpc::ToSec(Qpc::Now() - t0) >= script[ki].first) InjectKey(script[ki++].second);
        if (!inDuring && Qpc::ToSec(Qpc::Now() - t0) > 1.0) GetConsoleMode(hin, &inDuring);
        Sleep(20);
    }
    const bool quitByKey = sim.quit.load();
    const int64_t s0 = Qpc::Now();
    dash.Stop();
    const double stopMs = Qpc::ToMs(Qpc::Now() - s0);
    sim.stop = true;
    sim.th.join();
    GetConsoleMode(hin, &inAfter); GetConsoleMode(hout, &outAfter);
    std::string s;
    {
        char m[256];
        snprintf(m, sizeof(m), "console: in 0x%lX -> (tui 0x%lX) -> 0x%lX, out 0x%lX -> 0x%lX, cp %u -> %u%s\n",
                 inBefore, inDuring, inAfter, outBefore, outAfter, cpBefore, GetConsoleOutputCP(),
                 (inBefore | ENABLE_EXTENDED_FLAGS) == (inAfter | ENABLE_EXTENDED_FLAGS) && outBefore == outAfter ? " (restored)" : " (NOT restored)");
        s += m;
    }
    const DashboardPerf p = dash.Perf();
    char b[512];
    snprintf(b, sizeof(b), "ui: %llu refreshes, %llu frames, %llu draws, update %.3f ms avg (max %.3f), build %.3f ms avg (max %.3f); "
             "quit by key: %s; Stop() took %.1f ms\n",
             (unsigned long long)p.refreshes, (unsigned long long)p.frames, (unsigned long long)p.draws,
             p.updateMsAvg, p.updateMsMax, p.buildMsAvg, p.buildMsMax, quitByKey ? "yes" : "no", stopMs);
    s += b;
    fwrite(s.data(), 1, s.size(), stdout);
    if (!outPath.empty()) {
        if (FILE* f = fopen(outPath.c_str(), "wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
    }
    return 0;
}

// ------------------------------------------------------------------ bench
double TscHz() {
    const int64_t q0 = Qpc::Now();
    const uint64_t c0 = __rdtsc();
    Sleep(200);
    const int64_t q1 = Qpc::Now();
    const uint64_t c1 = __rdtsc();
    return (double)(c1 - c0) / Qpc::ToSec(q1 - q0);
}

struct Dist { double p50, p99, p999, max; size_t n; };
Dist Stats(std::vector<double> v) {
    Dist d{};
    d.n = v.size();
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    auto q = [&](double p) { return v[(size_t)std::min<double>((double)v.size() - 1, p * (double)(v.size() - 1) + 0.5)]; };
    d.p50 = q(0.5); d.p99 = q(0.99); d.p999 = q(0.999); d.max = v.back();
    return d;
}

struct PhaseResult {
    Dist push, status, overhead;
    DashboardPerf perf;
    double seconds = 0, procCpuPct = 0, uiCpuMsPerRefresh = 0;
    bool tui = false;
};

double ProcCpuSec() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) * 1e-7; };
    return v(k) + v(u);
}

PhaseResult BenchPhase(bool tui, double seconds, double tscHz) {
    Telemetry tel;
    Dashboard dash;
    DashboardOptions o; o.tui = tui; o.refreshHz = 10;
    dash.Start(&tel, o);
    PhaseResult r;
    r.tui = dash.TuiActive();
    SimApp app;
    app.warm = true; app.dispHz = 120;
    std::vector<double> push, status, ovh;
    push.reserve(200000); status.reserve(20000); ovh.reserve(200000);
    const double nsPerTick = 1e9 / tscHz;
    std::thread prod([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const int64_t t0 = Qpc::Now(), end = t0 + Qpc::FromMs(seconds * 1000);
        int64_t nextFrame = t0, nextStatus = t0;
        const int64_t framePeriod = Qpc::FromMs(1.0), statusPeriod = Qpc::FromMs(10.0);   // 1 kHz frames, 100 Hz status
        while (true) {
            const int64_t now = Qpc::Now();
            if (now >= end) break;
            if (now >= nextFrame) {
                const FrameSample f = app.Frame(now, Qpc::ToSec(now - t0));
                _mm_lfence();
                const uint64_t a = __rdtsc();
                _mm_lfence();
                tel.PushFrame(f);
                _mm_lfence();
                const uint64_t b = __rdtsc();
                _mm_lfence();
                const uint64_t c = __rdtsc();
                _mm_lfence();
                push.push_back((double)(b - a) * nsPerTick);
                ovh.push_back((double)(c - b) * nsPerTick);
                nextFrame += framePeriod;
            }
            if (now >= nextStatus) {
                const AppStatus st = app.Status(120);
                _mm_lfence();
                const uint64_t a = __rdtsc();
                _mm_lfence();
                tel.PublishStatus(st);
                _mm_lfence();
                const uint64_t b = __rdtsc();
                status.push_back((double)(b - a) * nsPerTick);
                nextStatus += statusPeriod;
            }
            YieldProcessor();
        }
    });
    const double cpu0 = ProcCpuSec();
    const int64_t w0 = Qpc::Now();
    prod.join();
    const double wall = Qpc::ToSec(Qpc::Now() - w0);
    const double cpu1 = ProcCpuSec();
    r.perf = dash.Perf();
    dash.Stop();
    r.seconds = wall;
    // the producer spins on one core (100 %); report the rest
    r.procCpuPct = ((cpu1 - cpu0) / wall - 1.0) * 100.0;
    r.uiCpuMsPerRefresh = r.perf.refreshes ? (double)r.perf.uiCycles / tscHz * 1000.0 / (double)r.perf.refreshes : 0;
    r.push = Stats(push); r.status = Stats(status); r.overhead = Stats(ovh);
    return r;
}

std::string FormatPhase(const char* name, const PhaseResult& r) {
    char b[1024];
    snprintf(b, sizeof(b),
             "%s (%s, %.1f s)\n"
             "  PushFrame      ns: p50 %6.0f  p99 %6.0f  p99.9 %6.0f  max %7.0f  (n %zu; rdtsc overhead p50 %.0f ns)\n"
             "  PublishStatus  ns: p50 %6.0f  p99 %6.0f  p99.9 %6.0f  max %7.0f  (n %zu)\n"
             "  UI thread: %llu refreshes, %llu frames, update %.3f ms avg / %.3f max, build %.3f ms avg / %.3f max (%llu draws),\n"
             "             CPU %.3f ms per refresh (all work on the UI thread incl. terminal output)\n"
             "  process CPU besides the spinning producer: %.1f %% of one core\n",
             name, r.tui ? "TUI running" : "record-only", r.seconds,
             r.push.p50, r.push.p99, r.push.p999, r.push.max, r.push.n, r.overhead.p50,
             r.status.p50, r.status.p99, r.status.p999, r.status.max, r.status.n,
             (unsigned long long)r.perf.refreshes, (unsigned long long)r.perf.frames, r.perf.updateMsAvg, r.perf.updateMsMax,
             r.perf.buildMsAvg, r.perf.buildMsMax, (unsigned long long)r.perf.draws, r.uiCpuMsPerRefresh, r.procCpuPct);
    return b;
}

int RunBench(double seconds, const std::string& outPath, bool withTui) {
    const double tsc = TscHz();
    std::string report = "tui_demo --bench: producer 1 kHz PushFrame + 100 Hz PublishStatus, dashboard refresh 10 Hz\n";
    report += "TSC " + std::to_string(tsc / 1e9).substr(0, 5) + " GHz\n";
    report += FormatPhase("phase 1", BenchPhase(false, seconds, tsc));
    if (withTui) report += FormatPhase("phase 2", BenchPhase(true, seconds, tsc));
    fwrite(report.data(), 1, report.size(), stdout);
    if (!outPath.empty()) {
        if (FILE* f = fopen(outPath.c_str(), "wb")) { fwrite(report.data(), 1, report.size(), f); fclose(f); }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::pair<int, int>> sizes;
    long frames = 10000;
    int ansi = -1;
    std::string history, out, keys;
    bool live = false, bench = false, noTui = false, help = false;
    double seconds = -1, hz = 120;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--render") sizes = ParseSizes(next());
        else if (a == "--frames") frames = atol(next());
        else if (a == "--plain") ansi = 0;
        else if (a == "--ansi") ansi = 1;
        else if (a == "--history") history = next();
        else if (a == "--help-overlay") help = true;
        else if (a == "--live") live = true;
        else if (a == "--bench") bench = true;
        else if (a == "--no-tui") noTui = true;
        else if (a == "--seconds") seconds = atof(next());
        else if (a == "--hz") hz = atof(next());
        else if (a == "--out") out = next();
        else if (a == "--keys") keys = next();
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (bench) return RunBench(seconds > 0 ? seconds : 4, out, !noTui);
    if (live) return RunLive(seconds > 0 ? seconds : 0, hz, history, keys, out);
    if (!sizes.empty()) {
        if (ansi < 0) { DWORD m; ansi = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m) ? 1 : 0; }
        return RunRender(sizes, frames, ansi, history, help);
    }
    fprintf(stderr, "usage: tui_demo --render 120x36[,80x24] [--frames N] [--plain|--ansi] [--history f.csv] [--help-overlay]\n"
                    "       tui_demo --live [--seconds N] [--hz 120] [--history f.csv] [--keys 3:f,5:q,9:esc] [--out report.txt]\n"
                    "       tui_demo --bench [--seconds N] [--out report.txt] [--no-tui]\n");
    return 2;
}
