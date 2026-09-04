#include "core/sound_facade.h"

#include <Arduino.h>

namespace {
ToneBackendFn g_toneFn = nullptr;
ToneSequenceBackendFn g_sequenceFn = nullptr;
StopSoundBackendFn g_stopFn = nullptr;

bool g_radioActive = false;
uint8_t g_currentClass = 0;       // 0 = idle, else a SoundClass value
uint32_t g_currentEndsAtMs = 0;

bool isCurrentStillPlaying() {
  return g_currentClass != 0 && static_cast<int32_t>(millis() - g_currentEndsAtMs) < 0;
}

bool higherOrEqualPriority(SoundClass requested, uint8_t playing) {
  // Only two classes exist; NOTIFICATION (2) outranks GAME (1).
  return static_cast<uint8_t>(requested) >= playing;
}
}  // namespace

void registerSoundBackend(ToneBackendFn toneFn, ToneSequenceBackendFn sequenceFn, StopSoundBackendFn stopFn) {
  g_toneFn = toneFn;
  g_sequenceFn = sequenceFn;
  g_stopFn = stopFn;
}

void playTone(uint16_t frequencyHz, uint16_t durationMs, SoundClass soundClass) {
  if (g_radioActive) return;
  if (isCurrentStillPlaying() && !higherOrEqualPriority(soundClass, g_currentClass)) return;

  if (g_toneFn != nullptr) {
    g_toneFn(frequencyHz, durationMs, soundClass);
  } else {
    Serial.printf("[sound] tone %uHz %ums class=%u\n", frequencyHz, durationMs, soundClass);
  }
  g_currentClass = soundClass;
  g_currentEndsAtMs = millis() + durationMs;
}

void playToneSequence(uint16_t frequencyHz, const uint16_t* durationsMs, size_t count, SoundClass soundClass) {
  if (g_radioActive) return;
  if (isCurrentStillPlaying() && !higherOrEqualPriority(soundClass, g_currentClass)) return;

  uint32_t totalMs = 0;
  for (size_t i = 0; i < count; i++) totalMs += durationsMs[i];

  if (g_sequenceFn != nullptr) {
    g_sequenceFn(frequencyHz, durationsMs, count, soundClass);
  } else {
    Serial.printf("[sound] toneSequence %uHz count=%u class=%u\n", frequencyHz, (unsigned)count, soundClass);
  }
  g_currentClass = soundClass;
  g_currentEndsAtMs = millis() + totalMs;
}

void stopCurrentToneSound() {
  if (g_stopFn != nullptr) {
    g_stopFn();
  }
  g_currentClass = 0;
  g_currentEndsAtMs = 0;
}

void setRadioAudioActive(bool active) {
  g_radioActive = active;
  if (active) {
    stopCurrentToneSound();
  }
}
