#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace cameravfx {

enum class LogLevel { Debug = 0, Info, Warn, Error };

namespace detail {
inline std::atomic<int> g_logLevel{(int)LogLevel::Info};
bool LogRateOk(std::atomic<int64_t>& last, double ms);
}

inline bool LogEnabled(LogLevel lvl) { return (int)lvl >= detail::g_logLevel.load(std::memory_order_relaxed); }
void SetLogLevel(LogLevel lvl);
void SetLogFile(const std::string& utf8Path);
void SetLogConsole(bool enabled);
void Log(LogLevel lvl, const char* fmt, ...);
bool LogFlush(int timeoutMs = 500);
void LogShutdown();

using LogSinkFn = void (*)(void* ctx, LogLevel lvl, double tSec, const char* msg, size_t len);
void SetLogSink(LogSinkFn fn, void* ctx);

bool ConsoleIsInteractive();

#define LOGD(...) ::cameravfx::Log(::cameravfx::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::cameravfx::Log(::cameravfx::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::cameravfx::Log(::cameravfx::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::cameravfx::Log(::cameravfx::LogLevel::Error, __VA_ARGS__)
#define LOG_EVERY_MS(lvl, ms, ...)                                                                      \
    do {                                                                                                \
        static std::atomic<int64_t> logRateLast{0};                                                     \
        if (::cameravfx::LogEnabled(lvl) && ::cameravfx::detail::LogRateOk(logRateLast, (double)(ms)))  \
            ::cameravfx::Log((lvl), __VA_ARGS__);                                                       \
    } while (0)

namespace style {
std::string Dim(std::string_view s);
std::string Bold(std::string_view s);
std::string Accent(std::string_view s);
}

std::string HrToString(HRESULT hr);
[[noreturn]] void ThrowHr(HRESULT hr, const char* expr, const char* file, int line);
#define HR_CHECK(expr) do { HRESULT _hr = (expr); if (FAILED(_hr)) ::cameravfx::ThrowHr(_hr, #expr, __FILE__, __LINE__); } while (0)

inline constexpr double kHnsPerSecond = 1e7;

struct FrameSize {
    unsigned width = 0, height = 0;
};

FrameSize FitAspect(FrameSize inner, FrameSize outer);

struct Qpc {
    static int64_t Freq() { static int64_t f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }(); return f; }
    static int64_t Now() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
    static double ToMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)Freq(); }
    static double ToSec(int64_t ticks) { return (double)ticks / (double)Freq(); }
    static int64_t FromMs(double ms) { return (int64_t)(ms * (double)Freq() / 1000.0); }
    static int64_t FromHns(int64_t hundredNs) { return (int64_t)((double)hundredNs * (double)Freq() / kHnsPerSecond); }
};

class PreciseSleeper {
public:
    PreciseSleeper();
    ~PreciseSleeper();
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;
    void SleepUntil(int64_t qpcDeadline, double spinMs = 0.3);
    double LastWakeLateMs() const { return lastWakeLateMs_; }
private:
    HANDLE timer_ = nullptr;
    double lastWakeLateMs_ = 0;
};

std::string Lower(std::string_view s);
std::optional<bool> ParseBool(std::string_view s);
bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part);

std::string WideToUtf8(const wchar_t* w);
std::wstring Utf8ToWide(const char* s);
FILE* OpenUtf8(const std::string& utf8Path, const wchar_t* mode);
std::wstring ExeDir();

}
