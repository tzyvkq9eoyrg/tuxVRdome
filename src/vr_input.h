#pragma once

#include "xr_context.h"

#include <algorithm>
#include <cmath>

// Fires once when an input goes down, then repeatedly while it stays down.
class HoldRepeat {
 public:
  HoldRepeat(double delay, double interval) : delay_(delay), interval_(interval) {}

  bool update(bool held, double now) {
    if (!held) {
      held_ = false;
      return false;
    }
    if (!held_) {
      held_ = true;
      start_ = now;
      next_ = now + delay_;
      return true;
    }
    if (now >= next_) {
      next_ = std::max(next_ + interval_, now);
      return true;
    }
    return false;
  }
  double heldSeconds(double now) const { return held_ ? now - start_ : 0.0; }

 private:
  double delay_, interval_;
  bool held_ = false;
  double start_ = 0, next_ = 0;
};

// Which way a thumbstick is pushed, counting only its dominant axis.
struct StickDirections {
  bool left = false, right = false, up = false, down = false;
};

inline StickDirections stickDirections(const HandState& hand) {
  constexpr float kThreshold = 0.6f;
  StickDirections d;
  if (std::abs(hand.stickX) > std::abs(hand.stickY)) {
    d.left = hand.stickX < -kThreshold;
    d.right = hand.stickX > kThreshold;
  } else {
    d.up = hand.stickY > kThreshold;
    d.down = hand.stickY < -kThreshold;
  }
  return d;
}

// Seek step in seconds that grows the longer the input is held.
inline int seekStep(double heldSeconds) {
  if (heldSeconds < 1.5) return 10;
  if (heldSeconds < 4.0) return 30;
  return 60;
}
