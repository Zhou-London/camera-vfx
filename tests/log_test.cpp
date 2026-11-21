// Logger + StatBook test and benchmark.
//  (a) correctness: 4 threads x 10000 messages per-thread ordered in the log file (each
//      written or counted as dropped), a 4-thread burst that fits the ring never dropped,
//      overflow drops + "[log] N messages dropped", LogFlush, level filter,
//      LOG_EVERY_MS, long messages, no escape codes in the file, sync fallback after
//      LogShutdown; the log sink (order, level filter, console off, clearing waits for a
//      call in progress, logging / clearing from inside the sink).
//  (d) pinned status line in child processes: redirected stdout (nothing written,
//      LogConsoleIsTty false) and a real windowless console, whose screen buffer is read
//      back (status stays the last line, logs scroll above it, colours, rate limit,
//      truncation, SetLogConsole(false) / LogShutdown erase it).
//  (b) caller-side LOGI cost (p50/p99/max us) vs. the old synchronous implementation,
//      measured in child processes whose stdout is NUL, a file, a real (windowless)
//      console, and that console with a status line updated at 30 Hz + a sink.
//  (c) StatBook::Add cost with long literal names vs. the old StatBook, allocations per
//      call, and Add latency while Report() runs on another thread.
#include "util/common.h"
#include "util/stats.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <new>

using namespace camss;

// ------------------------------------------------------------ allocation counter
static thread_local uint64_t t_allocs = 0;
void* operator new(size_t n) {
    t_allocs++;
    if (void* p = malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

// ------------------------------------------------------------ helpers
static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail++;
}

static std::string ReadFileStr(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t b = 0;
    while (b < s.size()) {
        size_t e = s.find('\n', b);
        if (e == std::string::npos) e = s.size();
        size_t ee = e;
        if (ee > b && s[ee - 1] == '\r') ee--;
        out.emplace_back(s, b, ee - b);
        b = e + 1;
    }
    return out;
}

static size_t CountSub(const std::string& s, const char* sub) {
    size_t n = 0, p = 0, l = strlen(sub);
    while ((p = s.find(sub, p)) != std::string::npos) { n++; p += l; }
    return n;
}

static std::string TempDir() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string d = std::string(tmp) + "camss_log_test_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

struct Lat { double p50 = 0, p99 = 0, max = 0, mean = 0; };
static Lat Summ(std::vector<double> v) {
    Lat l;
    if (v.empty()) return l;
    double s = 0; for (double x : v) s += x;
    l.mean = s / (double)v.size();
    l.max = *std::max_element(v.begin(), v.end());
    l.p50 = RollingStat::QuantileOf(v, 0.5);
    l.p99 = RollingStat::QuantileOf(v, 0.99);
    return l;
}
static double Us(int64_t ticks) { return (double)ticks * 1e6 / (double)Qpc::Freq(); }

// ------------------------------------------------------------ test sink
struct SinkRec { int lvl; double t; std::string msg; DWORD tid; };
struct SinkState {
    std::mutex m;
    std::vector<SinkRec> recs;
    std::atomic<int> inCall{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<int> sleepMs{0};
    bool keep = true;   // store the records (false: count only)
};
static void TestSinkFn(void* ctx, LogLevel lvl, double t, const char* msg, size_t len) {
    auto* s = static_cast<SinkState*>(ctx);
    s->inCall++;
    if (int ms = s->sleepMs.load()) Sleep((DWORD)ms);
    if (s->keep) {
        std::lock_guard<std::mutex> lk(s->m);
        s->recs.push_back({(int)lvl, t, std::string(msg, len), GetCurrentThreadId()});
    }
    s->calls++;
    s->inCall--;
}
// Logs from inside the sink and clears itself when it sees "trigger".
static std::atomic<int> g_reentrantCalls{0};
static void ReentrantSinkFn(void*, LogLevel, double, const char* msg, size_t len) {
    g_reentrantCalls++;
    if (std::string(msg, len) == "trigger") {
        LOGI("from-inside-sink");
        SetLogSink(nullptr, nullptr);
    }
}

// ------------------------------------------------------------ old implementation (baseline)
namespace old {
static std::mutex g_mutex;
static FILE* g_file = nullptr;
static void SyncLogV(LogLevel lvl, const char* fmt, va_list ap) {
    static const char* tags[] = {"D", "I", "W", "E"};
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    static const int64_t t0 = Qpc::Now();
    double t = Qpc::ToSec(Qpc::Now() - t0);
    std::lock_guard<std::mutex> lk(g_mutex);
    FILE* out = (lvl >= LogLevel::Warn) ? stderr : stdout;
    fprintf(out, "[%9.3f][%s] %s\n", t, tags[(int)lvl], msg);
    fflush(out);
    if (g_file) { fprintf(g_file, "[%9.3f][%s] %s\n", t, tags[(int)lvl], msg); fflush(g_file); }
}
static void SyncLog(LogLevel lvl, const char* fmt, ...) { va_list ap; va_start(ap, fmt); SyncLogV(lvl, fmt, ap); va_end(ap); }

class StatBook {
public:
    void Add(const std::string& name, double v) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = stats_.find(name);
        if (it == stats_.end()) { order_.push_back(name); it = stats_.emplace(name, RollingStat(2048)).first; }
        it->second.Add(v);
    }
    std::string Report() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string out;
        char line[256];
        snprintf(line, sizeof(line), "  %-26s %8s %8s %8s %8s\n", "stage(ms)", "p50", "p99", "max", "n");
        out += line;
        for (auto& n : order_) {
            auto& s = stats_.at(n);
            snprintf(line, sizeof(line), "  %-26s %8.3f %8.3f %8.3f %8zu\n", n.c_str(), s.Quantile(0.5), s.Quantile(0.99), s.Max(), s.Count());
            out += line;
        }
        return out;
    }
private:
    mutable std::mutex m_;
    std::map<std::string, RollingStat> stats_;
    std::vector<std::string> order_;
};
} // namespace old

// Stat names as used by the render loop (most longer than the 15-char SSO limit).
static const char* const kNames[] = {
    "cap.upload_ms", "cap.sample_to_arrival_ms", "cap.interval_ms", "loop.scanout_to_arrival_ms", "vfx.cpu_ms",
    "vfx.gpu_total", "vfx.gpu_sr", "vfx.gpu_fg", "rtx.cpu_ms", "present.late_ms", "render.cpu_ms", "present.call_ms",
    "present.interval_ms", "lat.arrival_to_present_ms", "pace.delay_ms", "pace.t", "gpu.convert", "gpu.upscale",
    "gpu.hdr", "gpu.composite", "lat.arrival_to_flip_ms", "lat.content_to_flip_ms", "lat.arrival_to_scanout_ms",
    "lat.content_to_scanout_ms", "lat.present_to_scanout_ms", "present.vblank_error_ms", "cap.overwritten",
    "present.gpu_busy", "gpu.interp", "gpu.total"};
