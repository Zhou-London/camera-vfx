#include "audio/audio_monitor.h"
#include "capture/mf_capture.h"
#include <atomic>
#include <optional>
#include <thread>
#include <vector>

using namespace cameravfx;

namespace {

constexpr int kExitUsage = 1;
constexpr int kExitStartFailed = 2;
constexpr int kStatsPerSecond = 10;
constexpr int kWarmupSeconds = 5;
constexpr float kQuietVolume = 0.25f;
constexpr double kBufferMs = 10;

std::atomic<bool> quitRequested{false};

BOOL WINAPI OnConsoleCtrl(DWORD) {
    quitRequested = true;
    return TRUE;
}

std::optional<VideoDevice> FindVideoDevice(const std::wstring& namePart) {
    for (const VideoDevice& d : VideoDevices())
        if (ContainsIgnoreCase(d.name, namePart)) return d;
    return std::nullopt;
}

void ListDevices() {
    for (const VideoDevice& d : VideoDevices())
        printf("video \"%s\"\n      %s\n", WideToUtf8(d.name.c_str()).c_str(), WideToUtf8(d.symbolicLink.c_str()).c_str());
    AudioMonitor::ListDevices();
}

void PrintUsage() {
    printf("audio_test --list\n"
           "audio_test [--video NAME] [--capture SUBSTR] [--render SUBSTR] [--seconds N] [--video-latency MS]\n"
           "           [--volume LINEAR] [--buffer MS] [--offset MS] [--no-sync] [--tone] [--debug]\n");
}

int Monitor(const AudioOptions& options, double seconds, double videoLatencyMs) {
    AudioMonitor monitor;
    const int64_t startT = Qpc::Now();
    if (!monitor.Start(options)) {
        printf("RESULT: Start() returned false (%.1f ms)\n", Qpc::ToMs(Qpc::Now() - startT));
        return kExitStartFailed;
    }
    AudioStats s = monitor.Stats();
    printf("Start() ok in %.1f ms: capture \"%s\" -> render \"%s\"\n", Qpc::ToMs(Qpc::Now() - startT), s.captureName.c_str(),
           s.renderName.c_str());
    double latencySum = 0, bufferSum = 0, bufferMin = 1e9, bufferMax = 0;
    int samples = 0;
    for (int sec = 1; sec <= (int)seconds && !quitRequested; sec++) {
        for (int k = 0; k < kStatsPerSecond && !quitRequested; k++) {
            if (videoLatencyMs > 0) monitor.SetVideoLatencyMs(videoLatencyMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kStatsPerSecond));
        }
        s = monitor.Stats();
        printf("[%3ds] %s %s latency %6.1f ms  buffer %6.1f ms  sync %5.1f ms  underruns %llu\n", sec, s.streaming ? "run " : "down",
               s.playing ? "play" : "prime", s.latencyMs, s.bufferMs, s.syncDelayMs, (unsigned long long)s.underruns);
        fflush(stdout);
        if (sec > kWarmupSeconds && s.playing) {
            latencySum += s.latencyMs;
            bufferSum += s.bufferMs;
            bufferMin = std::min(bufferMin, s.bufferMs);
            bufferMax = std::max(bufferMax, s.bufferMs);
            samples++;
        }
    }
    if (samples > 0)
        printf("SUMMARY (after %d s, %d samples): latency %.1f ms, buffer %.1f ms (min %.1f max %.1f), sync %.1f ms, underruns %llu\n",
               kWarmupSeconds, samples, latencySum / samples, bufferSum / samples, bufferMin, bufferMax, s.syncDelayMs,
               (unsigned long long)s.underruns);
    const int64_t stopT = Qpc::Now();
    monitor.Stop();
    printf("Stop() took %.1f ms\n", Qpc::ToMs(Qpc::Now() - stopT));
    return 0;
}

}

int wmain(int argc, wchar_t** argv) {
    bool list = false;
    double seconds = 20, videoLatencyMs = 0;
    std::wstring video;
    AudioOptions options;
    options.volume = kQuietVolume;
    options.bufferMs = kBufferMs;
    options.autoSync = true;
    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        const auto value = [&]() -> const wchar_t* {
            if (i + 1 >= argc) { PrintUsage(); exit(kExitUsage); }
            return argv[++i];
        };
        if (arg == L"--list") list = true;
        else if (arg == L"--seconds") seconds = _wtof(value());
        else if (arg == L"--capture") options.captureDevice = value();
        else if (arg == L"--render") options.renderDevice = value();
        else if (arg == L"--video") video = value();
        else if (arg == L"--video-latency") videoLatencyMs = _wtof(value());
        else if (arg == L"--volume") options.volume = (float)_wtof(value());
        else if (arg == L"--buffer") options.bufferMs = _wtof(value());
        else if (arg == L"--offset") options.offsetMs = _wtof(value());
        else if (arg == L"--no-sync") options.autoSync = false;
        else if (arg == L"--tone") options.testTone = true;
        else if (arg == L"--debug") SetLogLevel(LogLevel::Debug);
        else { PrintUsage(); return kExitUsage; }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    int rc = 0;
    if (list) {
        ListDevices();
    } else {
        if (!video.empty()) {
            if (const std::optional<VideoDevice> d = FindVideoDevice(video)) {
                printf("video device \"%s\"\n  symbolic link %s\n", WideToUtf8(d->name.c_str()).c_str(), WideToUtf8(d->symbolicLink.c_str()).c_str());
                options.videoDeviceName = d->name;
                options.videoSymbolicLink = d->symbolicLink;
            } else {
                printf("video device \"%s\" not found; name match only\n", WideToUtf8(video.c_str()).c_str());
                options.videoDeviceName = video;
            }
        }
        rc = Monitor(options, seconds, videoLatencyMs);
    }
    MFShutdown();
    CoUninitialize();
    return rc;
}
