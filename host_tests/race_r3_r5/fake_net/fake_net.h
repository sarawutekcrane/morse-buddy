#pragma once
// HOST TEST FAKE (not production): a scripted network for mqtt_manager.cpp.
//  - FreeRTOS queues/tasks on std::thread (FakeRtos): a "task" either runs on
//    its own thread (threads mode) or is not run at all (manual mode: the test
//    runs the worker's jobs itself, deterministically).
//  - A broker model per address (FakeNet::Script), DNS answers, gates that
//    hold the worker inside one step until the test releases it, and a log of
//    every MQTT operation with the thread it ran on.
//  - Checks: two tasks inside one client at once, two live sessions under one
//    client id, operations on a closed client.
// C++11 (the run_all.sh syntax check builds mqtt_manager.cpp with it).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace FakeRtos {
struct Shutdown {};
struct Queue;
struct World {
  std::mutex m;
  bool manual = true;          // tasks are not run (the test runs jobs itself)
  bool shuttingDown = false;
  std::vector<Queue*> queues;
  std::vector<std::thread> threads;
  int tasksCreated = 0;
  bool failTaskCreate = false;
};
inline World& world() {
  static World w;
  return w;
}
struct Queue {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::vector<uint8_t>> items;
  size_t itemSize = 0;
  size_t depth = 0;
};
}  // namespace FakeRtos

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef FakeRtos::Queue* QueueHandle_t;
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define errQUEUE_FULL 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
#define tskNO_AFFINITY 0x7fffffff

