#pragma once
// HOST TEST FAKE (not production): 256dpi/MQTT 2.5.2's MQTTClient surface,
// driven by FakeNet's scripted broker (fake_net.h). Errors close the client,
// as the real library does. Every call checks that no other thread is inside
// the same client at the same time.
#include "fake_net.h"

typedef enum {
  LWMQTT_SUCCESS = 0,
  LWMQTT_NETWORK_FAILED_CONNECT = -3,
  LWMQTT_NETWORK_TIMEOUT = -4,
  LWMQTT_NETWORK_FAILED_READ = -5,
  LWMQTT_NETWORK_FAILED_WRITE = -6,
  LWMQTT_MISSING_OR_WRONG_PACKET = -9,
  LWMQTT_CONNECTION_DENIED = -10,
  LWMQTT_PONG_TIMEOUT = -13
} lwmqtt_err_t;
typedef enum { LWMQTT_CONNECTION_ACCEPTED = 0, LWMQTT_UNKNOWN_RETURN_CODE = 6 } lwmqtt_return_code_t;

uint32_t millis();  // the test's clock
class MQTTClient;
typedef void (*MQTTClientCallbackAdvanced)(MQTTClient* client, char topic[], char bytes[], int length);

class MQTTClient {
 public:
  MQTTClient(int, int) {}
  ~MQTTClient() {
    if (connected_) closeSession("destroy");
  }
  void begin(Client& c) { net_ = &c; }
  template <class C>
  void begin(const char*, int, C& c) {  // pre-fix API (syntax check of the baseline)
    net_ = &c;
  }
  void onMessageAdvanced(MQTTClientCallbackAdvanced cb) { cb_ = cb; }
  void setKeepAlive(int k) { keepAlive_ = k; }
  void setCleanSession(bool c) { cleanSession_ = c; }
  void setTimeout(int t) { timeout_ = t; }
  void setOptions(int k, bool c, int t) {
    keepAlive_ = k;
    cleanSession_ = c;
    timeout_ = t;
  }
  void setWill(const char* topic, const char* payload, bool retained, int qos) {
    willTopic_ = topic;
    willPayload_ = payload;
    willRetained_ = retained;
    willQos_ = qos;
  }
  int timeout() const { return timeout_; }

