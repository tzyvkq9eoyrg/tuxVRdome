#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

typedef struct _cairo_surface cairo_surface_t;
struct SwsContext;
struct AVFrame;

// Scales a decoded frame with `sws` into tightly packed 32-bit pixels
// (width × height), safely: swscale may write past a row's end. Also used
// by PhotoPreloader.
bool scaleInto(SwsContext* sws, const AVFrame* frame, int width, int height,
               std::vector<uint32_t>& pixels);

// Makes thumbnails of local videos and photos for the file browser: a frame
// from 20% into a video (one eye for 3D), or the photo itself. They are
// decoded with FFmpeg on the CPU in a background thread, so the GL thread,
// which also draws the headset's frames, never waits for them. Kept in
// memory only, never written to disk.
class Thumbnailer {
 public:
  // Size of the thumbnails in pixels (16:9).
  static constexpr int kWidth = 320;  // big enough for the gallery
  static constexpr int kHeight = 180;

  Thumbnailer() = default;
  Thumbnailer(const Thumbnailer&) = delete;
  Thumbnailer& operator=(const Thumbnailer&) = delete;
  ~Thumbnailer();

  void start();
  // The thumbnail if it is ready, else null. Failed files also give null.
  cairo_surface_t* find(const std::string& path) const;
  // The files that should get thumbnails, most urgent first (e.g. the rows on
  // screen, then those around them); replaces earlier wishes. These are also
  // kept when the cache runs full.
  void want(const std::vector<std::string>& paths);
  // Takes over thumbnails the worker has finished. True if there were any.
  bool update();

 private:
  // A decoded frame, shrunk to a few hundred pixels (BGRA, top row first).
  struct Frame {
    std::string path;
    bool ok = false;
    int width = 0, height = 0;            // of `pixels`
    int videoWidth = 0, videoHeight = 0;  // of the video itself
    std::vector<uint32_t> pixels;
  };
  void work();
  cairo_surface_t* makeThumbnail(const Frame& frame) const;

  std::thread worker_;
  std::mutex mutex_;  // guards the members below up to done_
  std::condition_variable wake_;
  std::deque<std::string> queue_;
  std::string working_;  // the file being decoded
  std::vector<Frame> finished_;
  bool quit_ = false;
  std::map<std::string, cairo_surface_t*> done_;  // GL thread only; null = could not be made
  std::set<std::string> wanted_;                  // GL thread only: the last want()
};
