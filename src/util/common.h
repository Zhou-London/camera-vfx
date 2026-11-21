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
enum class LogLevel { Debug = 0, Info, Warn, Error };
void SetLogLevel(LogLevel lvl);
void SetLogFile(const char* path);
void LogV(LogLevel lvl, const char* fmt, va_list ap);
inline void Log(LogLevel lvl, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); LogV(lvl, fmt, ap); va_end(ap);
}
#define LOGD(...) ::camss::Log(::camss::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::camss::Log(::camss::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::camss::Log(::camss::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::camss::Log(::camss::LogLevel::Error, __VA_ARGS__)

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