static constexpr int kNumNames = (int)(sizeof(kNames) / sizeof(kNames[0]));

// ------------------------------------------------------------ (b) child: caller-cost benchmark
template <class F>
static Lat MeasurePaced(int n, double spacingUs, F&& f) {
    std::vector<double> v;
    v.reserve(n);
    const int64_t step = (int64_t)(spacingUs * (double)Qpc::Freq() / 1e6);
    int64_t next = Qpc::Now();
    for (int i = 0; i < n; i++) {
        while (Qpc::Now() < next) YieldProcessor();
        const int64_t t0 = Qpc::Now();
        f(i);
        const int64_t t1 = Qpc::Now();
        v.push_back(Us(t1 - t0));
        next = std::max(next + step, t1);
    }
    return Summ(std::move(v));
}

static int BenchChild(const char* mode, const char* resultPath) {
    FILE* res = fopen(resultPath, "w");
    if (!res) return 2;
    DWORD m = 0;
    const bool isCon = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m) != 0;

    // A realistic statistics table (as App::Report prints every 2 s).
    StatBook sb;
    for (int r = 0; r < 2048; r++)
        for (int i = 0; i < kNumNames; i++) sb.Add(kNames[i], 0.5 + 0.01 * ((r * 7 + i * 13) % 300));
    const std::string table = sb.Report();
    const char* head = "---- 12.0s  mode=vrr interp=nvof sr=rtx hdr=off | src 59.940 Hz (jit 0.05 ms) disp 119.880 Hz (jit 0.02 ms) | "
                       "D=9.12 ms minslack=1.10 lead=2.00 late=0 stall=0 missed=0 gpubusy=0 drops=0 tc=ok";

    const int nShort = 5000;
    const double shortSpacingUs = 200;
    const int nRep = 50;
    const double repSpacingUs = 5000;

    auto shortOld = MeasurePaced(nShort, shortSpacingUs, [&](int i) {
        old::SyncLog(LogLevel::Info, "frame %d presented late by %.3f ms (slot %d, queue %d)", i, 0.123 * (i % 7), i % 3, i % 5);
    });
    auto repOld = MeasurePaced(nRep, repSpacingUs, [&](int) { old::SyncLog(LogLevel::Info, "%s\n%s", head, table.c_str()); });

    SetLogLevel(LogLevel::Info);
    // "console-status": a status line updated at 30 Hz from another thread plus a sink
    // (as the dashboard / status line will run): the caller-side cost must not change.
    SinkState sink;
    sink.keep = false;
    std::atomic<bool> statusStop{false};
    std::thread statusThread;
    std::vector<double> statusCallUs;   // SetStatusLine cost (the app calls it from the render thread)
    statusCallUs.reserve(4096);
    if (!strcmp(mode, "console-status")) {
        SetLogSink(TestSinkFn, &sink);
        statusThread = std::thread([&] {
            for (int k = 0; !statusStop.load(); k++) {
                char s[160];
                snprintf(s, sizeof(s), "%s fps %.1f %s lat %.2f ms %s drops %d", style::Accent("\xE2\x97\x8F").c_str(), 120.0 - (k % 3) * 0.1,
                         style::Dim("\xE2\x94\x82").c_str(), 9.0 + (k % 10) * 0.01, style::Dim("\xE2\x94\x82").c_str(), k);
                const int64_t t0 = Qpc::Now();
                SetStatusLine(s);
                if (statusCallUs.size() < statusCallUs.capacity()) statusCallUs.push_back(Us(Qpc::Now() - t0));
                Sleep(33);
            }
        });
    }
    uint64_t allocs0 = t_allocs;
    auto shortNew = MeasurePaced(nShort, shortSpacingUs, [&](int i) {
        LOGI("frame %d presented late by %.3f ms (slot %d, queue %d)", i, 0.123 * (i % 7), i % 3, i % 5);
    });
    const double allocsPerShort = (double)(t_allocs - allocs0) / nShort;
    auto repNew = MeasurePaced(nRep, repSpacingUs, [&](int) { LOGI("%s\n%s", head, table.c_str()); });
    const int64_t f0 = Qpc::Now();
    const bool flushed = LogFlush(20000);
    const double flushMs = Qpc::ToMs(Qpc::Now() - f0);
    if (statusThread.joinable()) { statusStop = true; statusThread.join(); }
    SetLogSink(nullptr, nullptr);
    const LogCounters c = GetLogCounters();

    auto put = [&](const char* wl, const char* impl, const Lat& l) {
        fprintf(res, "%s %s %s %.3f %.3f %.3f %.3f\n", mode, wl, impl, l.p50, l.p99, l.max, l.mean);
    };
    put("short", "old", shortOld);
    put("short", "new", shortNew);
    put("report", "old", repOld);
    put("report", "new", repNew);
    if (!statusCallUs.empty()) put("status", "set", Summ(statusCallUs));
    fprintf(res, "info %d %llu %llu %llu %d %.1f %.3f %zu %llu %llu\n", isCon ? 1 : 0, (unsigned long long)c.enqueued,
            (unsigned long long)c.dropped, (unsigned long long)c.batches, flushed ? 1 : 0, flushMs, allocsPerShort, table.size(),
            (unsigned long long)c.statusDraws, (unsigned long long)sink.calls.load());
    fclose(res);
    LogShutdown();
    return 0;
}

struct BenchRow { std::string wl, impl; Lat l; };
struct BenchResult {
    bool ran = false, isCon = false, flushed = false;
    unsigned long long enq = 0, dropped = 0, batches = 0, statusDraws = 0, sinkCalls = 0;
    double flushMs = 0, allocsPerShort = 0;
    size_t tableBytes = 0;
    std::vector<BenchRow> rows;
    const Lat* Find(const char* wl, const char* impl) const {
        for (auto& r : rows) if (r.wl == wl && r.impl == impl) return &r.l;
        return nullptr;
    }
};

