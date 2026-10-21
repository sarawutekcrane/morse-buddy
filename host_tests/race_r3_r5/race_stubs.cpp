// HOST TEST (not production): link stubs needed only by src/core/race.cpp.
#include <cstddef>
#include <cstdint>
#include "core/radio_audio.h"
#include "core/radio_transport.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/notifications.h"
#include "core/settings.h"
int g_invitePending = 0;  // count of setRaceInvitePending(true)
namespace Notifications { void setRaceInvitePending(bool p) { if (p) g_invitePending++; } }
namespace RadioAudio {
void playFrame(const int16_t*, size_t) {}
void startCapture(CaptureFrameFn) {}
void stopCapture() {}
}
namespace RadioTransport {
bool registerAudioScopeHandler(AudioScope, AudioScopeHandlerFn) { return true; }
bool publishAudioPacket(const char*, const char*, AudioScope, const char*, uint16_t, const int16_t*, size_t) { return true; }
void setUiContext(UiContext) {}
}
namespace Settings { bool getMuteRadioOutsideRadio() { return false; } }
namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
void setRadioAudioActive(bool) {}
