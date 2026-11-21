// Common helpers: COM smart pointers, HRESULT checks, logging, QPC clock.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace camss {

// ---------------------------------------------------------------- logging
// Asynchronous logger. The calling thread only checks the level, formats the
// message (timestamp taken at call time) and copies it into a bounded 1 MB ring
// under a very short lock. A background writer thread drains the ring in batches:
// one console write per batch (Warn/Error yellow/red when the console supports VT
// sequences, never in redirected output or the log file) and buffered log-file
// writes (flushed every 250 ms, immediately for Warn/Error). When the ring is full
// messages are dropped and counted; "[log] N messages dropped" is emitted once the
// writer catches up. Before the writer has started / after LogShutdown() LogV
// writes synchronously. Normal process exit (return from main / exit()) flushes.
enum class LogLevel { Debug = 0, Info, Warn, Error };

namespace detail {
inline std::atomic<int> g_logLevel{(int)LogLevel::Info};
bool LogRateOk(std::atomic<int64_t>& last, double ms);
}

inline bool LogEnabled(LogLevel lvl) { return (int)lvl >= detail::g_logLevel.load(std::memory_order_relaxed); }
void SetLogLevel(LogLevel lvl);
// "debug|info|warn|error" (case-insensitive; also "d|i|w|e", "warning"); fallback otherwise.
LogLevel ParseLogLevel(const char* s, LogLevel fallback);
void SetLogFile(const char* path);          // nullptr closes; messages queued before the call are not written to the new file
void SetLogConsole(bool enabled);           // console (stdout/stderr) output on/off; the log file is unaffected
void LogV(LogLevel lvl, const char* fmt, va_list ap);
inline void Log(LogLevel lvl, const char* fmt, ...) {
    if (!LogEnabled(lvl)) return;
    va_list ap; va_start(ap, fmt); LogV(lvl, fmt, ap); va_end(ap);
}
// Blocks until everything logged before the call is written (console + file flushed),
// at most timeoutMs. Returns false on timeout.
bool LogFlush(int timeoutMs = 500);
// Flushes and stops the writer thread; later messages are written synchronously. Idempotent.
void LogShutdown();

struct LogCounters {
    uint64_t enqueued = 0;   // messages accepted into the ring
    uint64_t dropped = 0;    // messages lost because the ring was full
    uint64_t written = 0;    // messages written by the writer thread
    uint64_t batches = 0;    // writer batches (= console writes when console output is on)
};
LogCounters GetLogCounters();

namespace detail {
void LogTestPauseWriter(bool paused);       // test hook: stop draining (to exercise overflow)
}

#define LOGD(...) ::camss::Log(::camss::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::camss::Log(::camss::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::camss::Log(::camss::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::camss::Log(::camss::LogLevel::Error, __VA_ARGS__)
// Rate-limited log: at most one message per `ms` milliseconds per call site.
// e.g. LOG_EVERY_MS(::camss::LogLevel::Warn, 1000, "late frame %d", n);
#define LOG_EVERY_MS(lvl, ms, ...)                                                                   \
    do {                                                                                             \
        static std::atomic<int64_t> _camss_log_last{0};                                              \
        if (::camss::LogEnabled(lvl) && ::camss::detail::LogRateOk(_camss_log_last, (double)(ms)))   \
            ::camss::Log((lvl), __VA_ARGS__);                                                        \
    } while (0)

// ---------------------------------------------------------------- errors
std::string HrToString(HRESULT hr);
[[noreturn]] void ThrowHr(HRESULT hr, const char* expr, const char* file, int line);
#define HR_CHECK(expr) do { HRESULT _hr = (expr); if (FAILED(_hr)) ::camss::ThrowHr(_hr, #expr, __FILE__, __LINE__); } while (0)

// ---------------------------------------------------------------- time
// All timestamps inside camss are QPC ticks (int64). Helpers convert to ms.
struct Qpc {
    static int64_t Freq() { static int64_t f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }(); return f; }
    static int64_t Now() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
    static double ToMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)Freq(); }
    static double ToSec(int64_t ticks) { return (double)ticks / (double)Freq(); }
    static int64_t FromMs(double ms) { return (int64_t)(ms * (double)Freq() / 1000.0); }
    static int64_t FromHns(int64_t hns) { return (int64_t)((double)hns * (double)Freq() / 1e7); } // 100ns units
};

// High resolution sleep until an absolute QPC deadline. Uses a high-resolution
// waitable timer for the bulk, then spins for the final ~0.3 ms.
class PreciseSleeper {
public:
    PreciseSleeper();
    ~PreciseSleeper();
    void SleepUntil(int64_t qpcDeadline, double spinMs = 0.3);
private:
    HANDLE timer_ = nullptr;
};

std::string WideToUtf8(const wchar_t* w);
void DumpModules(const wchar_t* filter, const char* when);
std::wstring Utf8ToWide(const char* s);

} // namespace camss