static BenchResult RunChild(const char* mode, const std::string& dir) {
    BenchResult br;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string resPath = dir + "\\bench_" + mode + ".txt";
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --bench-child " + Utf8ToWide(mode) + L" \"" + Utf8ToWide(resPath.c_str()) + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE out = nullptr;
    DWORD flags = 0;
    if (!strncmp(mode, "console", 7)) {
        flags = CREATE_NO_WINDOW;   // a real console (conhost, headless): console writes, no window
    } else {
        const std::string target = !strcmp(mode, "nul") ? std::string("NUL") : dir + "\\bench_stdout.txt";
        out = CreateFileA(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = out;
    }
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, out != nullptr, flags, nullptr, nullptr, &si, &pi)) {
        printf("  CreateProcess failed (%lu)\n", GetLastError());
        if (out) CloseHandle(out);
        return br;
    }
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (out) CloseHandle(out);
    if (code != 0) { printf("  child exit code %lu\n", code); return br; }

    for (const auto& line : SplitLines(ReadFileStr(resPath))) {
        char a[32], b[32], c[32];
        Lat l;
        int con = 0, fl = 0;
        if (sscanf(line.c_str(), "info %d %llu %llu %llu %d %lf %lf %zu %llu %llu", &con, &br.enq, &br.dropped, &br.batches, &fl, &br.flushMs,
                   &br.allocsPerShort, &br.tableBytes, &br.statusDraws, &br.sinkCalls) == 10) {
            br.isCon = con != 0; br.flushed = fl != 0;
        } else if (sscanf(line.c_str(), "%31s %31s %31s %lf %lf %lf %lf", a, b, c, &l.p50, &l.p99, &l.max, &l.mean) == 7) {
            br.rows.push_back({b, c, l});
        }
    }
    br.ran = !br.rows.empty();
    return br;
}

// ------------------------------------------------------------ (a) correctness
static void TestCorrectness(const std::string& dir) {
    printf("\n== (a) logger correctness ==\n");
    const std::string path = dir + "\\log_a.txt";
    SetLogConsole(false);
    SetLogFile(path.c_str());

    // Level parsing / filtering.
    Check(ParseLogLevel("debug", LogLevel::Info) == LogLevel::Debug && ParseLogLevel("WARN", LogLevel::Info) == LogLevel::Warn &&
          ParseLogLevel("error", LogLevel::Info) == LogLevel::Error && ParseLogLevel("info", LogLevel::Warn) == LogLevel::Info &&
          ParseLogLevel("bogus", LogLevel::Warn) == LogLevel::Warn && ParseLogLevel(nullptr, LogLevel::Error) == LogLevel::Error,
          "ParseLogLevel");
    SetLogLevel(LogLevel::Warn);
    const bool enabledOk = !LogEnabled(LogLevel::Info) && LogEnabled(LogLevel::Warn) && LogEnabled(LogLevel::Error);
    LOGD("filtered-debug");
    LOGI("filtered-info");
    LOGW("kept-warn %d", 1);
    LOGE("kept-error %d", 2);
    SetLogLevel(LogLevel::Info);
    LOGD("filtered-debug-2");
    LOGI("kept-info %d", 3);
    Check(enabledOk, "LogEnabled reflects the level");

    // Rate limited macro: 200 calls in < 1 s with a 1000 ms limit -> exactly one line.
    for (int i = 0; i < 200; i++) LOG_EVERY_MS(LogLevel::Info, 1000, "every-ms %d", i);

    // Long messages (beyond the stack buffer) arrive intact.
    std::string longMsg(5000, 'x');
    for (size_t i = 0; i < longMsg.size(); i += 100) longMsg[i] = (char)('a' + (i / 100) % 26);
    LOGI("long:%s:end", longMsg.c_str());

    // Multi-threaded: 4 x 10000 against the running writer. The burst (~1.3 MB of records,
    // enqueued in ~5 ms) is larger than the 1 MB ring, so whether some messages are dropped
    // depends on scheduling (a writer descheduled for a few ms must drop): every message is
    // either written, in per-thread order, or counted as dropped. The no-drop guarantee is
    // checked deterministically below with a burst that fits the ring.
    const int kThreads = 4, kPer = 10000;
    const LogCounters c0 = GetLogCounters();
    const int64_t mt0 = Qpc::Now();
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; t++)
            th.emplace_back([t] { for (int i = 0; i < kPer; i++) LOGI("mt %d %d", t, i); });
        for (auto& x : th) x.join();
    }
    const double mtMs = Qpc::ToMs(Qpc::Now() - mt0);
    const int64_t fl0 = Qpc::Now();
    const bool flushed = LogFlush(5000);
    const double flMs = Qpc::ToMs(Qpc::Now() - fl0);
    const LogCounters c1 = GetLogCounters();
    Check(flushed, "LogFlush returned true after the multi-thread burst");
    printf("    4 threads x %d LOGI: %.1f ms enqueue (%.0f ns/msg aggregate), flush %.2f ms, dropped %llu, batches %llu\n", kPer, mtMs,
           mtMs * 1e6 / (kThreads * kPer), flMs, (unsigned long long)(c1.dropped - c0.dropped), (unsigned long long)(c1.batches - c0.batches));

    // Per-thread order: indices strictly increasing; gaps only where messages were dropped.
    auto scanOrdered = [&](const std::string& text, const char* tag, int threads, bool exact, int& lines, int& bad) {
        std::vector<int> next(threads, 0);
        lines = 0; bad = 0;
        const size_t tl = strlen(tag);
        for (auto& l : SplitLines(text)) {
            size_t p = l.find(tag);
            if (p == std::string::npos) continue;
            int t = -1, i = -1;
            if (sscanf(l.c_str() + p + tl, "%d %d", &t, &i) != 2 || t < 0 || t >= threads) { bad++; continue; }
            if (exact ? i != next[t] : i < next[t]) bad++;
            next[t] = i + 1;
            lines++;
        }
    };
    std::string text = ReadFileStr(path);
    const uint64_t mtDropped = c1.dropped - c0.dropped;
    int mtLines = 0, bad = 0;
    scanOrdered(text, "] mt ", kThreads, mtDropped == 0, mtLines, bad);
    Check((uint64_t)mtLines + mtDropped == (uint64_t)(kThreads * kPer) && bad == 0,
          "40000 messages per-thread ordered in the file, each written or counted as dropped");
    if ((uint64_t)mtLines + mtDropped != (uint64_t)(kThreads * kPer) || bad)
        printf("    got %d lines + %llu dropped, %d out of order\n", mtLines, (unsigned long long)mtDropped, bad);

    // Deterministic: a 4-thread burst that fits the ring (4 x 6000 x 32 B = 768 KB) is never
    // dropped, even while the writer does not drain at all.
    const int kFitPer = 6000;
    detail::LogTestPauseWriter(true);
    const LogCounters f0 = GetLogCounters();
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; t++)
            th.emplace_back([t] { for (int i = 0; i < kFitPer; i++) LOGI("mp %d %d", t, i); });
        for (auto& x : th) x.join();
    }
    const LogCounters f1 = GetLogCounters();
    detail::LogTestPauseWriter(false);
    const bool flushedFit = LogFlush(5000);
    text = ReadFileStr(path);
    int fitLines = 0, fitBad = 0;
    scanOrdered(text, "] mp ", kThreads, true, fitLines, fitBad);
    Check(flushedFit && f1.dropped == f0.dropped, "no drops for a 4-thread burst that fits the ring (writer paused)");
    Check(fitLines == kThreads * kFitPer && fitBad == 0, "24000 messages complete and per-thread ordered in the file");
    Check(CountSub(text, "filtered-") == 0, "Debug/Info filtered at level Warn");
    Check(CountSub(text, "kept-warn 1") == 1 && CountSub(text, "kept-error 2") == 1 && CountSub(text, "kept-info 3") == 1, "Warn/Error/Info kept");
    Check(CountSub(text, "[W] kept-warn") == 1 && CountSub(text, "[E] kept-error") == 1, "line format [%9.3f][W] msg");
    Check(CountSub(text, "every-ms ") == 1, "LOG_EVERY_MS emits once per interval");
    Check(text.find("long:" + longMsg + ":end") != std::string::npos, "5000-char message intact");

    // Overflow: pause the writer, overfill the 1 MB ring, resume.
    const LogCounters o0 = GetLogCounters();
    detail::LogTestPauseWriter(true);
    std::string payload(480, 'p');
    const int kOvf = 4000;
    for (int i = 0; i < kOvf; i++) LOGI("ovf %d %s", i, payload.c_str());
    const LogCounters o1 = GetLogCounters();
    detail::LogTestPauseWriter(false);
    const bool flushed2 = LogFlush(5000);
    const uint64_t dropped = o1.dropped - o0.dropped;
    text = ReadFileStr(path);
    char expect[96];
    snprintf(expect, sizeof(expect), "[W] [log] %llu messages dropped", (unsigned long long)dropped);
    const size_t ovfLines = CountSub(text, "] ovf ");
    printf("    overflow: %d sent, %llu accepted, %llu dropped, %zu in file\n", kOvf, (unsigned long long)(o1.enqueued - o0.enqueued),
           (unsigned long long)dropped, ovfLines);
    Check(flushed2 && dropped > 0 && dropped < (uint64_t)kOvf, "overflow drops (bounded ring)");
    Check(ovfLines + dropped == (size_t)kOvf, "accepted messages all written after overflow");
    Check(CountSub(text, expect) == 1, "\"[log] N messages dropped\" reported once with the right N");
    // Messages logged after recovery still arrive.
    LOGI("after-overflow");
    LogFlush();
    text = ReadFileStr(path);
    Check(CountSub(text, "after-overflow") == 1, "logging resumes after overflow");
    Check(text.find('\x1b') == std::string::npos, "no escape codes in the log file");

    // Flush latency on an idle queue.
    std::vector<double> fv;
    for (int i = 0; i < 50; i++) { LOGI("flush-probe %d", i); int64_t t0 = Qpc::Now(); LogFlush(); fv.push_back(Qpc::ToMs(Qpc::Now() - t0)); }
    Lat fl = Summ(fv);
    printf("    LogFlush after one message: p50 %.3f ms, max %.3f ms\n", fl.p50, fl.max);
    Check(fl.max < 100, "LogFlush is bounded and prompt");

    // Caller-side allocations: none for a short message.
    uint64_t a0 = t_allocs;
    for (int i = 0; i < 1000; i++) LOGI("alloc-probe %d %.3f %s", i, i * 0.5, "abc");
    Check(t_allocs == a0, "LOGI performs no heap allocation on the calling thread");
    LogFlush();
}

