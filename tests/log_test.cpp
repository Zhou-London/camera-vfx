// Logger + StatBook test and benchmark.
//  (a) correctness: 4 threads x 10000 messages complete and per-thread ordered in the
//      log file, overflow drops + "[log] N messages dropped", LogFlush, level filter,
//      LOG_EVERY_MS, long messages, no escape codes in the file, sync fallback after
//      LogShutdown.
//  (b) caller-side LOGI cost (p50/p99/max us) vs. the old synchronous implementation,
//      measured in child processes whose stdout is NUL, a file, and a real (windowless)
//      console.
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
    uint64_t allocs0 = t_allocs;
    auto shortNew = MeasurePaced(nShort, shortSpacingUs, [&](int i) {
        LOGI("frame %d presented late by %.3f ms (slot %d, queue %d)", i, 0.123 * (i % 7), i % 3, i % 5);
    });
    const double allocsPerShort = (double)(t_allocs - allocs0) / nShort;
    auto repNew = MeasurePaced(nRep, repSpacingUs, [&](int) { LOGI("%s\n%s", head, table.c_str()); });
    const int64_t f0 = Qpc::Now();
    const bool flushed = LogFlush(20000);
    const double flushMs = Qpc::ToMs(Qpc::Now() - f0);
    const LogCounters c = GetLogCounters();

    auto put = [&](const char* wl, const char* impl, const Lat& l) {
        fprintf(res, "%s %s %s %.3f %.3f %.3f %.3f\n", mode, wl, impl, l.p50, l.p99, l.max, l.mean);
    };
    put("short", "old", shortOld);
    put("short", "new", shortNew);
    put("report", "old", repOld);
    put("report", "new", repNew);
    fprintf(res, "info %d %llu %llu %llu %d %.1f %.3f %zu\n", isCon ? 1 : 0, (unsigned long long)c.enqueued,
            (unsigned long long)c.dropped, (unsigned long long)c.batches, flushed ? 1 : 0, flushMs, allocsPerShort, table.size());
    fclose(res);
    LogShutdown();
    return 0;
}

struct BenchRow { std::string wl, impl; Lat l; };
struct BenchResult {
    bool ran = false, isCon = false, flushed = false;
    unsigned long long enq = 0, dropped = 0, batches = 0;
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
    if (!strcmp(mode, "console")) {
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
        if (sscanf(line.c_str(), "info %d %llu %llu %llu %d %lf %lf %zu", &con, &br.enq, &br.dropped, &br.batches, &fl, &br.flushMs,
                   &br.allocsPerShort, &br.tableBytes) == 8) {
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

    // Multi-threaded: 4 x 10000, complete and per-thread ordered.
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

    std::string text = ReadFileStr(path);
    auto lines = SplitLines(text);
    std::vector<int> next(kThreads, 0);
    int bad = 0, mtLines = 0;
    for (auto& l : lines) {
        size_t p = l.find("] mt ");
        if (p == std::string::npos) continue;
        int t = -1, i = -1;
        if (sscanf(l.c_str() + p + 5, "%d %d", &t, &i) != 2 || t < 0 || t >= kThreads) { bad++; continue; }
        if (i != next[t]) bad++;
        next[t] = i + 1;
        mtLines++;
    }
    Check(c1.dropped == c0.dropped, "no drops during the multi-thread burst");
    Check(mtLines == kThreads * kPer && bad == 0, "40000 messages complete and per-thread ordered in the file");
    if (mtLines != kThreads * kPer || bad) printf("    got %d lines, %d out of order\n", mtLines, bad);
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
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string dir = TempDir();
    printf("log_test (temp dir %s)\n", dir.c_str());
    TestCorrectness(dir);
    TestStatBook();

    printf("\n== (b) caller-side cost per LOGI (us); child processes; 5000 short msgs @ 200 us, 50 x stats table @ 5 ms ==\n");
    printf("    %-8s %-7s %-4s %9s %9s %10s %9s\n", "stdout", "msg", "impl", "p50", "p99", "max", "mean");
    bool benchOk = true;
    for (const char* mode : {"nul", "file", "console"}) {
        BenchResult br = RunChild(mode, dir);
        if (!br.ran) { printf("    %s: child failed\n", mode); benchOk = false; continue; }
        for (auto& r : br.rows)
            printf("    %-8s %-7s %-4s %9.2f %9.2f %10.1f %9.2f\n", mode, r.wl.c_str(), r.impl.c_str(), r.l.p50, r.l.p99, r.l.max, r.l.mean);
        printf("    %-8s stdout-is-console=%d enqueued=%llu dropped=%llu batches=%llu flush=%.1f ms allocs/LOGI=%.2f table=%zu B\n", mode,
               br.isCon ? 1 : 0, br.enq, br.dropped, br.batches, br.flushMs, br.allocsPerShort, br.tableBytes);
        const Lat* so = br.Find("short", "old"); const Lat* sn = br.Find("short", "new");
        const Lat* ro = br.Find("report", "old"); const Lat* rn = br.Find("report", "new");
        if (!so || !sn || !ro || !rn || !br.flushed || br.dropped != 0 || br.allocsPerShort != 0.0) benchOk = false;
        else if (!strcmp(mode, "console") && (sn->p99 >= so->p99 || rn->p50 >= ro->p50)) benchOk = false;
        if (!strcmp(mode, "console") && !br.isCon) printf("    note: the console child did not get a console handle\n");
    }
    Check(benchOk, "benchmark children ran, no drops, no allocations, async faster than sync on a console");

    TestShutdown(dir);

    // Remove the temp files (kept on failure for inspection).
    if (!g_fail) {
        for (const char* f : {"log_a.txt", "bench_nul.txt", "bench_file.txt", "bench_console.txt", "bench_stdout.txt"})
            DeleteFileA((dir + "\\" + f).c_str());
        RemoveDirectoryA(dir.c_str());
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
