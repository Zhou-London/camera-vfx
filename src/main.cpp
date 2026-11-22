#include "app.h"
#include "selftest.h"
#include <avrt.h>
#include <exception>
#include <timeapi.h>

using namespace cameravfx;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitFatal = 2;
constexpr int kCloseFlushMs = 1000;
constexpr int kCrashFlushMs = 200;
constexpr UINT kTimerResolutionMs = 1;

BOOL WINAPI OnConsoleControl(DWORD event) {
    g_quitRequested = true;
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) LogFlush(kCloseFlushMs);
    return TRUE;
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    void* address = ep->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module))
        GetModuleFileNameA(module, name, MAX_PATH);
    RestoreConsoleAfterCrash();
    LogFlush(kCrashFlushMs);
    fprintf(stderr, "FATAL exception 0x%08lX at %p (%s+0x%llX)\n", ep->ExceptionRecord->ExceptionCode, address, name,
            (unsigned long long)((uintptr_t)address - (uintptr_t)module));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

class RenderThreadPriority {
public:
    RenderThreadPriority() {
        timeBeginPeriod(kTimerResolutionMs);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling)))
            LOGW("power throttling opt-out failed (%lu)", GetLastError());
        DWORD taskIndex = 0;
        mmcssTask_ = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (mmcssTask_) AvSetMmThreadPriority(mmcssTask_, AVRT_PRIORITY_HIGH);
        else LOGW("render thread: MMCSS registration failed (%lu)", GetLastError());
    }
    ~RenderThreadPriority() {
        if (mmcssTask_) AvRevertMmThreadCharacteristics(mmcssTask_);
        timeEndPeriod(kTimerResolutionMs);
    }
    RenderThreadPriority(const RenderThreadPriority&) = delete;
    RenderThreadPriority& operator=(const RenderThreadPriority&) = delete;

private:
    HANDLE mmcssTask_ = nullptr;
};

int RunWithOptions(const Options& o) {
    SetLogLevel(o.logLevel);
    if (!o.log.empty()) SetLogFile(o.log);
    if (o.list) {
        MfCapture::ListDevices();
        AudioMonitor::ListDevices();
        return kExitOk;
    }
    if (o.selftest) return RunSelfTest(o) ? kExitOk : kExitError;
    App app(o);
    if (!app.Init(GetModuleHandleW(nullptr))) {
        LOGE("init failed");
        return kExitError;
    }
    const RenderThreadPriority priority;
    return app.Run();
}

int Main(int argc, wchar_t** argv) {
    Options o;
    switch (LoadOptions(argc, argv, o)) {
    case ParseResult::Exit: return kExitOk;
    case ParseResult::Error: return kExitError;
    case ParseResult::Run: break;
    }
    return RunWithOptions(o);
}

}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    SetUnhandledExceptionFilter(OnCrash);
    std::set_terminate([] {
        LogFlush(kCrashFlushMs);
        std::abort();
    });
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HR_CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HR_CHECK(MFStartup(MF_VERSION));
    int rc = kExitOk;
    try {
        rc = Main(argc, argv);
    } catch (const std::exception& e) {
        LOGE("fatal: %s", e.what());
        LogFlush();
        rc = kExitFatal;
    }
    LogShutdown();
    MFShutdown();
    CoUninitialize();
    return rc;
}
