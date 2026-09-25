#include "core/ui_scratch.h"

#include <stdlib.h>

namespace {

struct SlotState {
  char* buf = nullptr;
  size_t cap = 0;
};

// Exactly two records, matching UiScratch::Slot -- see ui_scratch.h for
// why two (Text Message compose + history scratch can be needed at once).
SlotState g_slots[2];

SlotState& stateFor(UiScratch::Slot slot) { return g_slots[static_cast<uint8_t>(slot)]; }

}  // namespace

namespace UiScratch {

char* ensure(Slot slot, size_t minimumBytes) {
  if (minimumBytes == 0) return nullptr;
  SlotState& s = stateFor(slot);
  if (s.buf != nullptr && s.cap >= minimumBytes) return s.buf;  // already big enough -- reuse, no heap traffic

  // realloc(nullptr, n) behaves as malloc(n), so this covers both the
  // first-ever allocation for this slot and a later grow uniformly. On
  // failure the original block (if any) is left untouched and valid at
  // its old (too-small) capacity; since every call site asks for the same
  // fixed worst-case size every time, this grow path is not expected to
  // be exercised in steady state at all -- it only matters the very first
  // time each screen is opened after boot.
  char* grown = static_cast<char*>(realloc(s.buf, minimumBytes));
  if (grown == nullptr) return nullptr;
  s.buf = grown;
  s.cap = minimumBytes;
  return s.buf;
}

size_t capacity(Slot slot) { return stateFor(slot).cap; }

}  // namespace UiScratch
