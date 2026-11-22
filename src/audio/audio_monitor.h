#pragma once
#include "../util/common.h"
#include <memory>
#include <string>

namespace cameravfx {

struct AudioOptions {
    std::wstring captureDevice;
    std::wstring renderDevice;
    std::wstring videoDeviceName;
    std::wstring videoSymbolicLink;
    float volume = 1.0f;
    double bufferMs = 0;
    double offsetMs = 0;
    bool autoSync = false;
    bool testTone = false;
};

struct AudioStats {
    bool streaming = false;
    bool playing = false;
    std::string captureName, renderName;
    double latencyMs = 0;
    double bufferMs = 0;
    double syncDelayMs = 0;
    uint64_t underruns = 0;
};

struct AudioImpl;

class AudioMonitor {
public:
    AudioMonitor();
    ~AudioMonitor();
    static void ListDevices();
    bool Start(const AudioOptions& o);
    void Stop();
    bool Running() const;
    void SetMuted(bool muted);
    bool Muted() const;
    void SetPaused(bool paused);
    void SetVolume(float volume);
    float Volume() const;
    void SetVideoLatencyMs(double ms);
    AudioStats Stats() const;
private:
    std::unique_ptr<AudioImpl> p_;
};

}
