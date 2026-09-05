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

void serviceTick() {
  if (!g_captureActive) return;

  int32_t raw[64];
  size_t bytesRead = 0;
  // Zero timeout: never block the main loop waiting for DMA data.
  i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, 0);
  size_t samplesRead = bytesRead / sizeof(int32_t);

  for (size_t i = 0; i < samplesRead && g_captureFilled < kFrameSamples; i++) {
    g_captureFrame[g_captureFilled++] = static_cast<int16_t>(raw[i] >> 16);
  }

  if (g_captureFilled >= kFrameSamples) {
    if (g_captureFn != nullptr) g_captureFn(g_captureFrame, kFrameSamples);
    g_captureFilled = 0;
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
