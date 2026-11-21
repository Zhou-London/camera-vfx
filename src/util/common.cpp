#include "common.h"
#include <comdef.h>
#include <vector>
#include <psapi.h>

namespace camss {

static std::atomic<int> g_logLevel{(int)LogLevel::Info};
static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;

void SetLogLevel(LogLevel lvl) { g_logLevel = (int)lvl; }

void SetLogFile(const char* path) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = path ? fopen(path, "w") : nullptr;
}

void LogV(LogLevel lvl, const char* fmt, va_list ap) {
    if ((int)lvl < g_logLevel.load()) return;
    static const char* tags[] = {"D", "I", "W", "E"};
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    static const int64_t t0 = Qpc::Now();
    double t = Qpc::ToSec(Qpc::Now() - t0);
    std::lock_guard<std::mutex> lk(g_logMutex);
    FILE* out = (lvl >= LogLevel::Warn) ? stderr : stdout;
    fprintf(out, "[%9.3f][%s] %s\n", t, tags[(int)lvl], msg);
    fflush(out);
    if (g_logFile) { fprintf(g_logFile, "[%9.3f][%s] %s\n", t, tags[(int)lvl], msg); fflush(g_logFile); }
}

std::string HrToString(HRESULT hr) {
    char buf[512] = {};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), buf, sizeof(buf), nullptr);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    char out[640];
    snprintf(out, sizeof(out), "0x%08lX %s", (unsigned long)hr, n ? buf : "");
    return out;
}

void ThrowHr(HRESULT hr, const char* expr, const char* file, int line) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s failed: %s (%s:%d)", expr, HrToString(hr).c_str(), file, line);
    LOGE("%s", buf);
    throw std::runtime_error(buf);
}

PreciseSleeper::PreciseSleeper() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
}
PreciseSleeper::~PreciseSleeper() { if (timer_) CloseHandle(timer_); }

void PreciseSleeper::SleepUntil(int64_t deadline, double spinMs) {
    int64_t now = Qpc::Now();
    int64_t spin = Qpc::FromMs(spinMs);
    if (deadline - now > spin && timer_) {
        int64_t waitTicks = deadline - now - spin;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((double)waitTicks * 1e7 / (double)Qpc::Freq()); // relative, 100ns
        if (due.QuadPart < 0 && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer_, INFINITE);
    }
    while (Qpc::Now() < deadline) YieldProcessor();
}

void DumpModules(const wchar_t* filter, const char* when) {
    HMODULE mods[2048]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        wchar_t m[MAX_PATH]; GetModuleFileNameW(mods[i], m, MAX_PATH);
        if (wcsstr(m, filter)) LOGI("[%s] module %s", when, WideToUtf8(m).c_str());
    }
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

} // namespace camss