// ------------------------------------------------------------ (a) log sink
static void TestSink(const std::string& dir) {
    printf("\n== (a) log sink ==\n");
    // Console output is off here (TestCorrectness): the sink must still get everything.
    SinkState st;
    SetLogSink(TestSinkFn, &st);
    SetLogLevel(LogLevel::Warn);
    LOGI("sink-filtered %d", 0);
    LOGW("sink-w %d", 1);
    SetLogLevel(LogLevel::Info);
    LOGD("sink-filtered-debug");
    LOGI("sink-i %d", 2);
    LOGE("sink-e %d", 3);
    LOGI("sink-ml first\nsecond");
    const int kT = 3, kPer = 5000;
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kT; t++) th.emplace_back([t] { for (int i = 0; i < kPer; i++) LOGI("sk %d %d", t, i); });
        for (auto& x : th) x.join();
    }
    // Caller side still allocation-free with a sink installed.
    uint64_t a0 = t_allocs;
    for (int i = 0; i < 1000; i++) LOGI("sink-alloc-probe %d", i);
    const bool noAlloc = t_allocs == a0;
    LogFlush(5000);

    std::vector<SinkRec> recs;
    { std::lock_guard<std::mutex> lk(st.m); recs = st.recs; }
    auto idx = [&](const char* m) { for (size_t i = 0; i < recs.size(); i++) if (recs[i].msg == m) return (int)i; return -1; };
    const int iw = idx("sink-w 1"), ii = idx("sink-i 2"), ie = idx("sink-e 3"), iml = idx("sink-ml first\nsecond");
    Check(iw >= 0 && ii > iw && ie > ii && iml > ie, "sink gets the messages in order, plain text, multi-line as one call");
    Check(iw >= 0 && ie >= 0 && recs[iw].lvl == (int)LogLevel::Warn && recs[ii].lvl == (int)LogLevel::Info && recs[ie].lvl == (int)LogLevel::Error,
          "sink gets the level");
    bool filtered = true;
    for (auto& r : recs) if (r.msg.find("sink-filtered") != std::string::npos) filtered = false;
    Check(filtered, "sink only gets messages that pass the level filter");
    std::vector<int> next(kT, 0);
    int sk = 0, bad = 0;
    std::vector<double> lastT(kT, -1.0);
    bool tOk = true, oneThread = true;
    const DWORD writerTid = recs.empty() ? 0 : recs[0].tid;
    for (auto& r : recs) {
        if (r.tid != writerTid || r.tid == GetCurrentThreadId()) oneThread = false;
        int t = -1, i = -1;
        if (sscanf(r.msg.c_str(), "sk %d %d", &t, &i) != 2) continue;
        if (t < 0 || t >= kT || i != next[t]) { bad++; continue; }
        next[t] = i + 1;
        if (r.t < lastT[t]) tOk = false;
        lastT[t] = r.t;
        sk++;
    }
    Check(sk == kT * kPer && bad == 0, "15000 messages from 3 threads complete and per-thread ordered");
    Check(oneThread, "sink called on the writer thread only");
    Check(tOk, "sink timestamps non-decreasing per thread");
    Check(noAlloc, "LOGI does not allocate on the calling thread with a sink installed");

    // Clearing waits for a call in progress; no calls afterwards.
    st.sleepMs = 30;
    LOGI("sink-slow");
    const int64_t w0 = Qpc::Now();
    while (st.inCall.load() == 0 && Qpc::ToMs(Qpc::Now() - w0) < 2000) YieldProcessor();
    const bool wasIn = st.inCall.load() == 1;
    const int64_t c0 = Qpc::Now();
    SetLogSink(nullptr, nullptr);
    const double clearMs = Qpc::ToMs(Qpc::Now() - c0);
    const bool notIn = st.inCall.load() == 0;
    st.sleepMs = 0;
    const uint64_t callsAfterClear = st.calls.load();
    for (int i = 0; i < 100; i++) LOGI("after-clear %d", i);
    LogFlush();
    printf("    SetLogSink(nullptr) during a 30 ms sink call returned after %.1f ms\n", clearMs);
    Check(wasIn && notIn, "SetLogSink(nullptr) waits for the call in progress");
    Check(st.calls.load() == callsAfterClear, "no sink calls after SetLogSink(nullptr) returned");

    // Logging from inside the sink and clearing it from inside: no deadlock.
    g_reentrantCalls = 0;
    SetLogSink(ReentrantSinkFn, nullptr);
    LOGI("trigger");
    LOGI("after-trigger");
    bool fl = LogFlush(3000);
    fl = LogFlush(3000) && fl;   // the message logged inside the sink was queued during the first flush's batch
    const std::string text = ReadFileStr(dir + "\\log_a.txt");
    Check(fl && g_reentrantCalls.load() == 1 && CountSub(text, "] from-inside-sink") == 1 && CountSub(text, "] after-trigger") == 1,
          "sink may log and clear itself from inside");
    Check(CountSub(text, "] after-clear ") == 100, "messages after clearing still reach the file");
    SetLogSink(nullptr, nullptr);
}

