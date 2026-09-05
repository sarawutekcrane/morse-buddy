#pragma once
// Radio audio hardware (Addendum section 14; Phase 4 sections 3, 9):
// INMP441 microphone capture on I2S port 0, continuous speaker playback on
// I2S port 1 (reusing the driver Phase 3's sound_i2s.cpp already installed
// there). This module owns only the hardware I/O — session lifecycle,
// transport (UDP/MQTT), jitter buffering and mute/priority policy belong to
// radio_transport.cpp and race.cpp, which are the two callers.
//
// Fixed voice format everywhere in Phase 4: 16kHz, 16-bit signed mono PCM,
// 20ms frames (320 samples = 640 bytes).
//
// IMPORTANT: playFrame() writes straight to I2S_NUM_1. The caller MUST call
// setRadioAudioActive(true) (sound_facade.h) for the whole session first —
// that's what guarantees sound_i2s.cpp's tone generator isn't concurrently
// writing to the same port (Addendum: Radio > Notification > Game tone).

#include <stddef.h>
#include <stdint.h>

namespace RadioAudio {

constexpr uint32_t kSampleRate = 16000;
constexpr size_t kFrameSamples = 320;              // 20ms @ 16kHz
constexpr size_t kFrameBytes = kFrameSamples * 2;  // 640 bytes, 16-bit mono

// Called once per completed ~20ms frame while capture is active.
using CaptureFrameFn = void (*)(const int16_t* samples, size_t sampleCount);

// Only one capture consumer at a time — Radio Talk and Race Room voice are
// mutually exclusive screens, so this is never contended. Installs the
// INMP441 I2S driver on first use.
void startCapture(CaptureFrameFn fn);
void stopCapture();

// Plays one decoded frame immediately (non-blocking write, volume-scaled by
// Settings::getSpeakerVolume()). The caller owns any jitter buffering.
void playFrame(const int16_t* samples, size_t sampleCount);

}  // namespace RadioAudio
