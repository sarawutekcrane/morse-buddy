#pragma once
// HOST TEST FIXTURE (not production). The device under test is the ACTUAL
// src/core/race.cpp + menu.cpp (included by the test translation unit) with
// its ACTUAL RaceProtocol engine; peers are further ACTUAL RaceProtocol::Engine
// instances. A small broker model carries every publish (the device's go
// through MqttManager::publishBinary's stub, with their real PacketCodec
// header) to every subscriber of the same group: r2/+ to everyone, r2q/<id>
// to <id>; retained states are replayed to a peer that (re)connects.
// Delivery is asynchronous (pumped from tick()), like the real receive queue.
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/packet_codec.h"
#include "core/race_protocol.h"

namespace Fx {
struct Msg { std::string group, topic; uint8_t kind; std::vector<uint8_t> payload; bool retained; std::string from; };
struct Peer;
// Platform::nextSeq before the review (uint32_t(void*)) and after
// (bool(void*, uint32_t*)), so the same tests compile against both.
uint32_t peerSeq(void* c);
template <class F> struct SeqFn;
template <> struct SeqFn<uint32_t (*)(void*)> { static uint32_t fn(void* c) { return peerSeq(c); } };
template <> struct SeqFn<bool (*)(void*, uint32_t*)> {
  static bool fn(void* c, uint32_t* out) { *out = peerSeq(c); return true; }
};
std::deque<Msg> inflight;
std::map<std::string, Msg> retained;
std::vector<Peer*> peers;
bool deviceOnline = true;
std::function<bool(const Msg&)> dropIf;

struct Peer {
  std::string id;
  std::map<std::string, std::vector<uint8_t>> nvs;
  RaceProtocol::Engine eng;
  bool online = true;
  std::vector<uint32_t> script;  // random32 values to return first (e.g. a secret)
  uint32_t seqCounter = 0;
  explicit Peer(const char* id_) : id(id_) {}
  static bool pub(void* c, const char* g, const char* s, uint8_t k, const uint8_t* p, uint16_t l, bool r) {
    Peer* x = static_cast<Peer*>(c);
    if (!x->online) return false;
    inflight.push_back({g, s, k, std::vector<uint8_t>(p, p + l), r, x->id});
    return true;
  }
  static bool ready(void* c, const char*) { return static_cast<Peer*>(c)->online; }
  static bool w(void* c, const char* k, const void* d, size_t l) {
    static_cast<Peer*>(c)->nvs[k].assign(static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + l);
    return true;
  }
  static size_t r(void* c, const char* k, void* b, size_t cap) {
    auto& m = static_cast<Peer*>(c)->nvs;
    auto it = m.find(k);
    if (it == m.end() || it->second.size() > cap) return 0;
    memcpy(b, it->second.data(), it->second.size());
    return it->second.size();
  }
  static uint32_t ms(void*) { return T::now; }
  static uint32_t rnd(void* c) {
    Peer* x = static_cast<Peer*>(c);
    if (!x->script.empty()) { uint32_t v = x->script.front(); x->script.erase(x->script.begin()); return v; }
    static uint32_t s = 99; s = s * 1103515245u + 12345u; return s >> 4;
  }

  static uint32_t ut(void*) { return T::unixTime; }
  void start() {
    RaceProtocol::Platform p{this, id.c_str(), pub, ready, w, r, ms, rnd, &SeqFn<decltype(RaceProtocol::Platform::nextSeq)>::fn, ut};
    eng.init(p);
    eng.boot();
    for (auto& kv : retained) deliverTo(kv.second);
  }
  void deliverTo(const Msg& m) {
    if (!online) return;
    if (m.topic.rfind("r2/", 0) == 0 || m.topic == "r2q/" + id) eng.onPacket(m.group.c_str(), m.topic.c_str(), m.kind, m.payload.data(), m.payload.size());
  }
};

uint32_t peerSeq(void* c) { return ++static_cast<Peer*>(c)->seqCounter; }

void reset() {
  inflight.clear(); retained.clear(); peers.clear(); deviceOnline = true; dropIf = nullptr;
  T::onPublished = [](const T::Pub& p) {  // the device's own publishes, with their PacketCodec header
    PacketCodec::Header h; const uint8_t* payload = nullptr;
    if (!PacketCodec::decodeHeader(p.data.data(), p.data.size(), &h, &payload)) return;
    inflight.push_back({p.group, p.topic, h.packet_kind, std::vector<uint8_t>(payload, payload + h.payload_length), p.retained, Identity::deviceId()});
  };
}
void deliverToDevice(const Msg& m);  // defined by the test (calls race.cpp's handlers)
void pump() {
  for (int guard = 0; guard < 100 && !inflight.empty(); guard++) {
    Msg m = inflight.front(); inflight.pop_front();
    if (dropIf && dropIf(m)) continue;
    if (m.retained) retained[m.group + "|" + m.topic] = m;
    for (Peer* p : peers) p->deliverTo(m);
    bool toDevice = m.topic.rfind("r2/", 0) == 0 || m.topic == std::string("r2q/") + Identity::deviceId();
    if (deviceOnline && toDevice) deliverToDevice(m);
  }
}
}  // namespace Fx