  bool connect(const char* id, bool skip = false) { return connect(id, nullptr, nullptr, skip); }
  bool connect(const char* id, const char*, const char*, bool skip = false) {
    Use u(this);
    lastError_ = LWMQTT_SUCCESS;
    returnCode_ = LWMQTT_UNKNOWN_RETURN_CODE;
    WiFiClient* w = static_cast<WiFiClient*>(net_);
    if (w == nullptr) return false;
    if (!skip) {
      if (w->connect(IPAddress(0), 1883) <= 0) {
        lastError_ = LWMQTT_NETWORK_FAILED_CONNECT;
        return false;
      }
    }
    if (!w->connected()) {
      lastError_ = LWMQTT_NETWORK_FAILED_CONNECT;
      return false;
    }
    FakeNet::maybeGate(cleanSession_ ? "cleanup-connect" : "connack");
    FakeNet::Script s = FakeNet::scriptFor(w->ip());
    FakeNet::Event e;
    e.kind = "connect";
    e.clientId = id;
    e.cleanSession = cleanSession_;
    e.willTopic = willTopic_;
    e.willPayload = willPayload_;
    e.willRetained = willRetained_;
    e.willQos = willQos_;
    e.keepAlive = keepAlive_;
    e.ip = w->ip();
    e.thread = std::this_thread::get_id();
    if (s.connackError != 0) {
      lastError_ = static_cast<lwmqtt_err_t>(s.connackError);
      if (s.connackError == LWMQTT_CONNECTION_DENIED)
        returnCode_ = static_cast<lwmqtt_return_code_t>(s.connackReturnCode);
      e.kind = "connect-failed";
      FakeNet::record(e);
      w->stop();
      return false;
    }
    {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      int& live = FakeNet::st().liveSessions[id];
      if (live > 0) FakeNet::st().sameIdOverlap++;
      live++;
    }
    clientId_ = id;
    returnCode_ = LWMQTT_CONNECTION_ACCEPTED;
    connected_ = true;
    subscribeIndex_ = 0;
    script_ = s;
    FakeNet::record(e);
    return true;
  }
  bool subscribe(const char* topic, int qos) {
    Use u(this);
    if (!connected()) {
      noteUseAfterClose();
      return false;
    }
    FakeNet::maybeGate("subscribe");
    FakeNet::Event e = base("subscribe");
    e.topic = topic;
    e.qos = qos;
    FakeNet::record(e);
    if (script_.failSubscribeAt == subscribeIndex_++) {
      lastError_ = LWMQTT_MISSING_OR_WRONG_PACKET;
      closeSession("close");
      return false;
    }
    // Retained messages the broker sends right after this SUBSCRIBE arrive
    // while the client waits for the SUBACK: delivered from inside this call.
    std::vector<FakeNet::Msg> msgs;
    {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      std::string t(topic);
      for (auto& kv : FakeNet::st().deliverOnSubscribe) {
        if (t.size() >= kv.first.size() && t.compare(t.size() - kv.first.size(), kv.first.size(), kv.first) == 0)
          msgs = kv.second;
      }
    }
    for (auto& m : msgs) deliver(m);
    return true;
  }
  bool publish(const char* topic, const char* payload, bool retained, int qos) {
    return publish(topic, payload, static_cast<int>(strlen(payload)), retained, qos);
  }
  bool publish(const char* topic, const char* payload, int length, bool retained, int qos) {
    Use u(this);
    if (!connected()) {
      noteUseAfterClose();
      return false;
    }
    bool online = std::string(topic).find("/presence/") != std::string::npos && length > 0 &&
                  std::string(payload, static_cast<size_t>(length)).find("|ONLINE|") != std::string::npos;
    FakeNet::maybeGate(online ? "online" : "publish");
    // A client backed by a real socket (test hook attachFd()) writes the
    // packet as lwmqtt 2.5.2 does: the fixed header + topic first, then the
    // payload, each through lwmqtt_write_to_network() -- a loop over
    // Client::write() bounded by the command timer (setTimeout()), where a
    // write of 0 bytes is LWMQTT_NETWORK_FAILED_WRITE; any error closes the
    // client (MQTTClient::publish()).
    if (static_cast<WiFiClient*>(net_)->fd() >= 0) {
      std::string topicStr(topic);
      std::vector<uint8_t> hdr;
      hdr.push_back(static_cast<uint8_t>(0x30 | (qos << 1) | (retained ? 1 : 0)));
      size_t rem = 2 + topicStr.size() + (qos > 0 ? 2 : 0) + static_cast<size_t>(length);
      do {
        uint8_t b = static_cast<uint8_t>(rem % 128);
        rem /= 128;
        if (rem > 0) b |= 0x80;
        hdr.push_back(b);
      } while (rem > 0);
      hdr.push_back(static_cast<uint8_t>(topicStr.size() >> 8));
      hdr.push_back(static_cast<uint8_t>(topicStr.size() & 0xff));
      hdr.insert(hdr.end(), topicStr.begin(), topicStr.end());
      if (qos > 0) {
        hdr.push_back(0);
        hdr.push_back(1);
      }
      uint32_t timerStart = millis();
      lwmqtt_err_t err = writeToNetwork(hdr.data(), hdr.size(), timerStart);
      if (err == LWMQTT_SUCCESS && length > 0)
        err = writeToNetwork(reinterpret_cast<const uint8_t*>(payload), static_cast<size_t>(length), timerStart);
      if (err != LWMQTT_SUCCESS) {
        lastError_ = err;
        closeSession("close");
        return false;
      }
    }
    FakeNet::Event e = base("publish");
    e.topic = topic;
    e.payload.assign(payload, static_cast<size_t>(length));
    e.retained = retained;
    e.qos = qos;
    FakeNet::record(e);
    bool fail = online && script_.failPublishOnline && !setupDone_;
    {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      if (setupDone_ && FakeNet::st().livePublishFailures > 0) {
        FakeNet::st().livePublishFailures--;
        fail = true;
      }
    }
    if (online && !fail) setupDone_ = true;
    if (fail) {
      lastError_ = LWMQTT_MISSING_OR_WRONG_PACKET;
      closeSession("close");
      return false;
    }
    return true;
  }
  bool loop() {
    Use u(this);
    if (!connected()) return false;
    std::vector<FakeNet::Msg> msgs;
    bool drop = false;
    {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      msgs.swap(FakeNet::st().inbox[clientId_]);
      drop = FakeNet::st().dropNext.erase(clientId_) > 0;
    }
    if (drop) {
      lastError_ = LWMQTT_PONG_TIMEOUT;
      closeSession("close");
      return false;
    }
    for (auto& m : msgs) deliver(m);
    return true;
  }
  bool connected() { return connected_ && net_ != nullptr && net_->connected(); }
  bool disconnect() {
    Use u(this);
    if (!connected()) return false;
    closeSession("disconnect");
    return true;
  }
  lwmqtt_err_t lastError() { return lastError_; }
  lwmqtt_return_code_t returnCode() { return returnCode_; }
  const std::string& clientId() const { return clientId_; }