// ------------------------------------------------------------ (d) status line children
static std::wstring ConRow(HANDLE h, int y, int width) {
    std::wstring s((size_t)width, L' ');
    DWORD n = 0;
    ReadConsoleOutputCharacterW(h, s.data(), (DWORD)width, COORD{0, (SHORT)y}, &n);
    s.resize(n);
    while (!s.empty() && (s.back() == L' ' || s.back() == 0)) s.pop_back();
    return s;
}
static WORD ConAttr(HANDLE h, int x, int y) {
    WORD a = 0;
    DWORD n = 0;
    ReadConsoleOutputAttribute(h, &a, 1, COORD{(SHORT)x, (SHORT)y}, &n);
    return a;
}
struct ConPos { int width = 0, x = 0, y = 0; };
static ConPos ConCursor(HANDLE h) {
    CONSOLE_SCREEN_BUFFER_INFO bi = {};
    GetConsoleScreenBufferInfo(h, &bi);
    return {bi.dwSize.X, bi.dwCursorPosition.X, bi.dwCursorPosition.Y};
}
static int FindRow(HANDLE h, const wchar_t* text, int y0, int y1, int width) {
    for (int y = y1; y >= y0; y--) if (ConRow(h, y, width).find(text) != std::wstring::npos) return y;
    return -1;
}
static int CountRows(HANDLE h, const wchar_t* text, int y0, int y1, int width) {
    int n = 0;
    for (int y = y0; y <= y1; y++) if (ConRow(h, y, width).find(text) != std::wstring::npos) n++;
    return n;
}

