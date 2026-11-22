#include "check.h"
#include "util/common.h"
#include "util/stats.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace cameravfx;

static thread_local uint64_t t_allocations = 0;
void* operator new(size_t n) {
    t_allocations++;
    if (void* p = malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static void Check(bool ok, const char* what) { check::Expect(ok, "%s", what); }

static std::string ReadFileText(const std::string& path) {
    std::string s;
    FILE* f = OpenUtf8(path, L"rb");
    if (!f) return s;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
    fclose(f);
    return s;
}

static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    for (size_t b = 0; b < s.size();) {
        size_t e = s.find('\n', b);
        if (e == std::string::npos) e = s.size();
        size_t end = e;
        if (end > b && s[end - 1] == '\r') end--;
        out.emplace_back(s, b, end - b);
        b = e + 1;
    }
    return out;
}

static size_t CountOccurrences(const std::string& s, const char* sub) {
    size_t n = 0;
    const size_t len = strlen(sub);
    for (size_t p = 0; (p = s.find(sub, p)) != std::string::npos; p += len) n++;
    return n;
}

static uint64_t DroppedReported(const std::string& text) {
    uint64_t total = 0;
    for (const auto& l : SplitLines(text)) {
        const size_t p = l.find("[W] [log] ");
        unsigned long long n = 0;
        if (p != std::string::npos && sscanf(l.c_str() + p + 10, "%llu messages dropped", &n) == 1) total += n;
    }
    return total;
}

static std::string TempDir() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string d = std::string(tmp) + "cameravfx_log_test_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

struct OrderScan { int lines = 0, outOfOrder = 0; };
static OrderScan ScanPerThreadOrder(const std::string& text, const char* tag, int threads, bool noGaps) {
    OrderScan r;
    std::vector<int> next(threads, 0);
    const size_t tagLen = strlen(tag);
    for (const auto& l : SplitLines(text)) {
        const size_t p = l.find(tag);
        if (p == std::string::npos) continue;
        int t = -1, i = -1;
        if (sscanf(l.c_str() + p + tagLen, "%d %d", &t, &i) != 2 || t < 0 || t >= threads) { r.outOfOrder++; continue; }
        if (noGaps ? i != next[t] : i < next[t]) r.outOfOrder++;
        next[t] = i + 1;
        r.lines++;
    }
    return r;
}

static void LogFromThreads(int threads, int perThread, const char* tag) {
    std::vector<std::thread> th;
    for (int t = 0; t < threads; t++)
        th.emplace_back([=] { for (int i = 0; i < perThread; i++) LOGI("%s %d %d", tag, t, i); });
    for (auto& x : th) x.join();
}

struct SinkRecord { int lvl; double t; std::string msg; DWORD tid; };
struct SinkState {
    std::mutex m;
    std::vector<SinkRecord> records;
    std::atomic<int> inCall{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<int> sleepMs{0};
    std::atomic<bool> gateOpen{true};
    bool keep = true;
};
static void RecordingSink(void* ctx, LogLevel lvl, double t, const char* msg, size_t len) {
    auto* s = static_cast<SinkState*>(ctx);
    s->inCall++;
    if (int ms = s->sleepMs.load()) Sleep((DWORD)ms);
    while (!s->gateOpen.load()) Sleep(1);
    if (s->keep) {
        std::lock_guard<std::mutex> lk(s->m);
        s->records.push_back({(int)lvl, t, std::string(msg, len), GetCurrentThreadId()});
    }
    s->calls++;
    s->inCall--;
}

static std::atomic<int> g_reentrantCalls{0};
static void ReentrantSink(void*, LogLevel, double, const char* msg, size_t len) {
    g_reentrantCalls++;
    if (std::string(msg, len) == "trigger") {
        LOGI("from-inside-sink");
        SetLogSink(nullptr, nullptr);
    }
}

static bool WaitUntil(const std::atomic<int>& v, int value, double timeoutMs) {
    const int64_t t0 = Qpc::Now();
    while (v.load() != value) {
        if (Qpc::ToMs(Qpc::Now() - t0) > timeoutMs) return false;
        YieldProcessor();
    }
    return true;
}

struct StalledWriter {
    SinkState sink;
    StalledWriter() {
        sink.keep = false;
        sink.gateOpen = false;
        SetLogSink(RecordingSink, &sink);
        LOGI("stall-writer");
        stalled = WaitUntil(sink.inCall, 1, 2000);
    }
    void Release() {
        sink.gateOpen = true;
        LogFlush(5000);
        SetLogSink(nullptr, nullptr);
    }
    bool stalled = false;
};

static void TestCorrectness(const std::string& dir) {
    printf("\n== logger correctness ==\n");
    const std::string path = dir + "\\log_a.txt";
    SetLogConsole(false);
    SetLogFile(path);

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

    for (int i = 0; i < 200; i++) LOG_EVERY_MS(LogLevel::Info, 1000, "every-ms %d", i);

    std::string longMsg(5000, 'x');
    for (size_t i = 0; i < longMsg.size(); i += 100) longMsg[i] = (char)('a' + (i / 100) % 26);
    LOGI("long:%s:end", longMsg.c_str());

    constexpr int kThreads = 4, kPerThread = 10000;
    LogFromThreads(kThreads, kPerThread, "mt");
    Check(LogFlush(5000), "LogFlush returned true after the multi-thread burst");
    std::string text = ReadFileText(path);
    const uint64_t burstDropped = DroppedReported(text);
    const OrderScan burst = ScanPerThreadOrder(text, "] mt ", kThreads, burstDropped == 0);
    printf("    4 threads x %d LOGI: %d written, %llu dropped\n", kPerThread, burst.lines, (unsigned long long)burstDropped);
    Check((uint64_t)burst.lines + burstDropped == (uint64_t)(kThreads * kPerThread) && burst.outOfOrder == 0,
          "40000 messages per-thread ordered in the file, each written or reported as dropped");

    constexpr int kFitPerThread = 6000;
    {
        StalledWriter stall;
        LogFromThreads(kThreads, kFitPerThread, "mp");
        stall.Release();
        Check(stall.stalled, "writer stalled inside the sink");
    }
    text = ReadFileText(path);
    const OrderScan fit = ScanPerThreadOrder(text, "] mp ", kThreads, true);
    Check(DroppedReported(text) == burstDropped && fit.lines == kThreads * kFitPerThread && fit.outOfOrder == 0,
          "a 4-thread burst that fits the ring is complete and ordered while the writer is stalled");
    Check(CountOccurrences(text, "filtered-") == 0, "Debug/Info filtered at level Warn");
    Check(CountOccurrences(text, "kept-warn 1") == 1 && CountOccurrences(text, "kept-error 2") == 1 && CountOccurrences(text, "kept-info 3") == 1,
          "Warn/Error/Info kept");
    Check(CountOccurrences(text, "[W] kept-warn") == 1 && CountOccurrences(text, "[E] kept-error") == 1, "line format [%9.3f][W] msg");
    Check(CountOccurrences(text, "every-ms ") == 1, "LOG_EVERY_MS emits once per interval");
    Check(text.find("long:" + longMsg + ":end") != std::string::npos, "5000-char message intact");

    constexpr int kOverflow = 4000;
    const std::string payload(480, 'p');
    {
        StalledWriter stall;
        for (int i = 0; i < kOverflow; i++) LOGI("ovf %d %s", i, payload.c_str());
        stall.Release();
    }
    text = ReadFileText(path);
    const uint64_t dropped = DroppedReported(text) - burstDropped;
    const size_t overflowLines = CountOccurrences(text, "] ovf ");
    printf("    overflow: %d sent, %zu written, %llu dropped\n", kOverflow, overflowLines, (unsigned long long)dropped);
    Check(dropped > 0 && dropped < (uint64_t)kOverflow, "overflow drops (bounded ring)");
    Check(overflowLines + dropped == (size_t)kOverflow, "accepted messages all written after overflow");
    char expect[96];
    snprintf(expect, sizeof(expect), "[W] [log] %llu messages dropped", (unsigned long long)dropped);
    Check(CountOccurrences(text, expect) == 1, "\"[log] N messages dropped\" reported once with the right N");
    LOGI("after-overflow");
    LogFlush();
    text = ReadFileText(path);
    Check(CountOccurrences(text, "after-overflow") == 1, "logging resumes after overflow");
    Check(text.find('\x1b') == std::string::npos, "no escape codes in the log file");

    double flushMaxMs = 0;
    for (int i = 0; i < 50; i++) {
        LOGI("flush-probe %d", i);
        const int64_t t0 = Qpc::Now();
        LogFlush();
        flushMaxMs = std::max(flushMaxMs, Qpc::ToMs(Qpc::Now() - t0));
    }
    printf("    LogFlush after one message: max %.3f ms\n", flushMaxMs);
    Check(flushMaxMs < 100, "LogFlush is bounded and prompt");

    const uint64_t a0 = t_allocations;
    for (int i = 0; i < 1000; i++) LOGI("alloc-probe %d %.3f %s", i, i * 0.5, "abc");
    Check(t_allocations == a0, "LOGI performs no heap allocation on the calling thread");
    LogFlush();
}

static void TestSink(const std::string& dir) {
    printf("\n== log sink ==\n");
    SinkState st;
    SetLogSink(RecordingSink, &st);
    SetLogLevel(LogLevel::Warn);
    LOGI("sink-filtered %d", 0);
    LOGW("sink-w %d", 1);
    SetLogLevel(LogLevel::Info);
    LOGD("sink-filtered-debug");
    LOGI("sink-i %d", 2);
    LOGE("sink-e %d", 3);
    LOGI("sink-ml first\nsecond");
    constexpr int kThreads = 3, kPerThread = 5000;
    LogFromThreads(kThreads, kPerThread, "sk");
    const uint64_t a0 = t_allocations;
    for (int i = 0; i < 1000; i++) LOGI("sink-alloc-probe %d", i);
    const bool noAllocation = t_allocations == a0;
    LogFlush(5000);

    std::vector<SinkRecord> recs;
    {
        std::lock_guard<std::mutex> lk(st.m);
        recs = st.records;
    }
    auto indexOf = [&](const char* m) { for (size_t i = 0; i < recs.size(); i++) if (recs[i].msg == m) return (int)i; return -1; };
    const int iw = indexOf("sink-w 1"), ii = indexOf("sink-i 2"), ie = indexOf("sink-e 3"), iml = indexOf("sink-ml first\nsecond");
    Check(iw >= 0 && ii > iw && ie > ii && iml > ie, "sink gets the messages in order, plain text, multi-line as one call");
    Check(iw >= 0 && ie >= 0 && recs[iw].lvl == (int)LogLevel::Warn && recs[ii].lvl == (int)LogLevel::Info && recs[ie].lvl == (int)LogLevel::Error,
          "sink gets the level");
    bool filtered = true;
    for (const auto& r : recs) if (r.msg.find("sink-filtered") != std::string::npos) filtered = false;
    Check(filtered, "sink only gets messages that pass the level filter");
    std::vector<int> next(kThreads, 0);
    std::vector<double> lastT(kThreads, -1.0);
    int ordered = 0, bad = 0;
    bool timesOk = true, oneThread = true;
    const DWORD writerTid = recs.empty() ? 0 : recs[0].tid;
    for (const auto& r : recs) {
        if (r.tid != writerTid || r.tid == GetCurrentThreadId()) oneThread = false;
        int t = -1, i = -1;
        if (sscanf(r.msg.c_str(), "sk %d %d", &t, &i) != 2) continue;
        if (t < 0 || t >= kThreads || i != next[t]) { bad++; continue; }
        next[t] = i + 1;
        if (r.t < lastT[t]) timesOk = false;
        lastT[t] = r.t;
        ordered++;
    }
    Check(ordered == kThreads * kPerThread && bad == 0, "15000 messages from 3 threads complete and per-thread ordered");
    Check(oneThread, "sink called on the writer thread only");
    Check(timesOk, "sink timestamps non-decreasing per thread");
    Check(noAllocation, "LOGI does not allocate on the calling thread with a sink installed");

    st.sleepMs = 30;
    LOGI("sink-slow");
    const bool wasInCall = WaitUntil(st.inCall, 1, 2000);
    const int64_t c0 = Qpc::Now();
    SetLogSink(nullptr, nullptr);
    const double clearMs = Qpc::ToMs(Qpc::Now() - c0);
    const bool notInCall = st.inCall.load() == 0;
    st.sleepMs = 0;
    const uint64_t callsAfterClear = st.calls.load();
    for (int i = 0; i < 100; i++) LOGI("after-clear %d", i);
    LogFlush();
    printf("    SetLogSink(nullptr) during a 30 ms sink call returned after %.1f ms\n", clearMs);
    Check(wasInCall && notInCall, "SetLogSink(nullptr) waits for the call in progress");
    Check(st.calls.load() == callsAfterClear, "no sink calls after SetLogSink(nullptr) returned");

    g_reentrantCalls = 0;
    SetLogSink(ReentrantSink, nullptr);
    LOGI("trigger");
    LOGI("after-trigger");
    bool flushed = LogFlush(3000);
    flushed = LogFlush(3000) && flushed;
    const std::string text = ReadFileText(dir + "\\log_a.txt");
    Check(flushed && g_reentrantCalls.load() == 1 && CountOccurrences(text, "] from-inside-sink") == 1 &&
              CountOccurrences(text, "] after-trigger") == 1,
          "sink may log and clear itself from inside");
    Check(CountOccurrences(text, "] after-clear ") == 100, "messages after clearing still reach the file");
    SetLogSink(nullptr, nullptr);
}

static void TestStatBook() {
    printf("\n== StatBook ==\n");
    StatBook book;
    for (int i = 1; i <= 100; i++) book.Add("lat.arrival_to_flip_ms", (double)i);
    book.Add("cap.upload_ms", 0.5);
    const std::optional<Percentiles> flip = book.Get("lat.arrival_to_flip_ms");
    Check(flip && flip->p50 == 51 && flip->p99 == 99 && !book.Get("missing"), "Get() nearest-rank p50 / p99");
    const std::string report = book.Report();
    Check(report.find("lat.arrival_to_flip_ms") < report.find("cap.upload_ms") && report.find("100.000") != std::string::npos,
          "Report() in insertion order with the max");
    book.Clear();
    Check(!book.Get("lat.arrival_to_flip_ms") && book.Report().find("cap.upload_ms") == std::string::npos, "Clear()");
}

static void TestShutdown(const std::string& dir) {
    printf("\n== shutdown / sync fallback ==\n");
    LOGI("before-shutdown");
    constexpr int kThreads = 3, kAfter = 300;
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; t++)
            th.emplace_back([&, t] {
                started++;
                for (int i = 0, extra = 0; extra < kAfter; i++) {
                    LOGI("so %d %d", t, i);
                    if (stop.load()) extra++;
                }
            });
        while (started.load() < kThreads) Sleep(0);
        Sleep(5);
        LogShutdown();
        stop = true;
        for (auto& x : th) x.join();
    }
    LogShutdown();
    LOGW("after-shutdown %d", 7);
    const bool flushed = LogFlush(10);
    const std::string text = ReadFileText(dir + "\\log_a.txt");
    Check(CountOccurrences(text, "before-shutdown") == 1, "shutdown flushed queued messages");
    Check(CountOccurrences(text, "[W] after-shutdown 7") == 1 && flushed, "logging works synchronously after LogShutdown");
    const OrderScan so = ScanPerThreadOrder(text, "] so ", kThreads, false);
    printf("    logging across LogShutdown: %d lines, %d out of order\n", so.lines, so.outOfOrder);
    Check(so.lines >= kThreads * kAfter && so.outOfOrder == 0, "per-thread order kept across LogShutdown (async -> sync)");
    SetLogFile("");
}

static const int g_preMain = (LOGD("pre-main static-init message (Debug, filtered)"), LogEnabled(LogLevel::Info) ? 1 : 0);

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dir = TempDir();
    printf("log_test (temp dir %s)\n", dir.c_str());
    TestCorrectness(dir);
    TestSink(dir);
    TestStatBook();
    TestShutdown(dir);
    if (!check::g_failures) {
        DeleteFileA((dir + "\\log_a.txt").c_str());
        RemoveDirectoryA(dir.c_str());
    }
    return check::Summary();
}
