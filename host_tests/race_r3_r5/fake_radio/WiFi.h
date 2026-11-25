#pragma once
// HOST TEST FAKE (not production): the Arduino networking surface used by
// radio_transport.cpp (IPAddress, WiFiUDP). UDP never delivers anything
// here: the radio suite exercises the MQTT paths (claim, grant, session,
// MQTT fallback, broadcast); STUN/UDP direct are not modeled.
#include <stdint.h>
#include <stddef.h>
class IPAddress {
 public:
  IPAddress() : b_{0, 0, 0, 0} {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : b_{a, b, c, d} {}
  uint8_t operator[](int i) const { return b_[i]; }
  uint8_t& operator[](int i) { return b_[i]; }

 private:
  uint8_t b_[4];
};
class WiFiUDP {
 public:
  uint8_t begin(uint16_t) { return 1; }
  int beginPacket(const char*, uint16_t) { return 1; }
  int beginPacket(IPAddress, uint16_t) { return 1; }
  size_t write(const uint8_t*, size_t n) { return n; }
  int endPacket() { return 1; }
  int parsePacket() { return 0; }
  int read(uint8_t*, size_t) { return 0; }
};
