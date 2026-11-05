#pragma once
// The semantic input event queue (FIFO, fixed capacity) behind
// Input::popEvent() and friends, kept separate from input.cpp's GPIO and
// timing code so host tests run this exact code.
//
// Hardware Fix #4.7d Part H: raised from 8 to 32. Fix #4.7b's independent
// DOT/DASH timer can now accumulate up to kButtonEdgeQueueCap (16) accepted
// edges before drainButtonEdges() converts them into semantic InputEvents
// in a single Input::update() call, plus whatever ENCODER_ROTATE/encoder-
// switch activity also occurred during the same busy interval -- an 8-slot
// semantic queue could silently drop the tail of that backlog right here,
// even though every physical edge had already been captured reliably.
// 32 comfortably covers the full 16-edge DOT backlog plus normal
// concurrent encoder activity, at a small, acceptable RAM cost.
#include <stdint.h>

#include "core/input.h"

class InputQueue {
 public:
  static constexpr uint8_t kCapacity = 32;

  // Appends an event; when full the event is dropped (and counted) -- except
  // a Back (ENCODER_LONG): a screen that stopped draining (a chat loading
  // its index) must still see it, so it displaces the newest queued event
  // that is not a Back, which is counted as dropped instead. The order of
  // everything kept is unchanged. (A queue of nothing but Backs drops it.)
  void push(const InputEvent& e) {
    if (count_ >= kCapacity) {  // drop if the consumer is not draining fast enough
      dropped_++;
      if (e.type != InputEventType::ENCODER_LONG) return;
      uint8_t k = count_;
      while (k > 0 && q_[(head_ + k - 1) % kCapacity].type == InputEventType::ENCODER_LONG) k--;
      if (k == 0) return;
      for (uint8_t i = k; i < count_; i++) q_[(head_ + i - 1) % kCapacity] = q_[(head_ + i) % kCapacity];  // close the gap
      tail_ = static_cast<uint8_t>((tail_ + kCapacity - 1) % kCapacity);
      count_--;
    }
    q_[tail_] = e;
    tail_ = static_cast<uint8_t>((tail_ + 1) % kCapacity);
    count_++;
  }
  bool pop(InputEvent& out) {
    if (count_ == 0) {
      out = InputEvent{};
      return false;
    }
    out = q_[head_];
    head_ = static_cast<uint8_t>((head_ + 1) % kCapacity);
    count_--;
    return true;
  }
  bool peek(InputEvent& out) const {
    if (count_ == 0) return false;
    out = q_[head_];
    return true;
  }
  // Puts an event just popped back at the front.
  void unpop(const InputEvent& e) {
    if (count_ >= kCapacity) {  // cannot happen right after a pop
      dropped_++;
      return;
    }
    head_ = static_cast<uint8_t>((head_ + kCapacity - 1) % kCapacity);
    q_[head_] = e;
    count_++;
  }
  bool contains(InputEventType type) const {
    for (uint8_t i = 0; i < count_; i++) {
      if (q_[(head_ + i) % kCapacity].type == type) return true;
    }
    return false;
  }
  bool full() const { return count_ >= kCapacity; }
  uint8_t size() const { return count_; }
  uint32_t dropped() const { return dropped_; }
  void clear() {
    head_ = tail_ = count_ = 0;
    dropped_ = 0;
  }

 private:
  InputEvent q_[kCapacity];
  uint8_t head_ = 0;
  uint8_t tail_ = 0;
  uint8_t count_ = 0;
  uint32_t dropped_ = 0;
};
