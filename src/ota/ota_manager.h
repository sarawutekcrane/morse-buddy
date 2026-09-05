#pragma once
// Firmware OTA (Addendum n/a; Phase 5). Entirely self-registering, same
// pattern as every prior phase's module: a static registrar hooks one
// AppService (post-update health check at boot + a once-per-cold-boot
// background availability check) and this header exposes exactly the one
// screen Settings > System needs to push. Everything else — the manifest
// fetch/parse, the streaming HTTPS download (ota_https.*), the anti-loop
// rollback bookkeeping, and OTA Maintenance Mode's calls into
// sleep.h/mqtt_manager.h/radio_transport.h — is internal to
// ota_manager.cpp.

namespace Ota {

// Settings > System > Firmware Update. ScreenHandlerFn; push via
// Menu::pushScreen.
void screenFirmwareUpdate();

}  // namespace Ota