inline QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t itemSize) {
  FakeRtos::Queue* q = new FakeRtos::Queue();
  q->itemSize = itemSize;
  q->depth = depth;
  std::lock_guard<std::mutex> l(FakeRtos::world().m);
  FakeRtos::world().queues.push_back(q);
  return q;
}
inline bool fakeRtosShuttingDown() {
  std::lock_guard<std::mutex> l(FakeRtos::world().m);
  return FakeRtos::world().shuttingDown;
}
inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t wait) {
  std::unique_lock<std::mutex> l(q->m);
  while (q->items.size() >= q->depth) {
    if (wait == 0) return errQUEUE_FULL;
    bool manual;
    {
      std::lock_guard<std::mutex> wl(FakeRtos::world().m);
      manual = FakeRtos::world().manual;
    }
    if (manual) {  // single-threaded: nothing could ever make room
      fprintf(stderr, "FAKE RTOS: blocking send to a full queue in manual mode (test would hang)\n");
      abort();
    }
    if (fakeRtosShuttingDown()) throw FakeRtos::Shutdown();
    q->cv.wait_for(l, std::chrono::milliseconds(5));
  }
  const uint8_t* p = static_cast<const uint8_t*>(item);
  q->items.push_back(std::vector<uint8_t>(p, p + q->itemSize));
  q->cv.notify_all();
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t wait) {
  std::unique_lock<std::mutex> l(q->m);
  while (q->items.empty()) {
    if (wait == 0) return pdFALSE;
    if (fakeRtosShuttingDown()) throw FakeRtos::Shutdown();
    q->cv.wait_for(l, std::chrono::milliseconds(5));
  }
  memcpy(out, q->items.front().data(), q->itemSize);
  q->items.pop_front();
  q->cv.notify_all();
  return pdTRUE;
}
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
  std::lock_guard<std::mutex> l(q->m);
  return static_cast<UBaseType_t>(q->items.size());
}
inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char*, uint32_t, void* arg, UBaseType_t,
                                          TaskHandle_t* handle, BaseType_t) {
  FakeRtos::World& w = FakeRtos::world();
  std::lock_guard<std::mutex> l(w.m);
  if (w.failTaskCreate) return pdFAIL;
  w.tasksCreated++;
  if (!w.manual) {
    w.threads.push_back(std::thread([fn, arg]() {
      try {
        fn(arg);
      } catch (FakeRtos::Shutdown&) {
      }
    }));
  }
  if (handle != nullptr) *handle = reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(w.tasksCreated));
  return pdPASS;
}
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
inline void vTaskDelay(TickType_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct portMUX_TYPE {
  std::mutex m;
};
#define portMUX_INITIALIZER_UNLOCKED \
  {}
#define portENTER_CRITICAL(mux) ((mux)->m.lock())
#define portEXIT_CRITICAL(mux) ((mux)->m.unlock())

namespace FakeRtos {
// Stops every task thread (they leave their blocking queue wait) and joins them.
inline void stopAll() {
  std::vector<std::thread> ts;
  {
    std::lock_guard<std::mutex> l(world().m);
    world().shuttingDown = true;
    ts.swap(world().threads);
  }
  for (auto& t : ts) t.join();
}
}  // namespace FakeRtos

// ---------------------------------------------------------------------------
// Scripted network
// ---------------------------------------------------------------------------
namespace FakeNet {
// Behaviour of the broker reachable at one address.
struct Script {
  bool tcpOk = true;
  int connackError = 0;       // 0: CONNACK accepted; else lwmqtt error returned by connect()
  int connackReturnCode = 0;  // with connackError -10 (refused)
  int failSubscribeAt = -1;   // index of the SUBSCRIBE that fails (no SUBACK, -9)
  bool failPublishOnline = false;
};

struct Event {
  std::string kind;  // connect, subscribe, publish, disconnect, close, loop
  std::string clientId;
  std::string topic;
  std::string payload;
  bool retained = false;
  int qos = 0;
  bool cleanSession = false;
  std::string willTopic, willPayload;
  bool willRetained = false;
  int willQos = 0;
  int keepAlive = 0;
  uint32_t ip = 0;
  std::thread::id thread;
};

struct Msg {
  std::string topic, payload;
};

struct State {
  std::mutex m;
  std::condition_variable cv;
  std::map<uint32_t, Script> scripts;  // by address (network byte order)
  Script defaultScript;
  std::deque<uint32_t> dnsAnswers;     // consumed in order; the last one repeats
  bool dnsFail = false;
  int dnsQueries = 0;
  // Gate: the worker blocks when it reaches this step ("dns", "tcp",
  // "connack", "subscribe", "online", "cleanup-connect") until released.
  std::string gate;
  bool atGate = false;
  bool releaseGate = false;
  // Broker model.
  std::map<std::string, int> liveSessions;  // client id -> live connections
  int sameIdOverlap = 0;                    // a CONNECT while that id was already live
  int concurrentUse = 0;                    // two threads inside one client at once
  int useAfterClose = 0;                    // subscribe/publish on a client that is not connected
  std::vector<Event> log;
  std::map<std::string, std::vector<Msg>> deliverOnSubscribe;  // topic suffix -> retained messages sent at SUBSCRIBE
  std::map<std::string, std::vector<Msg>> inbox;               // client id -> messages for its next loop()
  std::set<std::string> dropNext;                              // client ids whose connection drops at next loop()
  int livePublishFailures = 0;                                 // publishes after setup that fail (connection closes)
};
inline State& st() {
  static State s;
  return s;
}
inline Script scriptFor(uint32_t ip) {
  std::lock_guard<std::mutex> l(st().m);
  std::map<uint32_t, Script>::iterator it = st().scripts.find(ip);
  return it == st().scripts.end() ? st().defaultScript : it->second;
}
inline void record(const Event& e) {
  std::lock_guard<std::mutex> l(st().m);
  st().log.push_back(e);
}
// Blocks the calling (worker) thread at `step` while it is the gate.
inline void maybeGate(const char* step) {
  std::unique_lock<std::mutex> l(st().m);
  if (st().gate != step) return;
  st().atGate = true;
  st().cv.notify_all();
  while (!st().releaseGate) {
    if (fakeRtosShuttingDown()) {
      st().atGate = false;
      throw FakeRtos::Shutdown();
    }
    st().cv.wait_for(l, std::chrono::milliseconds(5));
  }
  st().atGate = false;
  st().releaseGate = false;
  st().gate.clear();
}
inline void setGate(const std::string& step) {
  std::lock_guard<std::mutex> l(st().m);
  st().gate = step;
  st().atGate = false;
  st().releaseGate = false;
}
inline bool waitAtGate(int ms = 3000) {
  std::unique_lock<std::mutex> l(st().m);
  return st().cv.wait_for(l, std::chrono::milliseconds(ms), [] { return st().atGate; });
}
inline bool atGate() {
  std::lock_guard<std::mutex> l(st().m);
  return st().atGate;
}
inline void release() {
  std::lock_guard<std::mutex> l(st().m);
  st().releaseGate = true;
  st().cv.notify_all();
}
inline uint32_t ip4(int a, int b, int c, int d) {
  uint8_t bytes[4] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d)};
  uint32_t v;
  memcpy(&v, bytes, 4);
  return v;  // as stored in sin_addr
}
inline std::vector<Event> events() {
  std::lock_guard<std::mutex> l(st().m);
  return st().log;
}
inline void reset() {
  std::lock_guard<std::mutex> l(st().m);
  State& s = st();
  s.scripts.clear();
  s.defaultScript = Script();
  s.dnsAnswers.clear();
  s.dnsFail = false;
  s.dnsQueries = 0;
  s.gate.clear();
  s.atGate = false;
  s.releaseGate = false;
  s.liveSessions.clear();
  s.sameIdOverlap = 0;
  s.concurrentUse = 0;
  s.useAfterClose = 0;
  s.log.clear();
  s.deliverOnSubscribe.clear();
  s.inbox.clear();
  s.dropNext.clear();
  s.livePublishFailures = 0;
}
}  // namespace FakeNet

