#include "core/power.h"

#include <Arduino.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "core/pins.h"

namespace {

// R1/R2 = 100k/100k -> ADC node sees half of the sensed rail.
constexpr float kDividerRatio = 2.0f;  // (R1+R2)/R2

constexpr uint32_t kSampleIntervalMs = 6000;   // one sample every 6 seconds
constexpr uint8_t kSamplesPerCycle = 10;       // averaged once per 10-sample cycle
constexpr uint32_t kDividerSettleMs = 5;       // ADC-node settle time before reading

esp_adc_cal_characteristics_t g_adcChars;

uint16_t g_sampleBufferMv[kSamplesPerCycle];
uint8_t g_sampleIndex = 0;

float g_batteryVoltage = 0.0f;
uint8_t g_batteryPercent = 100;

enum class SampleState : uint8_t { IDLE, SETTLING };
SampleState g_state = SampleState::IDLE;
uint32_t g_lastSampleAtMs = 0;
uint32_t g_settleStartMs = 0;

struct LipoPoint {
  float voltage;
  uint8_t percent;
};

// Addendum section 20.1 — exact project LiPo lookup table.
const LipoPoint kLipoTable[] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75},
    {3.95f, 70},  {3.91f, 65}, {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45},
    {3.80f, 40},  {3.79f, 35}, {3.77f, 30}, {3.75f, 25}, {3.73f, 20}, {3.71f, 15},
    {3.69f, 10},  {3.61f, 5},  {3.50f, 2},  {3.30f, 0},
};
constexpr size_t kLipoTableSize = sizeof(kLipoTable) / sizeof(kLipoTable[0]);

uint8_t voltageToPercent(float v) {
  if (v >= kLipoTable[0].voltage) return kLipoTable[0].percent;
  if (v <= kLipoTable[kLipoTableSize - 1].voltage) return kLipoTable[kLipoTableSize - 1].percent;

  for (size_t i = 0; i + 1 < kLipoTableSize; i++) {
    float hi = kLipoTable[i].voltage;
    float lo = kLipoTable[i + 1].voltage;
    if (v <= hi && v > lo) {
      float ratio = (v - lo) / (hi - lo);
      float pct = kLipoTable[i + 1].percent + ratio * (kLipoTable[i].percent - kLipoTable[i + 1].percent);
      return static_cast<uint8_t>(pct + 0.5f);
    }
  }
  return 0;
}

uint32_t readAdcNodeMilliVolts() {
  int raw = adc1_get_raw(ADC1_CHANNEL_6);  // GPIO34
  return esp_adc_cal_raw_to_voltage(static_cast<uint32_t>(raw), &g_adcChars);
}

// Takes one full sample: divider on, settle, read, divider off.
// Blocking for kDividerSettleMs (5ms); only used at boot for the
// provisional reading, where a short setup()-time delay is acceptable.
uint32_t takeBlockingSampleMv() {
  digitalWrite(Pins::kBatterySenseControl, HIGH);
  delay(kDividerSettleMs);
  uint32_t mv = readAdcNodeMilliVolts();
  digitalWrite(Pins::kBatterySenseControl, LOW);
  return mv;
}

void recomputeFromBuffer(uint8_t count) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < count; i++) sum += g_sampleBufferMv[i];
  uint32_t avgMv = sum / count;
  g_batteryVoltage = (avgMv / 1000.0f) * kDividerRatio;
  g_batteryPercent = voltageToPercent(g_batteryVoltage);
}

}  // namespace

namespace Power {

void init() {
  pinMode(Pins::kBatterySenseControl, OUTPUT);
  digitalWrite(Pins::kBatterySenseControl, LOW);  // divider off by default

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &g_adcChars);

  // One immediate provisional reading at boot (Addendum section 20).
  uint32_t mv = takeBlockingSampleMv();
  for (uint8_t i = 0; i < kSamplesPerCycle; i++) g_sampleBufferMv[i] = static_cast<uint16_t>(mv);
  recomputeFromBuffer(kSamplesPerCycle);
  g_sampleIndex = 0;
  g_lastSampleAtMs = millis();
  g_state = SampleState::IDLE;
}

void tick() {
  uint32_t now = millis();

  switch (g_state) {
    case SampleState::IDLE:
      if (now - g_lastSampleAtMs >= kSampleIntervalMs) {
        digitalWrite(Pins::kBatterySenseControl, HIGH);
        g_settleStartMs = now;
        g_state = SampleState::SETTLING;
      }
      break;

    case SampleState::SETTLING:
      if (now - g_settleStartMs >= kDividerSettleMs) {
        uint32_t mv = readAdcNodeMilliVolts();
        digitalWrite(Pins::kBatterySenseControl, LOW);

        g_sampleBufferMv[g_sampleIndex] = static_cast<uint16_t>(mv);
        g_sampleIndex++;
        if (g_sampleIndex >= kSamplesPerCycle) {
          g_sampleIndex = 0;
          recomputeFromBuffer(kSamplesPerCycle);
        }

        g_lastSampleAtMs = now;
        g_state = SampleState::IDLE;
      }
      break;
  }
}

void setDividerEnabled(bool enabled) {
  digitalWrite(Pins::kBatterySenseControl, enabled ? HIGH : LOW);
  g_state = SampleState::IDLE;
}

uint8_t getBatteryPercent() { return g_batteryPercent; }
float getBatteryVoltage() { return g_batteryVoltage; }
bool isLowBattery() { return g_batteryPercent <= 20; }
bool isCriticalBattery() { return g_batteryPercent <= 10; }

}  // namespace Power
