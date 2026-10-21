#pragma once
// HOST STUB (not production): syntax-check surface only (256dpi/MQTT 2.5.2 names).
#include <stdint.h>
typedef enum { LWMQTT_SUCCESS = 0, LWMQTT_NETWORK_FAILED_CONNECT = -3 } lwmqtt_err_t;
typedef enum { LWMQTT_CONNECTION_ACCEPTED = 0 } lwmqtt_return_code_t;
class Client;
class MQTTClient;
typedef void (*MQTTClientCallbackAdvanced)(MQTTClient* client, char topic[], char bytes[], int length);
class MQTTClient {
 public:
  MQTTClient(int readBufSize, int writeBufSize) { (void)readBufSize; (void)writeBufSize; }
  template <class C> void begin(const char*, int, C&) {}
  void onMessageAdvanced(MQTTClientCallbackAdvanced) {}
  void setOptions(int, bool, int) {}
  void setTimeout(int) {}
  void setWill(const char*, const char*, bool, int) {}
  bool connect(const char*, bool = false) { return false; }
  bool connect(const char*, const char*, const char*, bool = false) { return false; }
  bool publish(const char*, const char*, int, bool, int) { return false; }
  bool publish(const char*, const char*, bool, int) { return false; }
  bool subscribe(const char*, int) { return false; }
  bool loop() { return false; }
  bool connected() { return false; }
  bool disconnect() { return false; }
  lwmqtt_err_t lastError() { return LWMQTT_SUCCESS; }
  lwmqtt_return_code_t returnCode() { return LWMQTT_CONNECTION_ACCEPTED; }
};