static int StatusChild(const char* mode, const std::string& dir) {
    FILE* res = fopen((dir + "\\status_" + mode + ".txt").c_str(), "w");
    if (!res) return 2;
    auto check = [&](bool ok, const char* what) { fprintf(res, "check %d %s\n", ok ? 1 : 0, what); fflush(res); };
    auto note = [&](const std::string& s) { fprintf(res, "note %s\n", s.c_str()); fflush(res); };
    SinkState sink;
    SetLogSink(TestSinkFn, &sink);
    const std::string logPath = dir + "\\status_" + mode + "_log.txt";
    SetLogFile(logPath.c_str());

    if (!strcmp(mode, "redirected")) {
        check(!LogConsoleIsTty(), "LogConsoleIsTty() false with redirected stdout");
        check(!style::Enabled() && style::Dim("x") == "x" && style::Accent("y") == "y" && style::Err("z") == "z" && style::Bold("b") == "b",
              "style:: helpers return the plain text when redirected");
        SetStatusLine("STATUS-XYZ one");
        for (int i = 0; i < 5; i++) LOGI("redir-line %d", i);
        LOGW("redir-warn");
        Sleep(250);
        SetStatusLine("STATUS-XYZ two");
        LOGI("redir-last");
        LogFlush();
        check(GetLogCounters().statusDraws == 0, "no status draws with redirected stdout");
        LogShutdown();
        check(sink.calls.load() == 7, "sink got the 7 messages");
        fclose(res);
        return 0;
    }

    // ---- tty: a real (windowless) console; read our own screen buffer back.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool tty = LogConsoleIsTty();
    check(tty, "LogConsoleIsTty() true on a VT console");
    if (!tty) { fclose(res); return 0; }
    ConPos p = ConCursor(h);
    const int W = p.width;
    note("console width " + std::to_string(W) + ", output code page " + std::to_string(GetConsoleOutputCP()));

    LOGI("tty-before");
    const std::string statusA = style::Accent("\xE2\x97\x8F") + " status-A " + style::Dim("\xE2\x94\x82") + " fps " + style::Bold("120.0") + " " +
                                style::Dim("\xE2\x94\x82") + " lat 9.12 ms";
    SetStatusLine(statusA);
    Sleep(300);
    p = ConCursor(h);
    const int yBefore = FindRow(h, L"tty-before", 0, p.y, W);
    std::wstring cur = ConRow(h, p.y, W);
    check(cur.find(L"status-A \x2502 fps 120.0 \x2502 lat 9.12 ms") != std::wstring::npos && yBefore == p.y - 1,
          "status line drawn below the last log line");
    check(p.x > 0 && cur.find(L'\x1b') == std::wstring::npos && cur.find(L"[") == std::wstring::npos,
          "status line: no visible escape codes, cursor at its end");

    // Logs scroll above the status line; the status text changes while logging.
    for (int i = 0; i < 40; i++) {
        LOGI("tty-line %02d", i);
        if (i % 5 == 4) {
            char s[64];
            snprintf(s, sizeof(s), "%s status-B %d", style::Ok("\xE2\x97\x8F").c_str(), i + 1);
            SetStatusLine(s);
        }
        Sleep(2);
    }
    LOGW("tty-warn %d", 1);
    LOGE("tty-error %d", 2);
    LOGI("tty-ml first\nsecond");
    LOGD("tty-debug (filtered)");
    LogFlush();
    Sleep(300);
    p = ConCursor(h);
    const int y0 = FindRow(h, L"tty-before", 0, p.y, W);
    bool seq = y0 >= 0;
    std::vector<std::wstring> expect = {L"tty-before"};
    for (int i = 0; i < 40; i++) { wchar_t b[32]; swprintf(b, 32, L"tty-line %02d", i); expect.push_back(b); }
    expect.push_back(L"tty-warn 1");
    expect.push_back(L"tty-error 2");
    expect.push_back(L"tty-ml first");
    expect.push_back(L"second");
    for (size_t k = 0; seq && k < expect.size(); k++) {
        const std::wstring r = ConRow(h, y0 + (int)k, W);
        if (r.find(expect[k]) == std::wstring::npos || r.find(L"status-") != std::wstring::npos) {
            seq = false;
            note("row " + std::to_string(k) + " unexpected: " + WideToUtf8(r.c_str()));
        }
    }
    check(seq && y0 + (int)expect.size() == p.y, "44 log lines in order directly above the status line, one row each");
    check(CountRows(h, L"status-", 0, p.y, W) == 1 && ConRow(h, p.y, W).find(L"status-B 40") != std::wstring::npos,
          "exactly one status line on screen (the last row) showing the latest text");
    // Visible layout and colours of the log lines.
    const std::wstring rInfo = ConRow(h, y0, W), rWarn = ConRow(h, y0 + 41, W), rErr = ConRow(h, y0 + 42, W), rCont = ConRow(h, y0 + 44, W);
    const size_t mi = rInfo.find(L'\x00B7'), mw = rWarn.find(L'\x25B2'), me = rErr.find(L'\x00D7');
    note("marker columns: info " + std::to_string((int)mi) + " warn " + std::to_string((int)mw) + " error " + std::to_string((int)me));
    check(rInfo.size() > 12 && rInfo[9] == L' ' && mi == 10 && rInfo.substr(12) == L"tty-before" && rInfo.find(L"[I]") == std::wstring::npos,
          "info line: '%9.3f · message'");
    check(mw == 10 && me == 10 && rWarn.substr(12) == L"tty-warn 1" && rErr.substr(12) == L"tty-error 2", "warn '▲' / error '×' markers");
    check(rCont == std::wstring(12, L' ') + L"second", "continuation line indented to the message column");
    const WORD aTs = ConAttr(h, 8, y0), aMsg = ConAttr(h, 12, y0), aWarn = ConAttr(h, 10, y0 + 41), aErrMsg = ConAttr(h, 12, y0 + 42);
    char ab[160];
    snprintf(ab, sizeof(ab), "attributes: timestamp 0x%02X, info text 0x%02X, warn marker 0x%02X, error text 0x%02X", aTs, aMsg, aWarn, aErrMsg);
    note(ab);
    check((aTs & 0x0F) == FOREGROUND_INTENSITY && (aMsg & 0x0F) == (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) &&
              (aWarn & 0x0F) == (FOREGROUND_RED | FOREGROUND_GREEN) && (aErrMsg & 0x0F) == (FOREGROUND_RED | FOREGROUND_INTENSITY),
          "colours: grey time, default text, yellow warn marker, red error");
    for (int y = std::max(y0, p.y - 8); y <= p.y; y++) note("screen|" + WideToUtf8(ConRow(h, y, W).c_str()));

    // Rate limit: 1000 updates in ~1 s -> about 10 redraws.
    const uint64_t d0 = GetLogCounters().statusDraws;
    const int64_t r0 = Qpc::Now();
    int updates = 0;
    while (Qpc::ToMs(Qpc::Now() - r0) < 1000) {
        char s[64];
        snprintf(s, sizeof(s), "status-R %d", updates++);
        SetStatusLine(s);
        Sleep(1);
    }
    char lastR[64];
    snprintf(lastR, sizeof(lastR), "status-R %d", updates - 1);
    Sleep(250);
    const uint64_t draws = GetLogCounters().statusDraws - d0;
    note(std::to_string(updates) + " SetStatusLine calls in 1 s -> " + std::to_string(draws) + " redraws");
    p = ConCursor(h);
    check(draws >= 5 && draws <= 13, "status redraws rate-limited to ~10/s");
    check(ConRow(h, p.y, W) == Utf8ToWide(lastR) && CountRows(h, L"status-", 0, p.y, W) == 1, "latest text shown after the burst");

    // Control characters (backspace, tab, bell, CR, newline) cannot move the cursor off the line.
    const int yCtl = p.y;
    SetStatusLine("status-K a\bb\tc\ad\re\nf");
    Sleep(250);
    p = ConCursor(h);
    check(p.y == yCtl && ConRow(h, p.y, W) == L"status-K a b c d e f" && CountRows(h, L"status-", 0, p.y, W) == 1,
          "control characters in the status text become spaces");

    // Longer than the console: truncated with '…', never wraps.
    const int yLong = p.y;
    SetStatusLine(std::string(W + 50, 'w') + "END");
    Sleep(250);
    p = ConCursor(h);
    cur = ConRow(h, p.y, W);
    check(p.y == yLong && cur.size() < (size_t)W && cur.back() == L'\x2026' && cur.find(L"END") == std::wstring::npos &&
              ConRow(h, p.y - 1, W).find(L"www") == std::wstring::npos,
          "over-long status truncated to the width with '…' (no wrap)");
    // Wide (CJK) text: width counted in columns.
    std::string cjk;
    for (int i = 0; i < W; i++) cjk += "\xE7\x8A\xB6";   // 状 x W (2 columns each)
    SetStatusLine(cjk);
    Sleep(250);
    p = ConCursor(h);
    check(p.y == yLong && ConRow(h, p.y - 1, W).find(L'\x72B6') == std::wstring::npos && p.x < W, "CJK status truncated by columns (no wrap)");

    // SetLogConsole(false) erases it at once; logs go to the sink only; true restores it.
    SetStatusLine("status-C ok");
    Sleep(250);
    SetLogConsole(false);
    p = ConCursor(h);
    check(p.x == 0 && ConRow(h, p.y, W).empty() && CountRows(h, L"status-", 0, p.y, W) == 0, "SetLogConsole(false) erased the status line synchronously");
    const uint64_t sc0 = sink.calls.load();
    LOGI("tty-while-off");
    LogFlush();
    Sleep(150);
    p = ConCursor(h);
    check(CountRows(h, L"tty-while-off", 0, p.y, W) == 0 && ConRow(h, p.y, W).empty() && sink.calls.load() == sc0 + 1,
          "console off: nothing drawn, sink still called");
    SetLogConsole(true);
    Sleep(250);
    p = ConCursor(h);
    check(ConRow(h, p.y, W) == L"status-C ok", "SetLogConsole(true) redraws the status line");
    SetStatusLine("");
    Sleep(250);
    p = ConCursor(h);
    check(p.x == 0 && ConRow(h, p.y, W).empty(), "empty text removes the status line");

    // LogShutdown erases it; later synchronous lines start at column 0.
    SetStatusLine("status-F final");
    Sleep(250);
    LOGI("tty-last");
    LogShutdown();
    p = ConCursor(h);
    check(p.x == 0 && ConRow(h, p.y, W).empty() && ConRow(h, p.y - 1, W).find(L"tty-last") != std::wstring::npos &&
              CountRows(h, L"status-", 0, p.y, W) == 0,
          "LogShutdown erased the status line, cursor at column 0");
    LOGW("tty-after-shutdown");
    p = ConCursor(h);
    check(ConRow(h, p.y - 1, W).substr(10) == L"\x25B2 tty-after-shutdown", "synchronous line after shutdown");
    SetStatusLine("status-G ignored");
    Sleep(100);
    p = ConCursor(h);
    check(ConRow(h, p.y, W).empty(), "SetStatusLine after LogShutdown does nothing");

    const std::string logText = ReadFileStr(logPath);
    check(CountSub(logText, "status-") == 0 && logText.find('\x1b') == std::string::npos && CountSub(logText, "[I] tty-line 39") == 1 &&
              CountSub(logText, "[W] tty-warn 1") == 1,
          "log file: plain format, no status line, no escape codes");
    fclose(res);
    return 0;
}

