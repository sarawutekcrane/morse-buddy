// HOST TEST (not production): link stubs needed only by src/core/race.cpp.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "core/radio_audio.h"
#include "core/radio_transport.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/notifications.h"
#include "core/settings.h"
int g_invitePending = 0;  // count of setRaceInvitePending(true)
namespace Notifications { void setRaceInvitePending(bool p) { if (p) g_invitePending++; } }
// Room voice (Gate 3): what race.cpp hands the radio transport, recorded.
int g_roomFramesPlayed = 0;
RadioAudio::CaptureFrameFn g_roomCapture = nullptr;
RadioTransport::AudioScopeHandlerFn g_roomAudioHandler = nullptr;  // race.cpp's AUDIO_RACE_ROOM handler
struct RoomAudioPub { std::string group, topic; int scope; std::string key; uint16_t seq; size_t samples; };
std::vector<RoomAudioPub> g_roomAudioPubs;
bool g_roomAudioAccept = true;  // false: the transport skips the frame (congested socket / lent connection)
namespace RadioAudio {
void playFrame(const int16_t*, size_t) { g_roomFramesPlayed++; }
void startCapture(CaptureFrameFn fn) { g_roomCapture = fn; }
void stopCapture() { g_roomCapture = nullptr; }
}
namespace RadioTransport {
bool registerAudioScopeHandler(AudioScope scope, AudioScopeHandlerFn fn) {
  if (scope == AUDIO_RACE_ROOM) g_roomAudioHandler = fn;
  return true;
}
bool publishAudioPacket(const char* group, const char* topic, AudioScope scope, const char* key, uint16_t seq,
                        const int16_t*, size_t count) {
  g_roomAudioPubs.push_back({group, topic, static_cast<int>(scope), key != nullptr ? key : "", seq, count});
  return g_roomAudioAccept;
}
void setUiContext(UiContext) {}
}
namespace Settings { bool getMuteRadioOutsideRadio() { return false; } }
namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
bool g_roomRadioAudioActive = false;  // sound_facade: the live-voice flag (it holds deep sleep off)
void setRadioAudioActive(bool a) { g_roomRadioAudioActive = a; }
