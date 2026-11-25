#pragma once
// HOST TEST FAKE (not production): esp_deep_sleep_start() records the entry
// (radio_test.cpp T_deepSleepEntered()) and returns -- the real call never
// returns; the test checks when it would have happened.
void T_deepSleepEntered();
inline void esp_deep_sleep_start() { T_deepSleepEntered(); }
