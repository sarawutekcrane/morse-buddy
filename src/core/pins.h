#pragma once
// Central GPIO map (Phase 0 Overview). Keep every pin assignment here so no
// other module hardcodes a pin number.

namespace Pins {

constexpr int kTftMosi = 23;
constexpr int kTftSclk = 18;
constexpr int kTftRst = 4;
constexpr int kTftDc = 2;
constexpr int kTftCs = 15;
constexpr int kTftBacklight = 32;

constexpr int kDotDash = 21;

constexpr int kEncoderClk = 16;
constexpr int kEncoderDt = 33;
constexpr int kEncoderSw = 25;

constexpr int kInmp441Bclk = 14;
constexpr int kInmp441Ws = 27;
constexpr int kInmp441Sd = 13;

constexpr int kMax98357Bclk = 19;
constexpr int kMax98357Ws = 22;
constexpr int kMax98357Din = 5;

constexpr int kBatteryAdc = 34;
constexpr int kBatterySenseControl = 26;

}  // namespace Pins
