#include "core/sound_i2s.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#include "core/hooks.h"
#include "core/pins.h"
#include "core/settings.h"
#include "core/sound_facade.h"

namespace {

constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kChunkSamples = 160;  // 10ms @ 16kHz
constexpr size_t kMaxSeqLen = 128;
constexpr int16_t kAmplitude = 3000;  // headroom below int16 full scale
constexpr double kTwoPi = 6.283185307179586;

bool g_active = false;
uint16_t g_frequencyHz = 0;

bool g_sequenceMode = false;
uint16_t g_seqDurations[kMaxSeqLen];
size_t g_seqCount = 0;
size_t g_seqIndex = 0;
bool g_currentSegmentIsGap = false;

uint32_t g_segmentTotalSamples = 0;
uint32_t g_segmentSamplesDone = 0;
double g_phase = 0.0;

void startSegment(uint16_t durationMs, bool isGap) {
  g_currentSegmentIsGap = isGap;
  g_segmentTotalSamples = static_cast<uint32_t>(durationMs) * kSampleRate / 1000;
  g_segmentSamplesDone = 0;
}

// Sequence durations alternate TONE, GAP, TONE, GAP, ... starting with a TONE.
void advanceToNextSegment() {
  if (!g_sequenceMode) {
    g_active = false;
    return;
  }
  g_seqIndex++;
  if (g_seqIndex >= g_seqCount) {
    g_active = false;
    return;
  }
  startSegment(g_seqDurations[g_seqIndex], (g_seqIndex % 2) == 1);
}

void toneBackend(uint16_t frequencyHz, uint16_t durationMs, SoundClass soundClass) {
  (void)soundClass;  // priority/arbitration already handled by sound_facade.cpp
  g_sequenceMode = false;
  g_frequencyHz = frequencyHz;
  g_phase = 0.0;
  startSegment(durationMs, false);
  g_active = true;
}

void toneSequenceBackend(uint16_t frequencyHz, const uint16_t* durationsMs, size_t count, SoundClass soundClass) {
  (void)soundClass;
  if (count == 0) return;
  g_frequencyHz = frequencyHz;
  g_sequenceMode = true;
  g_seqCount = (count < kMaxSeqLen) ? count : kMaxSeqLen;
  for (size_t i = 0; i < g_seqCount; i++) g_seqDurations[i] = durationsMs[i];
  g_seqIndex = 0;
  g_phase = 0.0;
  startSegment(g_seqDurations[0], false);
  g_active = true;
}

void stopBackend() { g_active = false; }

void serviceInit() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  // Mono output for a single MAX98357A. If the assembled board's SD-pin
  // L/R strapping needs the other channel, verify on hardware and swap to
  // I2S_CHANNEL_FMT_ONLY_RIGHT — a wiring detail, not an algorithm choice.
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.use_apll = false;

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = Pins::kMax98357Bclk;
  pinConfig.ws_io_num = Pins::kMax98357Ws;
  pinConfig.data_out_num = Pins::kMax98357Din;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_1, &config, 0, nullptr);
  i2s_set_pin(I2S_NUM_1, &pinConfig);

  registerSoundBackend(toneBackend, toneSequenceBackend, stopBackend);
}

void serviceTick() {
  if (!g_active) return;

  if (g_segmentSamplesDone >= g_segmentTotalSamples) {
    advanceToNextSegment();
    if (!g_active) return;
  }

  uint32_t remaining = g_segmentTotalSamples - g_segmentSamplesDone;
  uint32_t chunk = (remaining < kChunkSamples) ? remaining : kChunkSamples;
  if (chunk == 0) return;

  float volScale = Settings::getSpeakerVolume() / 100.0f;
  int16_t buf[kChunkSamples];
  for (uint32_t i = 0; i < chunk; i++) {
    if (g_currentSegmentIsGap || g_frequencyHz == 0) {
      buf[i] = 0;
    } else {
      buf[i] = static_cast<int16_t>(sin(g_phase) * kAmplitude * volScale);
      g_phase += kTwoPi * g_frequencyHz / kSampleRate;
      if (g_phase > kTwoPi) g_phase -= kTwoPi;
    }
  }

  size_t bytesWritten = 0;
  // Zero timeout: never block the main loop waiting for DMA buffer space.
  i2s_write(I2S_NUM_1, buf, chunk * sizeof(int16_t), &bytesWritten, 0);
  g_segmentSamplesDone += static_cast<uint32_t>(bytesWritten / sizeof(int16_t));
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = serviceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace
