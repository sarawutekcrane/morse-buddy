#pragma once
// Feature Fix #4.8: central palette for the optional per-user personal
// identity color (Name + Personal Color), used to visually distinguish
// family members' sender names in the Unified Thread. Deliberately the
// ONLY place RGB565 identity-color values are defined -- every screen and
// protocol layer refers to colors solely by this palette's index, never by
// a scattered raw RGB565 constant.
//
// The palette intentionally excludes BLACK, WHITE, pure RED, pure YELLOW,
// and pure GREEN: those already carry fixed semantic UI meaning elsewhere
// (message body WHITE, selection cursor RED, Enigma lock
// red/yellow/green states) and must never be mistaken for a personal
// color choice.

#include <stdint.h>

namespace IdentityColor {

constexpr uint8_t kColorCount = 12;

// Sentinel for "no personal color configured/known" -- e.g. a never-
// configured local identity, or a remote peer whose color has never been
// observed (legacy presence payload, or simply not yet seen). Callers
// resolving a color for on-screen display fall back to the existing CYAN
// sender color when they see this value; it is never itself a valid
// index into the palette.
constexpr uint8_t kInvalidColor = 0xFF;

// True for any index in [0, kColorCount) -- the only valid persisted/
// wire-transmitted color index values. kInvalidColor and anything else
// out of range are not valid.
bool isValid(uint8_t index);

// RGB565 value for a valid palette index. Returns ST77XX_WHITE-adjacent
// safe fallback (actually pure white, 0xFFFF) for an invalid index rather
// than an arbitrary color, so a caller that forgets to check isValid()
// first never silently renders a wrong-but-plausible palette color.
uint16_t color565(uint8_t index);

// Human-readable name for a valid palette index (e.g. "Cyan"), for the
// color-picker UI preview line. Returns "?" for an invalid index.
const char* colorName(uint8_t index);

}  // namespace IdentityColor
