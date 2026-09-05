#pragma once
// Number Guessing "Play with Friend" (Addendum section 12; Phase 3
// sections 10-14). Entirely self-registering: a static registrar in
// number_guessing_friend.cpp calls registerGameSubModeHandler(FRIEND, ...),
// registerMessageType(...) for MSG_TYPE_GAME, TextMessage::
// registerIncomingMessageHandler(MSG_TYPE_GAME, ...),
// registerNetworkPacketHandler(PK_GAME_RESULT_CHUNK, ...) and the single
// registerEmptyLineAction(...) slot — no other file needs to change for
// Play with Friend to become fully functional (settings.cpp's
// trampolineFriend/kNumberGuessingItems already route into the sub-mode
// registry from Phase 3's own earlier Solo work).
