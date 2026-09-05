#pragma once
// Radio mode UI (Addendum section 14; Phase 4 sections 1-2, 9-10). Entirely
// self-registering: a static registrar calls registerModeHandler(RADIO, ...)
// and registerSettingsChangeHook (Mute Radio baseline availability) — no
// other file needs to change. Audio hardware lives in radio_audio.cpp;
// claim/transport/broadcast protocol lives in radio_transport.cpp; this
// file is the screens (Recipient, Talk/PTT) that drive them.
