#pragma once
// Hardware Fix #4.4b: a small shared pool of large, reusable UI scratch
// buffers for temporary text expansion (RAW-Morse compose/history
// expansion, etc.) that Hardware Fix #4.3/#4.4 previously gave each screen
// as its own permanent 1801-byte static .bss array. Four such arrays
// (Enigma compose, Text compose, Text history, Friend history) together
// overflowed the ESP32's static DRAM segment at link time.
//
// Only two small slot records (a pointer + a capacity) live in .bss here.
// The actual backing storage is allocated once, lazily, from the normal
// ESP32 internal heap the first time a screen actually needs it, and is
// then kept and reused indefinitely -- never freed or reallocated on a
// per-frame/per-tick basis, so there is no steady-state heap churn or
// fragmentation risk beyond the first handful of allocations that happen
// as each screen is visited for the first time after boot.
//
// Two slots exist (not one) because Text Message's screenChat() can need
// its own long RAW-Morse expansion for the CONFIRMED COMPOSE line at the
// same time its HISTORY renderer also needs its own long expansion,
// within the same render pass -- see BUFFER ASSIGNMENT in the Hardware
// Fix #4.4b task notes. Enigma and Friend Number Guessing each only ever
// need one large scratch buffer live at a time, so they reuse these same
// two slots; their screens are never rendering concurrently with Text
// Message's (or with each other's), so sharing is safe.
#include <stddef.h>
#include <stdint.h>

namespace UiScratch {

enum class Slot : uint8_t { A, B };

// Returns a buffer of at least `minimumBytes` for `slot`. Allocates it
// from the heap on the first call for that slot; a later call that asks
// for a LARGER size than the slot currently holds grows it (via
// realloc-in-place-or-move), otherwise the existing buffer is reused
// as-is -- so repeated calls with the same (steady-state) size never
// touch the heap again after the first one. Returns nullptr if the
// requested size is 0 or the allocation/growth fails; callers MUST check
// for nullptr before writing or reading through the returned pointer, and
// must leave whatever they were about to send/store untouched when it
// happens (Hardware Fix #4.4b).
char* ensure(Slot slot, size_t minimumBytes);

// The slot's current usable capacity in bytes (0 if ensure() was never
// called for it, or every call so far has failed).
size_t capacity(Slot slot);

}  // namespace UiScratch
