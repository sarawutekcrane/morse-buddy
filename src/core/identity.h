#pragma once
// Device identity + persistent application ID counter (Addendum sections
// 4.1 and 7.3).

#include <stddef.h>
#include <stdint.h>

namespace Identity {

// Reads the MAC and formats device_id; loads the persistent id_counter
// from mb_core and stores Race's sequence floor (see reserveCounter()) if it
// is not stored yet. Call once after Storage::init(). Immutable thereafter.
void init();

// 12 uppercase hex characters, no separators, e.g. "AABBCCDDEEFF".
const char* deviceId();

// Formats "<device_id>-<counter>" into outBuf (must be at least 24 bytes)
// and increments+saves the counter to mb_core before returning, so an
// interrupted write can never hand out the same ID twice. Never resets at
// boot. Used for every locally-created unique application ID: message_id,
// event_id, invite_id, round_id, Radio session_id, Game result_event_id.
void nextId(char* outBuf, size_t outBufSize);

// Reserves the next Race sequence number (room requests, room counters).
// Race numbers have their own persistent counter ("raceSeq" in mb_core), so
// nextId()'s counter wrapping can never make Race reuse a number. Earlier
// Race numbers came from idCounter: init() (or else the first nextId() or
// reserveCounter()) stores idCounter's current value as raceSeq before
// idCounter may change. Returns false -- and no number -- if that or the new
// value could not be saved, if a stored value cannot be read, or at
// 0xFFFFFFFF (it never wraps). A number returned is never handed out again.
bool reserveCounter(uint32_t* out);

}  // namespace Identity
