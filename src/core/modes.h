#pragma once
// Main Menu mode IDs dispatched through the Mode Handler registry
// (Addendum section 3.7). Training Game and Settings are Phase 1-native
// screens and do not go through this registry.

#include <stdint.h>

namespace Modes {
constexpr uint8_t TEXT = 1;
constexpr uint8_t ENIGMA = 2;
constexpr uint8_t RADIO = 3;
}  // namespace Modes

// Number Guessing sub-mode IDs dispatched through the Game Sub-mode
// registry (Addendum section 3.8).
namespace GameSubModes {
constexpr uint8_t SOLO = 1;
constexpr uint8_t FRIEND = 2;
constexpr uint8_t RACE = 3;
}  // namespace GameSubModes
