#include "core/radio_audio.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "core/hooks.h"
#include "core/pins.h"
#include "core/settings.h"

namespace RadioAudio {

namespace {

// ---- Capture: INMP441 on I2S port 0 ----------------------------------------
// INMP441 outputs 24-bit data MSB-justified in a 32-bit I2S slot; the
// standard technique is to configure the driver for 32-bit samples and take
// the top 16 bits of each word as the PCM value.
bool g_micInstalled = false;
bool g_captureActive = false;
CaptureFrameFn g_captureFn = nullptr;
int16_t g_captureFrame[kFrameSamples];
size_t g_captureFilled = 0;

// ---- Diagnostics (Fix Phase 1E: instrumentation only, no behavior change) --
// Bounded, RAM-only aggregate -- no dynamic allocation, no audio content.
// Measures the two DISJOINT phases already present in serviceTick() (the
// I2S read/accumulate step, and -- only on the roughly-1-in-5 ticks where a
// full frame completes -- the capture-frame callback) plus the tick's own
// total duration, separately. The callback is where a caller (radio_transport.cpp/
// race.cpp) may synchronously call into MqttManager::publishBinary(); that
// nested publish's own elapsed time is entirely CONTAINED WITHIN
// callbackElapsed here -- it is not measured a second time by this file, and
// must never be added to mqtt_manager.cpp's own qos0 publish diagnostics
// (see publishBinary()'s comment there): doing so would double-count the
// same span, since callbackElapsedMs already includes it.
//
// A window's summary is emitted at most once per second, and only when the
// window actually contains something notable (a read, callback, or total
// tick reaching the existing >=20ms PERF threshold below, or an I2S read
// error) -- fast, healthy windows stay completely silent. This check runs
// unconditionally at the very top of serviceTick(), BEFORE the
// !g_captureActive early return, specifically so a short transmission's
// still-pending (already-notable, not-yet-1-second-old) window is not lost
// merely because capture stopped before that second elapsed -- serviceTick()
// itself keeps being called every loop() iteration regardless of capture
// state, so the pending window is still flushed once due.
struct AudioDiag {
  uint32_t windowStartMs = 0;
  uint32_t activeTicks = 0;
  uint32_t callbacks = 0;
  uint32_t readSumMs = 0;
  uint32_t readMaxMs = 0;
  uint32_t callbackSumMs = 0;
  uint32_t callbackMaxMs = 0;
  uint32_t tickSumMs = 0;
  uint32_t tickMaxMs = 0;
  uint32_t i2sReadErrors = 0;
  bool notable = false;
};
AudioDiag g_diag;

constexpr uint32_t kDiagWindowMs = 1000;
// Matches the existing serviceTick PERF line's own threshold below, so
// "notable" means exactly what would already have triggered that line --
// no new arbitrary threshold is introduced.
constexpr uint32_t kDiagNotableMs = 20;

void recordDiagSample(uint32_t readElapsedMs, bool hadCallback, uint32_t callbackElapsedMs, uint32_t tickElapsedMs,
                      esp_err_t readErr) {
  g_diag.activeTicks++;
  g_diag.readSumMs += readElapsedMs;
  if (readElapsedMs > g_diag.readMaxMs) g_diag.readMaxMs = readElapsedMs;
  if (hadCallback) {
    g_diag.callbacks++;
    g_diag.callbackSumMs += callbackElapsedMs;
    if (callbackElapsedMs > g_diag.callbackMaxMs) g_diag.callbackMaxMs = callbackElapsedMs;
  }
  g_diag.tickSumMs += tickElapsedMs;
  if (tickElapsedMs > g_diag.tickMaxMs) g_diag.tickMaxMs = tickElapsedMs;
  if (readErr != ESP_OK) g_diag.i2sReadErrors++;
  if (readElapsedMs >= kDiagNotableMs || callbackElapsedMs >= kDiagNotableMs || tickElapsedMs >= kDiagNotableMs ||
      readErr != ESP_OK) {
    g_diag.notable = true;
  }
}

// Rollover-safe: plain uint32_t subtraction of two millis() samples is
// correct across a wraparound regardless of which side wrapped, the same
// idiom already used throughout mqtt_manager.cpp (e.g.
// drainReceiveQueueCooperative()). Called unconditionally every tick --
// see this section's own comment above for why.
void flushDiagIfDue(uint32_t nowMs) {
  uint32_t windowElapsedMs = nowMs - g_diag.windowStartMs;
  if (windowElapsedMs < kDiagWindowMs) return;
  if (g_diag.notable) {
    Serial.printf(
        "[DIAG][RADIO_AUDIO] tMs=%lu windowMs=%lu ticks=%lu callbacks=%lu readSumMs=%lu readMaxMs=%lu "
        "cbSumMs=%lu cbMaxMs=%lu tickSumMs=%lu tickMaxMs=%lu i2sErrors=%lu\n",
        static_cast<unsigned long>(nowMs), static_cast<unsigned long>(windowElapsedMs),
        static_cast<unsigned long>(g_diag.activeTicks), static_cast<unsigned long>(g_diag.callbacks),
        static_cast<unsigned long>(g_diag.readSumMs), static_cast<unsigned long>(g_diag.readMaxMs),
        static_cast<unsigned long>(g_diag.callbackSumMs), static_cast<unsigned long>(g_diag.callbackMaxMs),
        static_cast<unsigned long>(g_diag.tickSumMs), static_cast<unsigned long>(g_diag.tickMaxMs),
        static_cast<unsigned long>(g_diag.i2sReadErrors));
  }
  g_diag = AudioDiag{};
  g_diag.windowStartMs = nowMs;
}

void ensureMicInstalled() {
  if (g_micInstalled) return;

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.use_apll = false;

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = Pins::kInmp441Bclk;
  pinConfig.ws_io_num = Pins::kInmp441Ws;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = Pins::kInmp441Sd;

  i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  i2s_set_pin(I2S_NUM_0, &pinConfig);
  g_micInstalled = true;
}

// Hardware Diagnostic #4.9e Part B4: instrumentation only, no behavior
// change -- total tick duration, gated to >=20ms. UNCHANGED by Fix Phase 1E.
void serviceTick() {
  // Fix Phase 1E: runs every tick, active or not -- see AudioDiag's own
  // comment above for why this must not be gated behind g_captureActive.
  flushDiagIfDue(static_cast<uint32_t>(millis()));

  if (!g_captureActive) return;
  uint32_t tickStart = static_cast<uint32_t>(millis());

  int32_t raw[64];
  size_t bytesRead = 0;
  uint32_t readStart = tickStart;
  // Zero timeout: never block the main loop waiting for DMA data.
  esp_err_t readErr = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, 0);
  size_t samplesRead = bytesRead / sizeof(int32_t);

