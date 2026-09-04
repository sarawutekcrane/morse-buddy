#pragma once
// Text Message mode (Addendum sections 6-16 assorted; Phase 2 sections
// 9-13). Entirely self-registering: a static registrar in text_message.cpp
// calls registerModeHandler(Modes::TEXT, ...), registerMessageType(...)
// for TEXT, and registerNetworkPacketHandler(PK_MESSAGE, ...) — no other
// file needs to change for Text to become fully functional.
