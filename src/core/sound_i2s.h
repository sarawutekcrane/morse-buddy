#pragma once
// Real I2S speaker tone backend (Addendum section 14.1 pins; Phase 3
// section 1). Entirely self-registering: a static registrar calls
// registerSoundBackend(...) and registers its own AppService — Phase 1's
// sound_facade.cpp and every existing playTone()/playToneSequence() call
// site need no changes; Phase 2 notifications become audible automatically.
