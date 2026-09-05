#pragma once
// Number Guessing Race Mode + Race Room voice (Addendum sections 12, 14;
// Phase 4 sections 11-18). Entirely self-registering: a static registrar
// calls registerGameSubModeHandler(RACE, ...), registers the PK_RACE_*
// packet handlers through Phase 2's Network Packet Handler Registry, and
// registers the RACE_ROOM audio scope through RadioTransport — no other
// file needs to change (settings.cpp's trampolineRace/kNumberGuessingItems
// already route into the sub-mode registry from Phase 3's Solo work).
