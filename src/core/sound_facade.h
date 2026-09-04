#pragma once
// Sound facade (Addendum section 3.6). Phase 1/2 have no backend registered,
// so calls simply log to Serial. Phase 3 registers a non-blocking I2S tone
// backend; no source change is needed here or at any call site when that
// happens.

#include <stddef.h>
#include <stdint.h>

enum SoundClass : uint8_t {
  SOUND_GAME = 1,
  SOUND_NOTIFICATION = 2
};

using ToneBackendFn = void (*)(uint16_t frequencyHz, uint16_t durationMs, SoundClass soundClass);
using ToneSequenceBackendFn = void (*)(uint16_t frequencyHz,
                                       const uint16_t* durationsMs,
                                       size_t count,
                                       SoundClass soundClass);
using StopSoundBackendFn = void (*)();

void registerSoundBackend(ToneBackendFn toneFn, ToneSequenceBackendFn sequenceFn, StopSoundBackendFn stopFn);

// Priority: Radio > SOUND_NOTIFICATION > SOUND_GAME. While Radio audio is
// active (see setRadioAudioActive), both classes are skipped. While a
// higher-priority class is still playing, a lower-priority request is
// dropped; a higher-priority request always preempts a lower one.
void playTone(uint16_t frequencyHz, uint16_t durationMs, SoundClass soundClass);

// durationsMs alternates TONE, GAP, TONE, GAP, ... starting with a TONE.
void playToneSequence(uint16_t frequencyHz, const uint16_t* durationsMs, size_t count, SoundClass soundClass);

void stopCurrentToneSound();

// Phase 4 calls setRadioAudioActive(true) for the duration of a live call;
// this immediately stops/blocks lower-priority tone playback.
void setRadioAudioActive(bool active);