// Runs a status child: "redirected" (stdout/stderr -> file) or "tty" (windowless console).
static void TestStatusLine(const std::string& dir) {
    printf("\n== (d) pinned status line (child processes) ==\n");
    for (const char* mode : {"redirected", "tty"}) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --status-child " + Utf8ToWide(mode) + L" \"" + Utf8ToWide(dir.c_str()) + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE out = nullptr;
        DWORD flags = CREATE_NO_WINDOW;
        const std::string outPath = dir + "\\status_stdout.txt";
        if (!strcmp(mode, "redirected")) {
            flags = 0;
            out = CreateFileA(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = out;
            si.hStdError = out;
        }
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, out != nullptr, flags, nullptr, nullptr, &si, &pi)) {
            Check(false, "status child started");
            if (out) CloseHandle(out);
            continue;
        }
        const bool done = WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0;
        if (!done) TerminateProcess(pi.hProcess, 3);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (out) CloseHandle(out);
        printf("  -- %s\n", mode);
        int checks = 0;
        for (const auto& line : SplitLines(ReadFileStr(dir + "\\status_" + mode + ".txt"))) {
            if (line.rfind("check ", 0) == 0 && line.size() > 8) { Check(line[6] == '1', line.c_str() + 8); checks++; }
            else if (line.rfind("note ", 0) == 0) printf("    %s\n", line.c_str() + 5);
        }
        Check(done && code == 0 && checks > 0, "status child finished");
        if (!strcmp(mode, "redirected")) {
            const std::string o = ReadFileStr(outPath);
            Check(CountSub(o, "STATUS-XYZ") == 0 && o.find('\x1b') == std::string::npos && CountSub(o, "[I] redir-line 0") == 1 &&
                      CountSub(o, "[W] redir-warn") == 1,
                  "redirected stdout: plain '[%9.3f][I] msg' lines, no status line, no escape codes");
            const std::string lf = ReadFileStr(dir + "\\status_redirected_log.txt");
            Check(CountSub(lf, "STATUS-XYZ") == 0 && CountSub(lf, "] redir-last") == 1, "status line not in the log file");
        }
    }
}

static void TestShutdown(const std::string& dir) {
    printf("\n== (a) shutdown / sync fallback ==\n");
    LOGI("before-shutdown");
    // Producers keep logging across LogShutdown: each thread's order must survive the
    // async -> sync switch (its queued messages are written before its synchronous ones).
    const int kT = 3, kAfter = 300;
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kT; t++)
            th.emplace_back([&, t] {
                started++;
                int extra = 0;
                for (int i = 0; extra < kAfter; i++) {
                    LOGI("so %d %d", t, i);
                    if (stop.load()) extra++;
                }
            });
        while (started.load() < kT) Sleep(0);
        Sleep(5);
        LogShutdown();
        stop = true;
        for (auto& x : th) x.join();
    }
    LogShutdown();   // idempotent
    LOGW("after-shutdown %d", 7);   // synchronous path
    const bool fl = LogFlush(10);
    std::string text = ReadFileStr(dir + "\\log_a.txt");
    Check(CountSub(text, "before-shutdown") == 1, "shutdown flushed queued messages");
    Check(CountSub(text, "[W] after-shutdown 7") == 1 && fl, "LogV works synchronously after LogShutdown");
    std::vector<int> next(kT, 0);
    int soLines = 0, soBad = 0;
    for (auto& l : SplitLines(text)) {
        size_t p = l.find("] so ");
        if (p == std::string::npos) continue;
        int t = -1, i = -1;
        if (sscanf(l.c_str() + p + 5, "%d %d", &t, &i) != 2 || t < 0 || t >= kT) { soBad++; continue; }
        if (i < next[t]) soBad++;   // gaps are drops (bounded ring); going backwards is a reorder
        next[t] = i + 1;
        soLines++;
    }
    printf("    logging across LogShutdown: %d lines, %d out of order\n", soLines, soBad);
    Check(soLines >= kT * kAfter && soBad == 0, "per-thread order kept across LogShutdown (async -> sync)");
    SetLogFile(nullptr);
}

// ------------------------------------------------------------ (c) StatBook
template <class Book>
static double AddNsPerOp(Book& b, int rounds) {
    const int64_t t0 = Qpc::Now();
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < kNumNames; i++) b.Add(kNames[i], (double)((r + i) & 63) * 0.01);
    return Us(Qpc::Now() - t0) * 1000.0 / ((double)rounds * kNumNames);
}

struct ConcResult { Lat add; int over50 = 0, over200 = 0; int reports = 0; double reportUs = 0; };
template <class Book>
static ConcResult AddDuringReport(Book& b, double seconds) {
    std::atomic<bool> stop{false};
    std::atomic<int> reports{0};
    std::atomic<int64_t> reportTicks{0};
    std::thread rep([&] {
        while (!stop.load()) {
            int64_t t0 = Qpc::Now();
            std::string s = b.Report();
            reportTicks += Qpc::Now() - t0;
            reports++;
            if (s.empty()) printf("?");
            // Report every ~1 ms (the app reports every 2 s; this is a stress cadence that
            // still lets the adder run between reports, unlike a back-to-back loop).
            const int64_t w = Qpc::Now() + Qpc::FromMs(1.0);
            while (!stop.load() && Qpc::Now() < w) std::this_thread::yield();
        }
    });
    std::vector<double> lat;
    lat.reserve(4000000);
    const int64_t end = Qpc::Now() + Qpc::FromMs(seconds * 1000);
    int i = 0;
    while (Qpc::Now() < end) {
        const int64_t t0 = Qpc::Now();
        b.Add(kNames[i % kNumNames], (double)(i & 127) * 0.01);
        lat.push_back(Us(Qpc::Now() - t0));
        i++;
        if ((i & 31) == 0) { int64_t w = Qpc::Now() + Qpc::FromMs(0.02); while (Qpc::Now() < w) YieldProcessor(); }
    }
    stop = true;
    rep.join();
    ConcResult r;
    for (double v : lat) { if (v > 50) r.over50++; if (v > 200) r.over200++; }
    r.add = Summ(std::move(lat));
    r.reports = reports.load();
    r.reportUs = r.reports ? Us(reportTicks.load()) / r.reports : 0;
    return r;
}