  for (size_t i = 0; i < samplesRead && g_captureFilled < kFrameSamples; i++) {
    g_captureFrame[g_captureFilled++] = static_cast<int16_t>(raw[i] >> 16);
  }
  // Fix Phase 1E (corrected): readElapsed now covers i2s_read() AND the
  // sample-copy/accumulation loop above -- not the i2s_read() call alone --
  // while still ending strictly before the capture callback starts below,
  // so read/accumulation and callback timing remain fully disjoint (never
  // double-counted against each other).
  uint32_t readElapsed = static_cast<uint32_t>(millis()) - readStart;

  bool hadCallback = false;
  uint32_t callbackElapsed = 0;
  if (g_captureFilled >= kFrameSamples) {
    if (g_captureFn != nullptr) {
      uint32_t callbackStart = static_cast<uint32_t>(millis());
      g_captureFn(g_captureFrame, kFrameSamples);
      callbackElapsed = static_cast<uint32_t>(millis()) - callbackStart;
      hadCallback = true;  // "completed callbacks": only counted when g_captureFn was actually invoked
    }
    g_captureFilled = 0;
  }

  uint32_t tickElapsed = static_cast<uint32_t>(millis()) - tickStart;
  recordDiagSample(readElapsed, hadCallback, callbackElapsed, tickElapsed, readErr);
  if (tickElapsed >= 20) {
    Serial.printf("[PERF][RADIO_AUDIO] serviceTick %lu ms\n", static_cast<unsigned long>(tickElapsed));
  }
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = nullptr;
    svc.tick = serviceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace

void startCapture(CaptureFrameFn fn) {
  ensureMicInstalled();
  g_captureFn = fn;
  g_captureFilled = 0;
  g_captureActive = true;
}

void stopCapture() {
  g_captureActive = false;
  g_captureFn = nullptr;
}

void playFrame(const int16_t* samples, size_t sampleCount) {
  size_t n = (sampleCount < kFrameSamples) ? sampleCount : kFrameSamples;
  float volScale = Settings::getSpeakerVolume() / 100.0f;
  int16_t scaled[kFrameSamples];
  for (size_t i = 0; i < n; i++) {
    scaled[i] = static_cast<int16_t>(static_cast<float>(samples[i]) * volScale);
  }
  size_t bytesWritten = 0;
  i2s_write(I2S_NUM_1, scaled, n * sizeof(int16_t), &bytesWritten, 0);
}

}  // namespace RadioAudio
