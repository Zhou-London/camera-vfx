#include "audio_monitor.h"
// STUB: replaced by the audio work item.
namespace camss {
struct AudioImpl {};
AudioMonitor::AudioMonitor() = default;
AudioMonitor::~AudioMonitor() = default;
void AudioMonitor::ListDevices() {}
bool AudioMonitor::Start(const AudioOptions&) { return false; }
void AudioMonitor::Stop() {}
bool AudioMonitor::Running() const { return false; }
void AudioMonitor::SetMuted(bool) {}
bool AudioMonitor::Muted() const { return false; }
void AudioMonitor::SetVolume(float) {}
float AudioMonitor::Volume() const { return 1.0f; }
void AudioMonitor::SetVideoLatencyMs(double) {}
AudioStats AudioMonitor::Stats() const { return {}; }
} // namespace camss
