#pragma once

#include <chrono>
#include <cstdio>

// Measures how long each part of the frame loop blocks and prints averages
// every two seconds (enabled with --timing).
class FrameTimer {
 public:
  enum Section { Events, XrWait, Mpv, Draw, Fence, XrEnd, Swap, kCount };

  explicit FrameTimer(bool enabled) : enabled_(enabled) {}
  void start() { last_ = Clock::now(); }
  void lap(Section s) {
    const auto now = Clock::now();
    totals_[s] += std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;
  }
  // Re-attributes part of an already measured section to another one.
  void move(Section from, Section to, double ms) {
    totals_[from] -= ms;
    totals_[to] += ms;
  }
  void finishFrame() {
    ++frames_;
    const double elapsed = std::chrono::duration<double>(Clock::now() - reportStart_).count();
    if (elapsed < 2.0) return;
    if (enabled_) {
      static const char* names[kCount] = {"events", "xrWait", "mpv", "draw", "fence", "xrEnd", "swap"};
      std::printf("[timing] %.1f fps |", frames_ / elapsed);
      for (int i = 0; i < kCount; ++i) std::printf(" %s %.2f ms", names[i], totals_[i] / frames_);
      std::printf("\n");
      std::fflush(stdout);
    }
    for (double& t : totals_) t = 0;
    frames_ = 0;
    reportStart_ = Clock::now();
  }

 private:
  using Clock = std::chrono::steady_clock;
  bool enabled_;
  Clock::time_point last_ = Clock::now();
  Clock::time_point reportStart_ = Clock::now();
  double totals_[kCount] = {};
  int frames_ = 0;
};
