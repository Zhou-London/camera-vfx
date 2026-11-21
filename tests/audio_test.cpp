// HDMI audio monitor test against the REAL devices.
//   audio_test --list
//   audio_test --video "UGREEN 25173" [--seconds 30] [--video-latency 40] [--volume 0.3]
//   audio_test --capture "HDMI" [--render "Speakers"] [--tone] [--buffer MS] [--delay MS] [--no-sync]
//              [--debug] [--drop-insert] [--mute-toggle] [--video-latency2 MS] [--cycles N] [--stall MS]
// Prints AudioStats once per second; Ctrl+C stops early. Exit code: 0 ok, 1 usage, 2 start failed.
#include "audio/audio_monitor.h"
#include <mfapi.h>
#include <mfidl.h>
#include <tlhelp32.h>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <thread>

using namespace camss;

static std::atomic<bool> g_quit{false};

static BOOL WINAPI OnCtrl(DWORD) { g_quit = true; return TRUE; }

// Fault injection: freeze the monitor's audio thread (found by its thread description) for `ms`,
// like a long system stall. The render side starves, capture data piles up.
static bool StallAudioThread(int ms) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{sizeof(te)};
    bool done = false;
    for (BOOL ok = Thread32First(snap, &te); ok && !done; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc && wcscmp(desc, L"camss audio") == 0) {
            SuspendThread(h);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            ResumeThread(h);
            done = true;
        }
        LocalFree(desc);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return done;
}

static std::wstring Upper(std::wstring s) { for (auto& c : s) c = (wchar_t)std::towupper(c); return s; }

// Enumerates MF video capture devices like the app does; returns the first whose friendly name
// contains `want` (case-insensitive) with its symbolic link.
static bool FindVideoDevice(const std::wstring& want, std::wstring& name, std::wstring& link, bool print) {
    ComPtr<IMFAttributes> a;
    if (FAILED(MFCreateAttributes(&a, 1))) return false;
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(a.Get(), &devs, &count))) return false;
    bool found = false;
    for (UINT32 i = 0; i < count; i++) {
        WCHAR* n = nullptr; WCHAR* l = nullptr; UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &n, &len);
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &l, &len);
        const std::wstring sn = n ? n : L"", sl = l ? l : L"";
        if (print) printf("video [%u] %s\n        %s\n", i, WideToUtf8(sn.c_str()).c_str(), WideToUtf8(sl.c_str()).c_str());
        if (!found && !want.empty() && Upper(sn).find(Upper(want)) != std::wstring::npos) { name = sn; link = sl; found = true; }
        CoTaskMemFree(n); CoTaskMemFree(l);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return found;
}

static void Usage() {
    printf("audio_test [--list] [--seconds N] [--capture SUBSTR] [--render SUBSTR] [--video NAME]\n"
           "           [--video-latency MS] [--video-latency2 MS] [--volume LINEAR] [--buffer MS] [--delay MS] [--no-sync] [--tone]\n"
           "           [--debug] [--drop-insert] [--mute-toggle]\n");
}