 private:
  struct Use {
    explicit Use(MQTTClient* c) : c_(c) {
      if (c_->inUse_.fetch_add(1) != 0) {
        std::lock_guard<std::mutex> l(FakeNet::st().m);
        FakeNet::st().concurrentUse++;
      }
    }
    ~Use() { c_->inUse_.fetch_sub(1); }
    MQTTClient* c_;
  };
  FakeNet::Event base(const char* kind) {
    FakeNet::Event e;
    e.kind = kind;
    e.clientId = clientId_;
    e.thread = std::this_thread::get_id();
    e.ip = net_ != nullptr ? static_cast<WiFiClient*>(net_)->ip() : 0;
    return e;
  }
  lwmqtt_err_t writeToNetwork(const uint8_t* buf, size_t len, uint32_t timerStart) {
    size_t written = 0;
    while (written < len) {
      if (static_cast<int32_t>(timeout_) - static_cast<int32_t>(millis() - timerStart) <= 0)
        return LWMQTT_NETWORK_TIMEOUT;
      size_t n = net_->write(buf + written, len - written);
      if (n == 0) return LWMQTT_NETWORK_FAILED_WRITE;
      written += n;
    }
    return LWMQTT_SUCCESS;
  }
  void noteUseAfterClose() {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().useAfterClose++;
  }
  void closeSession(const char* how) {
    if (connected_) {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      FakeNet::st().liveSessions[clientId_]--;
    }
    connected_ = false;
    FakeNet::record(base(how));
    if (net_ != nullptr) net_->stop();
  }
  void deliver(const FakeNet::Msg& m) {
    if (cb_ == nullptr) return;
    std::vector<char> t(m.topic.begin(), m.topic.end());
    t.push_back('\0');
    std::vector<char> p(m.payload.begin(), m.payload.end());
    p.push_back('\0');
    cb_(this, t.data(), p.data(), static_cast<int>(m.payload.size()));
  }

  Client* net_ = nullptr;
  MQTTClientCallbackAdvanced cb_ = nullptr;
  int keepAlive_ = 60;
  bool cleanSession_ = true;
  int timeout_ = 1000;
  std::string willTopic_, willPayload_;
  bool willRetained_ = false;
  int willQos_ = 0;
  bool connected_ = false;
  bool setupDone_ = false;
  int subscribeIndex_ = 0;
  FakeNet::Script script_;
  std::string clientId_;
  lwmqtt_err_t lastError_ = LWMQTT_SUCCESS;
  lwmqtt_return_code_t returnCode_ = LWMQTT_UNKNOWN_RETURN_CODE;
  std::atomic<int> inUse_{0};
};
