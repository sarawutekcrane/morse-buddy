#pragma once
// HOST TEST STUBS (not production) for the Phase 2C-2 Friend harness.
// Unlike earlier bundles, Menu is NOT stubbed: the real src/core/menu.cpp
// is compiled into the test translation unit.
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include "core/input.h"
#include "core/storage_messages.h"
struct Draw { uint32_t t; int x, y; std::string text; uint16_t color = 0xFFFF; };  // color: printLineColored only
namespace T {
extern uint32_t now;
extern std::vector<Draw> screen;        // currently visible (clear/fillRect erase)
extern std::vector<Draw> drawLog;       // every draw call, timestamped with the mock clock
extern std::deque<InputEvent> inputQ;  // converted events (popEvent reads these)
extern std::deque<InputEvent> raw;     // captured edges; only Input::update() converts them
extern int updateCount;
// MQTT
extern bool connected;
extern std::deque<bool> publishScript;  // next publishBinary() results while connected (default true)
struct Pub { std::string topic; size_t len; bool result; bool guardRejected; std::string group; std::vector<uint8_t> data; bool retained; };
extern std::vector<Pub> pubs;
extern std::function<void()> duringPublish;  // runs INSIDE a connected publishBinary() call
extern std::function<void(const Pub&)> onPublished;  // after a successful publishBinary() (e.g. a broker model)
// storage
struct Rec { MessageRef ref; MessageStore::StoredHeader hdr; PacketCodec::MessageEnvelope env;
             std::vector<uint8_t> wire, typePayload, local; };
extern std::vector<Rec> store;
extern bool appendResult, updateResult;
extern int appendCalls, updateCalls;
extern std::function<void()> duringUpdate;  // runs INSIDE updateTypeLocalPayload()
extern std::vector<std::string> seq;        // ordered: draws (D:text), nextId, publish, append, update
extern int notifications;
extern std::vector<std::string> indexLoads;  // loadConversationIndex() calls, "group/contact"
extern uint32_t unixTime;  // WifiManager::getUnixTime()
// Display fake measurement: false (default) = per-glyph advance widths, no
// clipping; true = the real FreeSans9pt7b glyph bearings and widths
// (font_metrics.h) with Adafruit getTextBounds() semantics, and printLine()
// clipping exactly as display.cpp does. Reset by reset().
extern bool realGlyphs;
// REAL_NOTIFICATIONS builds only: MessageStore::findMessagesByPredicate()
// scans the fake store; true makes it report the scan as unreliable.
extern bool scanUnreliable;
// MessageStore conversation walk and listMessageSequences(): true makes them
// fail as a directory-open failure would.
extern bool listUnreliable;
// listMessageSequences() calls since reset() (REAL_NOTIFICATIONS fake store).
extern unsigned listings;
extern std::function<void(const std::string&)> onDraw;  // runs after each printLine (models time passing during a draw)
void reset();
std::string visibleJoined();
std::string rowAt(int y);
uint16_t colorOf(const std::string& text, int y);  // color of a visible text at row y (0 if absent)  // visible texts on one row, left to right, joined by single spaces
bool visible(const std::string& s);
}
