#pragma once
// HOST TEST FAKE (not production): see ../fake_net.h.
#include "../fake_net.h"
// Production code (mqtt_manager.cpp) reaches send() through this header:
// route it through the fault model (passes through unless a test arms it).
#define send(fd, buf, len, flags) FakeNet::faultySend(fd, buf, len, flags)