// lwIP resolver (lwip/netdb.h)
inline int lwip_getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo** res) {
  FakeNet::maybeGate("dns");
  uint32_t ip = 0;
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dnsQueries++;
    if (FakeNet::st().dnsFail || FakeNet::st().dnsAnswers.empty()) {
      *res = nullptr;
      return EAI_FAIL;
    }
    ip = FakeNet::st().dnsAnswers.front();
    if (FakeNet::st().dnsAnswers.size() > 1) FakeNet::st().dnsAnswers.pop_front();
  }
  struct addrinfo* a = new struct addrinfo();
  struct sockaddr_in* sin = new struct sockaddr_in();
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = ip;
  a->ai_family = AF_INET;
  a->ai_socktype = SOCK_STREAM;
  a->ai_addr = reinterpret_cast<struct sockaddr*>(sin);
  a->ai_addrlen = sizeof(*sin);
  a->ai_next = nullptr;
  *res = a;
  return 0;
}
inline void lwip_freeaddrinfo(struct addrinfo* a) {
  while (a != nullptr) {
    struct addrinfo* next = a->ai_next;
    delete reinterpret_cast<struct sockaddr_in*>(a->ai_addr);
    delete a;
    a = next;
  }
}

// ---------------------------------------------------------------------------
// Arduino networking surface
// ---------------------------------------------------------------------------
class IPAddress {
 public:
  IPAddress() : v_(0) {}
  IPAddress(uint32_t v) : v_(v) {}
  operator uint32_t() const { return v_; }

 private:
  uint32_t v_;
};

class Client {
 public:
  virtual ~Client() {}
  virtual int connect(IPAddress ip, uint16_t port) = 0;
  virtual size_t write(const uint8_t* buf, size_t size) = 0;
  virtual void stop() = 0;
  virtual uint8_t connected() = 0;
};

class WiFiClient : public Client {
 public:
  WiFiClient() {}
  int connect(IPAddress ip, uint16_t port) override { return connect(ip, port, 3000); }
  int connect(IPAddress ip, uint16_t, int32_t) {
    FakeNet::maybeGate("tcp");
    ip_ = ip;
    FakeNet::Script s = FakeNet::scriptFor(ip);
    tcpUp_ = s.tcpOk;
    return s.tcpOk ? 1 : 0;
  }
  size_t write(const uint8_t*, size_t size) override { return size; }
  size_t write(uint8_t) { return 1; }
  void stop() override {
    tcpUp_ = false;
    fd_ = -1;
  }
  uint8_t connected() override { return tcpUp_ ? 1 : 0; }
  int fd() const { return fd_; }
  // Test hook: back this client with a real socket (for the bounded write).
  void attachFd(int fd) {
    fd_ = fd;
    tcpUp_ = true;
  }
  uint32_t ip() const { return ip_; }

 private:
  int fd_ = -1;
  bool tcpUp_ = false;
  uint32_t ip_ = 0;
};
