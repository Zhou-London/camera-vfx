// HDMI audio monitoring: captures the capture card's audio endpoint (WASAPI shared mode,
// event driven) and plays it on a render endpoint with low latency. Runs on its own
// MMCSS thread; the video pipeline never waits on it.
//  * capture endpoint: explicit name substring, or auto = the audio endpoint that belongs to the
//    same physical device (ContainerId) as the video capture device, falling back to a name match
//  * clock drift between capture and render devices is absorbed by a slow servo on the buffer
//    fill (IAudioClockAdjustment rate trim, or sample drop/insert as a fallback)
//  * optional auto-sync: extra delay so that audio latency matches the video path latency
#pragma once
#include "../util/common.h"
#include <memory>
#include <string>

namespace camss {

struct AudioOptions {
    bool enabled = true;
    std::wstring captureDevice;      // substring of the capture endpoint name; empty = auto
    std::wstring videoDeviceName;    // friendly name of the video capture device (auto matching)
    std::wstring videoSymbolicLink;  // MF symbolic link of the video capture device (ContainerId matching), may be empty
    std::wstring renderDevice;       // substring of the render endpoint name; empty = default console render device
    float volume = 1.0f;             // linear gain, 0..2
    double bufferMs = 10.0;          // jitter buffer target on top of the device periods
    double delayMs = 0.0;            // extra fixed delay
    bool autoSync = true;            // add delay so audio latency matches SetVideoLatencyMs()
    bool testTone = false;           // replace the captured samples with a -36 dBFS 440 Hz sine (render path test)
    bool forceDropInsert = false;    // testing: do not use IAudioClockAdjustment, absorb drift by frame drop/insert
};

struct AudioStats {
    bool running = false;
    std::string captureName, renderName;
    uint32_t sampleRate = 0, channels = 0;
    double latencyMs = 0;            // estimated capture timestamp -> played at the render endpoint
    double bufferMs = 0;             // current buffered audio (ring + render padding)
    double targetBufferMs = 0;       // buffer the servo aims for (incl. sync delay)
    double syncDelayMs = 0;          // delay added by auto-sync
    double rateAdjustPpm = 0;        // current resampling trim
    uint64_t underruns = 0, overflows = 0, discontinuities = 0;
    float peak = 0;                  // recent peak level (0..1), for a simple meter
    // diagnostics
    std::string matchMethod;         // how the capture endpoint was chosen ("ContainerId", "name", ...)
    bool playing = false;            // false while (re)priming the buffer or reopening devices
    bool rateAdjust = false;         // IAudioClockAdjustment in use (else sample drop/insert)
    double capturePeriodMs = 0, renderPeriodMs = 0, renderLatencyMs = 0;
    uint64_t glitches = 0;           // render buffer found empty while playing (engine starved)
    uint64_t reopens = 0;            // device error recoveries
};

struct AudioImpl;

class AudioMonitor {
public:
    AudioMonitor();
    ~AudioMonitor();
    static void ListDevices();                 // logs capture and render endpoints
    bool Start(const AudioOptions& o);         // false (with a warning) if no usable device
    void Stop();
    bool Running() const;
    void SetMuted(bool m);
    bool Muted() const;
    void SetVolume(float v);                   // linear gain 0..2
    float Volume() const;
    void SetVideoLatencyMs(double ms);         // video sample -> display latency (p50), for auto-sync
    AudioStats Stats() const;
private:
    std::unique_ptr<AudioImpl> p_;
};

} // namespace camss