int wmain(int argc, wchar_t** argv) {
    bool list = false, muteToggle = false;
    int cycles = 0, stallMs = 0;
    double seconds = 20, videoLatency = 0, videoLatency2 = -1;
    AudioOptions o;
    o.volume = 0.25f;   // keep the test quiet unless asked otherwise
    std::wstring video;
    for (int i = 1; i < argc; i++) {
        const std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* { if (i + 1 >= argc) { Usage(); exit(1); } return argv[++i]; };
        if (a == L"--list") list = true;
        else if (a == L"--seconds") seconds = _wtof(next());
        else if (a == L"--capture") o.captureDevice = next();
        else if (a == L"--render") o.renderDevice = next();
        else if (a == L"--video") video = next();
        else if (a == L"--video-latency") videoLatency = _wtof(next());
        else if (a == L"--video-latency2") videoLatency2 = _wtof(next());   // switched to at half time
        else if (a == L"--volume") o.volume = (float)_wtof(next());
        else if (a == L"--buffer") o.bufferMs = _wtof(next());
        else if (a == L"--delay") o.delayMs = _wtof(next());
        else if (a == L"--no-sync") o.autoSync = false;
        else if (a == L"--tone") o.testTone = true;
        else if (a == L"--debug") SetLogLevel(LogLevel::Debug);
        else if (a == L"--drop-insert") o.forceDropInsert = true;
        else if (a == L"--mute-toggle") muteToggle = true;   // muted during odd seconds
        else if (a == L"--cycles") cycles = _wtoi(next());   // start/stop N times before the main run
        else if (a == L"--stall") stallMs = _wtoi(next());   // freeze the audio thread at 5 s and 10 s
        else { Usage(); return 1; }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    int rc = 0;

    if (list) {
        std::wstring n, l;
        FindVideoDevice(L"", n, l, true);
        AudioMonitor::ListDevices();
    } else {
        if (!video.empty()) {
            std::wstring name, link;
            if (FindVideoDevice(video, name, link, false)) {
                printf("video device \"%s\"\n  symbolic link %s\n", WideToUtf8(name.c_str()).c_str(), WideToUtf8(link.c_str()).c_str());
                o.videoDeviceName = name;
                o.videoSymbolicLink = link;
            } else {
                printf("video device \"%s\" not found; name match only\n", WideToUtf8(video.c_str()).c_str());
                o.videoDeviceName = video;
            }
        }
        AudioMonitor am;
        // start/stop cycling: Stop() must return quickly whatever the thread is doing (opening, priming, playing)
        double worstStop = 0;
        for (int c = 0; c < cycles && !g_quit; c++) {
            if (!am.Start(o)) { printf("cycle %d: Start() failed\n", c); rc = 2; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds((c * 37) % 300));
            const int64_t ts = Qpc::Now();
            am.Stop();
            worstStop = std::max(worstStop, Qpc::ToMs(Qpc::Now() - ts));
        }
        if (cycles > 0) printf("%d start/stop cycles, worst Stop() %.1f ms\n", cycles, worstStop);
        const int64_t t0 = Qpc::Now();
        if (rc != 0) {
        } else if (!am.Start(o)) {
            printf("RESULT: Start() returned false (%.1f ms)\n", Qpc::ToMs(Qpc::Now() - t0));
            rc = 2;
        } else {
            AudioStats s = am.Stats();
            printf("Start() ok in %.1f ms: capture \"%s\" -> render \"%s\", matched by %s\n", Qpc::ToMs(Qpc::Now() - t0),
                   s.captureName.c_str(), s.renderName.c_str(), s.matchMethod.c_str());
            double sumLat = 0, sumBuf = 0, minBuf = 1e9, maxBuf = 0;
            int n = 0;
            for (int sec = 1; sec <= (int)seconds && !g_quit; sec++) {
                for (int k = 0; k < 10 && !g_quit; k++) {
                    const double vl = (videoLatency2 >= 0 && sec > (int)seconds / 2) ? videoLatency2 : videoLatency;
                    if (vl > 0) am.SetVideoLatencyMs(vl);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (muteToggle) am.SetMuted(sec % 2 == 1);
                if (stallMs > 0 && (sec == 5 || sec == 10))
                    printf("  -- audio thread frozen for %d ms: %s\n", stallMs, StallAudioThread(stallMs) ? "done" : "thread not found");
                s = am.Stats();
                const double db = s.peak > 0 ? 20.0 * std::log10(s.peak) : -120.0;
                printf("[%3ds] %s %s lat %6.1f ms  buf %6.1f / %6.1f ms  sync %5.1f  ppm %+8.1f  under %llu over %llu disc %llu glitch %llu reopen %llu  peak %6.1f dBFS\n",
                       sec, s.running ? "run " : "down", s.playing ? "play" : "prime", s.latencyMs, s.bufferMs, s.targetBufferMs, s.syncDelayMs,
                       s.rateAdjustPpm, (unsigned long long)s.underruns, (unsigned long long)s.overflows,
                       (unsigned long long)s.discontinuities, (unsigned long long)s.glitches, (unsigned long long)s.reopens, db);
                fflush(stdout);
                if (sec > 5 && s.playing) {
                    sumLat += s.latencyMs; sumBuf += s.bufferMs; n++;
                    minBuf = std::min(minBuf, s.bufferMs); maxBuf = std::max(maxBuf, s.bufferMs);
                }
            }
            if (n > 0)
                printf("SUMMARY (after 5 s, %d samples): %u Hz %u ch, capture period %.2f ms, render period %.2f ms, render stream latency %.2f ms, "
                       "rate adjust %s; mean latency %.1f ms, buffer mean %.1f (min %.1f max %.1f) target %.1f ms, sync %.1f ms, ppm %+.1f\n",
                       n, s.sampleRate, s.channels, s.capturePeriodMs, s.renderPeriodMs, s.renderLatencyMs, s.rateAdjust ? "yes" : "no",
                       sumLat / n, sumBuf / n, minBuf, maxBuf, s.targetBufferMs, s.syncDelayMs, s.rateAdjustPpm);
            const int64_t ts = Qpc::Now();
            am.Stop();
            printf("Stop() took %.1f ms\n", Qpc::ToMs(Qpc::Now() - ts));
        }
    }
    MFShutdown();
    CoUninitialize();
    return rc;
}
