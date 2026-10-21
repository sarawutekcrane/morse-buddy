// Emits the target sizes as constants (read back with nm), compiled with the ESP32 toolchain.
#include "core/race_protocol.h"
extern const unsigned kEngine = sizeof(RaceProtocol::Engine);
extern const unsigned kRoomState = sizeof(RaceProtocol::RoomState);
extern const unsigned kPlatform = sizeof(RaceProtocol::Platform);