static void TestStatBook() {
    printf("\n== (c) StatBook ==\n");
    // Equivalence: same report text as the old implementation.
    {
        old::StatBook ob;
        StatBook nb;
        for (int r = 0; r < 3000; r++)
            for (int i = 0; i < kNumNames; i++) {
                double v = 0.1 * ((r * 31 + i * 17) % 997);
                ob.Add(kNames[i], v);
                nb.Add(kNames[i], v);
            }
        Check(ob.Report() == nb.Report(), "Report() text identical to the old implementation");
        double p50 = 0, p99 = 0;
        Check(nb.Get("lat.arrival_to_flip_ms", p50, p99) && p99 >= p50 && !nb.Get("missing", p50, p99), "Get()");
        auto snap = nb.Snapshot();
        Check(snap.size() == (size_t)kNumNames && snap[0].name == kNames[0] && snap[0].n == 2048 && StatBook::Report(snap) == nb.Report(),
              "Snapshot() + Report(snapshot)");
        nb.Clear();
        Check(!nb.Get("lat.arrival_to_flip_ms", p50, p99), "Clear()");
    }

    // Add cost with long literal names.
    {
        old::StatBook ob;
        StatBook nb;
        AddNsPerOp(ob, 200); AddNsPerOp(nb, 200);   // warm-up (creates the entries)
        uint64_t a0 = t_allocs;
        double oldNs = AddNsPerOp(ob, 20000);
        const double oldAllocs = (double)(t_allocs - a0) / (20000.0 * kNumNames);
        a0 = t_allocs;
        double newNs = AddNsPerOp(nb, 20000);
        const double newAllocs = (double)(t_allocs - a0) / (20000.0 * kNumNames);
        printf("    Add (30 names, mostly > SSO): old %.1f ns/op (%.2f allocs/op), new %.1f ns/op (%.2f allocs/op)\n", oldNs, oldAllocs,
               newNs, newAllocs);
        Check(newAllocs == 0.0, "StatBook::Add does not allocate after warm-up");
        Check(newNs < oldNs, "StatBook::Add faster than before");

        uint64_t q0 = t_allocs;
        double sink = 0;
        RollingStat rs(2048);
        for (int i = 0; i < 4096; i++) rs.Add(i * 0.1);
        sink += rs.Quantile(0.5);
        q0 = t_allocs;
        for (int i = 0; i < 100; i++) sink += rs.Quantile(0.99);
        Check(t_allocs == q0 && sink > 0, "RollingStat::Quantile does not allocate after warm-up");
    }

    // Add latency while another thread reports continuously (40 full stats would be the app case).
    {
        old::StatBook ob;
        StatBook nb;
        for (int r = 0; r < 2048; r++)
            for (int i = 0; i < kNumNames; i++) { ob.Add(kNames[i], r * 0.01); nb.Add(kNames[i], r * 0.01); }
        ConcResult o = AddDuringReport(ob, 1.0);
        ConcResult n = AddDuringReport(nb, 1.0);
        auto pr = [](const char* tag, const ConcResult& r) {
            printf("    %s: Add p50 %.3f us p99 %.3f us max %.1f us, >50us %d, >200us %d | %d reports, %.0f us/report\n", tag, r.add.p50,
                   r.add.p99, r.add.max, r.over50, r.over200, r.reports, r.reportUs);
        };
        pr("old", o);
        pr("new", n);
        Check(n.over200 * 10 <= std::max(o.over200, 1) && n.over50 * 10 <= std::max(o.over50, 1),
              "Add is not blocked by a concurrent Report");
    }
}

// ------------------------------------------------------------ main
// Logging from a static initializer (before main) must work.
static const int g_preMain = (LOGD("pre-main static-init message (Debug, filtered)"), LogEnabled(LogLevel::Info) ? 1 : 0);

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "--bench-child")) return BenchChild(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "--status-child")) return StatusChild(argv[2], argv[3]);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string dir = TempDir();
    printf("log_test (temp dir %s)\n", dir.c_str());
    TestCorrectness(dir);
    TestSink(dir);
    TestStatusLine(dir);
    TestStatBook();

    printf("\n== (b) caller-side cost per LOGI (us); child processes; 5000 short msgs @ 200 us, 50 x stats table @ 5 ms ==\n");
    printf("    %-14s %-7s %-4s %9s %9s %10s %9s\n", "stdout", "msg", "impl", "p50", "p99", "max", "mean");
    bool benchOk = true;
    for (const char* mode : {"nul", "file", "console", "console-status"}) {
        BenchResult br = RunChild(mode, dir);
        if (!br.ran) { printf("    %s: child failed\n", mode); benchOk = false; continue; }
        for (auto& r : br.rows)
            printf("    %-14s %-7s %-4s %9.2f %9.2f %10.1f %9.2f\n", mode, r.wl.c_str(), r.impl.c_str(), r.l.p50, r.l.p99, r.l.max, r.l.mean);
        printf("    %-14s stdout-is-console=%d enqueued=%llu dropped=%llu batches=%llu flush=%.1f ms allocs/LOGI=%.2f table=%zu B"
               " status-draws=%llu sink-calls=%llu\n", mode, br.isCon ? 1 : 0, br.enq, br.dropped, br.batches, br.flushMs, br.allocsPerShort,
               br.tableBytes, br.statusDraws, br.sinkCalls);
        const Lat* so = br.Find("short", "old"); const Lat* sn = br.Find("short", "new");
        const Lat* ro = br.Find("report", "old"); const Lat* rn = br.Find("report", "new");
        if (!so || !sn || !ro || !rn || !br.flushed || br.dropped != 0 || br.allocsPerShort != 0.0) benchOk = false;
        else if (!strncmp(mode, "console", 7) && (sn->p99 >= so->p99 || rn->p50 >= ro->p50)) benchOk = false;
        if (!strcmp(mode, "console-status") && (br.sinkCalls != br.enq || (br.isCon && br.statusDraws == 0))) benchOk = false;
        if (!strncmp(mode, "console", 7) && !br.isCon) printf("    note: the console child did not get a console handle\n");
    }
    Check(benchOk, "benchmark children ran, no drops, no allocations, async faster than sync on a console (also with status line + sink)");

    TestShutdown(dir);

    // Remove the temp files (kept on failure for inspection).
    if (!g_fail) {
        for (const char* f : {"log_a.txt", "bench_nul.txt", "bench_file.txt", "bench_console.txt", "bench_console-status.txt", "bench_stdout.txt",
                              "status_redirected.txt", "status_redirected_log.txt", "status_tty.txt", "status_tty_log.txt", "status_stdout.txt"})
            DeleteFileA((dir + "\\" + f).c_str());
        RemoveDirectoryA(dir.c_str());
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
