#pragma once

#include "gl_util.h"

#include <string>

struct mpv_handle;
struct mpv_render_context;

// A second, silent mpv instance that shows single frames on request, for the
// seek bar preview (it can open YouTube streams too). Frames are scaled into
// an sRGB texture (sampling returns linear colour). Nothing is logged.
// All methods must be called on the thread that owns the GL context.
class FrameGrabber {
 public:
  FrameGrabber() = default;
  FrameGrabber(const FrameGrabber&) = delete;
  FrameGrabber& operator=(const FrameGrabber&) = delete;
  ~FrameGrabber();

  // Frames are scaled down to fit maxWidth x maxHeight. Pass a copy-back
  // hwdec ("nvdec-copy") or "no": the frames are shrunk on the CPU.
  bool init(const std::string& hwdec, int maxWidth, int maxHeight);
  // Opens a file or stream, paused at `start` (mpv syntax: seconds or "20%").
  void open(const std::string& path, const std::string& start);
  void close();
  const std::string& path() const { return path_; }
  // Shows the frame near `seconds`. While a seek runs, only the latest request is kept.
  void seek(double seconds);
  // Polls mpv and renders a new frame if one is ready. True if the texture changed.
  bool update();
  // A frame of the current file is in texture().
  bool hasFrame() const { return hasFrame_; }
  // The current file could not be opened.
  bool failed() const { return failed_; }
  GLuint texture() const { return texture_; }
  GLuint framebuffer() const { return fbo_; }  // with texture() attached
  int width() const { return width_; }
  int height() const { return height_; }
  // The video's own size (before scaling down).
  int videoWidth() const { return videoWidth_; }
  int videoHeight() const { return videoHeight_; }

  // Part of the frame (0..1, y = 0 at the top) copied into croppedTexture(),
  // e.g. one eye of a 3D video.
  void setCrop(float x0, float y0, float x1, float y1);
  GLuint croppedTexture() const { return cropTexture_; }
  int croppedWidth() const { return cropWidth_; }
  int croppedHeight() const { return cropHeight_; }

 private:
  void updateVideoSize();
  void releaseTarget();
  void updateCrop();
  void sendSeek(double seconds, bool exact = false);

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  int maxWidth_ = 0, maxHeight_ = 0;
  std::string path_;
  GLuint texture_ = 0, fbo_ = 0;
  int width_ = 0, height_ = 0;
  int videoWidth_ = 0, videoHeight_ = 0;
  GLuint cropTexture_ = 0, cropFbo_ = 0;
  int cropWidth_ = 0, cropHeight_ = 0;
  float crop_[4] = {0, 0, 1, 1};
  bool fileLoaded_ = false;
  bool restarted_ = false;  // the file's first frame has been decoded
  bool seeking_ = false;
  double pendingSeek_ = -1;  // seconds, < 0 = none
  double target_ = -1;       // the last seek sent, seconds
  bool exact_ = true;        // ... and whether it was exact
  bool framePending_ = false;
  bool hasFrame_ = false;
  bool failed_ = false;
};
