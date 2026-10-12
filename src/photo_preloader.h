#pragma once

#include "gl_util.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Shows photos: FFmpeg decodes them in a background thread, then they are
// uploaded into a mipmapped sRGB texture a few megabytes per frame, so the
// headset's frames never stall. The next photo is decoded ahead, so
// switching to it (by hand or in a slideshow) shows it at once.
class PhotoPreloader {
 public:
  PhotoPreloader() = default;
  PhotoPreloader(const PhotoPreloader&) = delete;
  PhotoPreloader& operator=(const PhotoPreloader&) = delete;
  ~PhotoPreloader();

  void start();
  // The photo on screen (decoded first) and the one to have ready next;
  // replaces earlier requests still waiting.
  void request(const std::string& current, const std::string& next);
  // GL thread, every frame: uploads a decoded photo a part at a time.
  void update();
  // The finished texture of `path`, or 0. Requires the GL context.
  GLuint texture(const std::string& path, int& width, int& height);

 private:
  struct Slot {
    std::string path;
    GLuint texture = 0;
    int width = 0, height = 0;
    int uploadedRows = 0;
    bool ready = false;
    std::vector<uint32_t> pixels;  // RGBA, until uploaded
  };
  void work();

  Slot slots_[2];  // the current photo and the next one
  int shown_ = -1;
  std::string current_, next_;

  std::thread worker_;
  std::mutex mutex_;  // guards the members below
  std::condition_variable wake_;
  std::deque<std::string> wanted_;  // photos to decode, most urgent first
  std::string decoding_;
  bool done_ = false;  // a decoded photo waits in decoded_
  Slot decoded_;
  bool quit_ = false;
};
