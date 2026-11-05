#pragma once
// Input a chat receives while its conversation index is still loading (chat
// entry latency, device A on 2ed04b6): held in arrival order and replayed
// once the index is ready, so nothing typed or rotated during the load is
// lost or reordered. Events carry their own accepted time (eventMs), so a
// replayed key keeps its rhythm. The buffer is heap-allocated only while
// something is held.
#include <stdint.h>
#include <string.h>
#include <new>

#include "core/input.h"

class HeldInput {
 public:
  // False if the event could not be kept (buffer full or no memory).
  bool hold(const InputEvent& e) {
    if (buf_ == nullptr) {
      buf_ = new (std::nothrow) InputEvent[kCap];
      if (buf_ == nullptr) return false;
    }
    if (pos_ > 0) {  // compact: what was already replayed is gone
      memmove(buf_, buf_ + pos_, sizeof(InputEvent) * (count_ - pos_));
      count_ = static_cast<uint8_t>(count_ - pos_);
      pos_ = 0;
    }
    if (count_ >= kCap) return false;
    buf_[count_++] = e;
    return true;
  }
  // The oldest held event not yet replayed.
  bool pop(InputEvent& e) {
    if (pos_ >= count_) return false;
    e = buf_[pos_++];
    return true;
  }
  // Puts back the event pop() just returned (it is replayed first next time).
  void unpop() {
    if (pos_ > 0) pos_--;
  }
  bool empty() const { return pos_ >= count_; }
  // True if an event of this type is held and not replayed yet.
  bool contains(InputEventType type) const {
    for (uint8_t i = pos_; i < count_; i++) {
      if (buf_[i].type == type) return true;
    }
    return false;
  }
  // Frees the buffer once everything held was replayed.
  void releaseIfEmpty() {
    if (empty()) clear();
  }
  void clear() {
    delete[] buf_;
    buf_ = nullptr;
    count_ = 0;
    pos_ = 0;
  }

 private:
  static constexpr uint8_t kCap = 32;
  InputEvent* buf_ = nullptr;
  uint8_t count_ = 0;
  uint8_t pos_ = 0;
};
