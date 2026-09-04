#pragma once
// Mixed Text Entry (Addendum section 18, Phase 1 section 11).
//
// Three input states drive one confirmed-character buffer:
//   Empty              - encoder rotation begins Encoder-preview; a normal
//                         DOT/DASH press begins Morse accumulation;
//                         DOT/DASH >=2000ms deletes the previous confirmed
//                         character; Encoder short finishes the field.
//   Encoder-preview     - rotation changes the previewed character (wraps
//                         forward, exits to Empty on rotating back past the
//                         first item); any DOT/DASH duration confirms it.
//   Morse accumulation  - DOT/DASH presses append dot/dash symbols using
//                         current-WPM timing; the pattern is decoded and
//                         appended to the buffer after one letter-gap of no
//                         press (standard Morse letter-gap finalization);
//                         eight consecutive dots deletes the previous
//                         character immediately, before normal decoding.
// A trailing Confirmation step (Save/Cancel, default Save) finishes the
// session; Encoder long returns to editing instead.

#include <stdint.h>

enum class FieldCharset : uint8_t { GENERAL_NAME, GROUP_CODE, WIFI_PASSWORD };

enum class MixedTextEntryResult : uint8_t { NONE, SAVED, CANCELLED };

struct MixedTextEntryConfig {
  const char* title;
  FieldCharset charset;
  uint8_t maxLength;
  uint8_t minLength;                          // 0 allowed (e.g. WiFi Password)
  bool (*validator)(const char* candidate);   // optional, e.g. Group Code uniqueness; nullptr = none
};

namespace MixedTextEntry {

// Begins a new editing session. initialValue may be "" for a new field.
void start(const MixedTextEntryConfig& config, const char* initialValue);

// Non-blocking per-frame update; call every loop() iteration while this is
// the active screen (register/tick it like any other ScreenHandlerFn).
void tick();

// True once Save or Cancel has completed the session.
bool isFinished();
MixedTextEntryResult result();
const char* getValue();

}  // namespace MixedTextEntry
