#pragma once
// HOST STUB (not production): minimal Arduino surface for host tests.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
uint32_t millis();
void delay(uint32_t ms);  // declared only (syntax checks)
long random(long maxExclusive);
uint32_t esp_random();  // ESP-IDF (declared via Arduino.h on target)
// Serial (host stub): formatted output is discarded.
struct HostSerial {
  int printf(const char*, ...) { return 0; }
  void println(const char* = "") {}
  void print(const char*) {}
};
extern HostSerial Serial;
