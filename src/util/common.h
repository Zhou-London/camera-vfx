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
#include <string_view>
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
// one console write per batch (styled, see below, when the console supports VT
// sequences, never in redirected output or the log file) and buffered log-file
// writes (flushed every 250 ms, immediately for Warn/Error). When the ring is full
// messages are dropped and counted; "[log] N messages dropped" is emitted once the
// writer catches up. Before the writer has started / after LogShutdown() LogV
// writes synchronously. Normal process exit (return from main / exit()) flushes.
//
// Console look: on a console with VT support each message is one calm line
//     "   12.345 · message"      (dim time, dim '·' Info, yellow '▲' Warn, red '×' Error,
//                                  Debug lines entirely dim; continuation lines indented)
// Redirected output and the log file keep the plain "[%9.3f][I] message" format.
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
// Console (stdout/stderr) output on/off; the log file and the sink are unaffected.
// SetLogConsole(false) is a barrier: it waits for a console write in progress and erases
// the status line, so nothing reaches the console after it returns (a dashboard may then
// own the screen). Batches already formatted for the console are dropped from it.
void SetLogConsole(bool enabled);
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

// Log sink: called on the writer thread for every message that passes the level filter,
// in order, with the plain message text (no prefix, no colour, no trailing newline; a
// multi-line message is passed as one call), whether or not console output is enabled.
// Calls are serialised (never concurrent). After LogShutdown() the (rare) synchronous
// messages are passed on the logging thread instead. SetLogSink(nullptr, nullptr) waits
// for a call in progress: no call starts after it returns (it may block for one batch;
// never call it from the render thread). The sink may log (the message is queued for the
// next batch) and may call SetLogSink itself. Keep it short (e.g. copy into the UI's own
// buffer): it delays the console / file write of the batch.
using LogSinkFn = void (*)(void* ctx, LogLevel lvl, double tSec, const char* msg, size_t len);
void SetLogSink(LogSinkFn fn, void* ctx);

// Pinned status line (interactive console only: stdout is a console with VT sequences).
// The writer thread keeps the text as the last line on screen: log lines scroll above it.
// SetStatusLine only stores the text and wakes the writer (a short lock + string copy,
// never waits for console I/O); new text is drawn at most ~10 times per second (log
// batches erase and redraw the current text in the same console write), truncated to the
// console width. Empty text removes it. It may contain style:: colours (SGR only); other
// control characters (newline, CR, tab, ...) are replaced by spaces. Never written to the log file or the sink. Does nothing
// when stdout is not a VT console or after LogShutdown. SetLogConsole(false) and
// LogShutdown erase it first (cursor left at column 0); SetLogConsole(true) restores it.
void SetStatusLine(std::string_view utf8);
// true when stdout is a console with VT processing (pinned status line and colours work).
bool LogConsoleIsTty();

struct LogCounters {
    uint64_t enqueued = 0;   // messages accepted into the ring
    uint64_t dropped = 0;    // messages lost because the ring was full
    uint64_t written = 0;    // messages written by the writer thread
    uint64_t batches = 0;    // writer batches (= console writes when console output is on)
    uint64_t statusDraws = 0;   // console writes that (re)drew the status line
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

// ---------------------------------------------------------------- console styling
// Tiny SGR helpers for console text (status line, --help, dashboard). They return the
// text unchanged when stdout is not a VT console (redirected, legacy console), so their
// result can always be printed. Colours end with a foreground reset (39) / bold-off (22),
// so they nest inside each other without clearing the outer style completely.
namespace style {
bool Enabled();                          // == LogConsoleIsTty()
std::string Dim(std::string_view s);     // grey (bright black): labels, defaults, timestamps
std::string Bold(std::string_view s);
std::string Accent(std::string_view s);  // warm orange: 24-bit in Windows Terminal, else 256-colour 173
std::string Ok(std::string_view s);      // green
std::string Warn(std::string_view s);    // yellow
std::string Err(std::string_view s);     // red
// Visible width in console columns of UTF-8 text, skipping escape sequences: CJK / emoji
// count 2, combining marks 0. East-Asian "ambiguous" symbols (· × ▲ ─ …) count 1 as in
// Windows Terminal, or 2 with ambiguousWide (CJK-font conhost renders them wide).
int Width(std::string_view s, bool ambiguousWide = false);
} // namespace style

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
