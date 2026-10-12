#include "frame_grabber.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdio>

namespace {

void* getProcAddress(void*, const char* name) {
  return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

}  // namespace

FrameGrabber::~FrameGrabber() {
  if (render_) mpv_render_context_free(render_);
  releaseTarget();
  if (mpv_) mpv_terminate_destroy(mpv_);
}

bool FrameGrabber::init(const std::string& hwdec, int maxWidth, int maxHeight) {
  std::setlocale(LC_NUMERIC, "C");
  maxWidth_ = maxWidth;
  maxHeight_ = maxHeight;
  mpv_ = mpv_create();
  if (!mpv_) return false;
  mpv_set_option_string(mpv_, "vo", "libmpv");
  mpv_set_option_string(mpv_, "hwdec", hwdec.c_str());
  mpv_set_option_string(mpv_, "pause", "yes");
  mpv_set_option_string(mpv_, "keep-open", "yes");
  mpv_set_option_string(mpv_, "idle", "yes");
  mpv_set_option_string(mpv_, "aid", "no");
  mpv_set_option_string(mpv_, "sid", "no");
  mpv_set_option_string(mpv_, "osd-level", "0");
  mpv_set_option_string(mpv_, "video-timing-offset", "0");
  // Seek to keyframes only: fast, and close enough for a preview.
  mpv_set_option_string(mpv_, "hr-seek", "no");
  mpv_set_option_string(mpv_, "demuxer-max-bytes", "32MiB");
  // Shrink each frame on the CPU, in mpv's own threads, before it reaches
  // OpenGL: the GL thread (which also draws the headset's frames) then only
  // handles a small picture, not an 8K one. Copy-back decoding hands the
  // decoded frames to that filter.
  const std::string scale = "scale=w=" + std::to_string(maxWidth) + ":h=" +
                            std::to_string(maxHeight) + ":force_original_aspect_ratio=decrease";
  mpv_set_option_string(mpv_, "vf", scale.c_str());
  // Cheap scaling: these frames are small, quality filters only cost GPU time.
  mpv_set_option_string(mpv_, "scale", "bilinear");
  mpv_set_option_string(mpv_, "dscale", "bilinear");
  mpv_set_option_string(mpv_, "cscale", "bilinear");
  mpv_set_option_string(mpv_, "correct-downscaling", "no");
  mpv_set_option_string(mpv_, "linear-downscaling", "no");
  mpv_set_option_string(mpv_, "sigmoid-upscaling", "no");
  mpv_set_option_string(mpv_, "deband", "no");
  mpv_set_option_string(mpv_, "dither-depth", "no");
  mpv_set_option_string(mpv_, "ytdl", "yes");
  // A small stream is plenty for a preview.
  mpv_set_option_string(mpv_, "ytdl-format",
                        "bestvideo[height<=?720]/best[height<=?720]/worst");
  // Silent: no file names in the console output.
  mpv_set_option_string(mpv_, "terminal", "no");
  mpv_set_option_string(mpv_, "msg-level", "all=no");
  if (mpv_initialize(mpv_) < 0) {
    std::fprintf(stderr, "Frame grabber: mpv_initialize failed\n");
    return false;
  }
  mpv_opengl_init_params glInit{getProcAddress, nullptr};
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  if (mpv_render_context_create(&render_, mpv_, params) < 0) {
    std::fprintf(stderr, "Frame grabber: mpv_render_context_create failed\n");
    return false;
  }
  return true;
}

void FrameGrabber::open(const std::string& path, const std::string& start) {
  path_ = path;
  fileLoaded_ = false;
  restarted_ = false;
  seeking_ = false;
  pendingSeek_ = -1;
  target_ = -1;
  exact_ = true;
  hasFrame_ = false;
  failed_ = false;
  videoWidth_ = videoHeight_ = 0;
  const std::string options = "start=" + start;
  const char* cmd[] = {"loadfile", path.c_str(), "replace", "-1", options.c_str(), nullptr};
  mpv_command(mpv_, cmd);  // returns at once; mpv opens the file in its own thread
}

void FrameGrabber::close() {
  if (path_.empty()) return;
  const char* cmd[] = {"stop", nullptr};
  mpv_command(mpv_, cmd);
  path_.clear();
  fileLoaded_ = false;
  restarted_ = false;
  hasFrame_ = false;
  failed_ = false;
}

// First a quick keyframe seek (it lands on the keyframe in the direction of
// the seek, which in videos with few keyframes can be far off); once the
// pointer rests, update() follows with an exact one.
void FrameGrabber::sendSeek(double seconds, bool exact) {
  const std::string target = std::to_string(seconds);
  const char* cmd[] = {"seek", target.c_str(), exact ? "absolute+exact" : "absolute+keyframes",
                       nullptr};
  mpv_command_async(mpv_, 0, cmd);
  seeking_ = true;
  target_ = seconds;
  exact_ = exact;
}

void FrameGrabber::seek(double seconds) {
  if (path_.empty()) return;
  if (seconds == target_ && pendingSeek_ < 0) return;  // already there or on its way
  if (!fileLoaded_ || seeking_)
    pendingSeek_ = seconds;
  else
    sendSeek(seconds);
}

void FrameGrabber::setCrop(float x0, float y0, float x1, float y1) {
  if (x0 == crop_[0] && y0 == crop_[1] && x1 == crop_[2] && y1 == crop_[3]) return;
  crop_[0] = x0;
  crop_[1] = y0;
  crop_[2] = x1;
  crop_[3] = y1;
  updateCrop();
}

bool FrameGrabber::update() {
  if (!mpv_) return false;
  while (true) {
    mpv_event* ev = mpv_wait_event(mpv_, 0);
    if (ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
      case MPV_EVENT_FILE_LOADED:
        fileLoaded_ = true;
        if (pendingSeek_ >= 0 && !seeking_) {
          sendSeek(pendingSeek_);
          pendingSeek_ = -1;
        }
        break;
      case MPV_EVENT_VIDEO_RECONFIG:
        updateVideoSize();
        break;
      case MPV_EVENT_PLAYBACK_RESTART:
        if (fileLoaded_) restarted_ = true;
        seeking_ = false;
        if (pendingSeek_ >= 0) {
          sendSeek(pendingSeek_);
          pendingSeek_ = -1;
        } else if (!exact_ && target_ >= 0) {
          sendSeek(target_, true);  // the pointer rests: the exact frame
        }
        break;
      case MPV_EVENT_END_FILE: {
        const auto* end = static_cast<mpv_event_end_file*>(ev->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR) failed_ = true;
        break;
      }
      default:
        break;
    }
  }
  if (!render_) return false;
  if (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME) framePending_ = true;
  // Before the first restart there is no frame of this file yet.
  if (!framePending_ || !fbo_ || !restarted_) return false;
  framePending_ = false;

  mpv_opengl_fbo fbo{static_cast<int>(fbo_), width_, height_, 0};
  int flipY = 0;  // texel row 0 = top of the image
  int block = 0;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flipY},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  glDisable(GL_FRAMEBUFFER_SRGB);  // mpv writes sRGB values itself
  mpv_render_context_render(render_, params);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  hasFrame_ = true;
  updateCrop();
  return true;
}

void FrameGrabber::updateVideoSize() {
  int64_t w = 0, h = 0;
  if (mpv_get_property(mpv_, "dwidth", MPV_FORMAT_INT64, &w) < 0 ||
      mpv_get_property(mpv_, "dheight", MPV_FORMAT_INT64, &h) < 0 || w <= 0 || h <= 0)
    return;
  // The video's own size, before the scale filter.
  int64_t vw = 0, vh = 0;
  if (mpv_get_property(mpv_, "video-dec-params/w", MPV_FORMAT_INT64, &vw) >= 0 &&
      mpv_get_property(mpv_, "video-dec-params/h", MPV_FORMAT_INT64, &vh) >= 0 && vw > 0 &&
      vh > 0) {
    videoWidth_ = static_cast<int>(vw);
    videoHeight_ = static_cast<int>(vh);
  } else {
    videoWidth_ = static_cast<int>(w);
    videoHeight_ = static_cast<int>(h);
  }
  const double s = std::min({1.0, double(maxWidth_) / w, double(maxHeight_) / h});
  const int tw = std::max(2, static_cast<int>(w * s)), th = std::max(2, static_cast<int>(h * s));
  if (fbo_ && tw == width_ && th == height_) return;
  releaseTarget();
  width_ = tw;
  height_ = th;
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, width_, height_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) releaseTarget();
  framePending_ = true;
}

void FrameGrabber::updateCrop() {
  if (!fbo_ || !hasFrame_) return;
  const int x0 = static_cast<int>(crop_[0] * width_), x1 = static_cast<int>(crop_[2] * width_);
  const int y0 = static_cast<int>(crop_[1] * height_), y1 = static_cast<int>(crop_[3] * height_);
  const int w = std::max(2, x1 - x0), h = std::max(2, y1 - y0);
  if (!cropFbo_ || w != cropWidth_ || h != cropHeight_) {
    if (cropFbo_) glDeleteFramebuffers(1, &cropFbo_);
    if (cropTexture_) glDeleteTextures(1, &cropTexture_);
    cropWidth_ = w;
    cropHeight_ = h;
    glGenTextures(1, &cropTexture_);
    glBindTexture(GL_TEXTURE_2D, cropTexture_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &cropFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, cropFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cropTexture_, 0);
  }
  // A plain copy: with GL_FRAMEBUFFER_SRGB off, sRGB values pass unchanged.
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cropFbo_);
  glBlitFramebuffer(x0, y0, x0 + w, y0 + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void FrameGrabber::releaseTarget() {
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (texture_) glDeleteTextures(1, &texture_);
  if (cropFbo_) glDeleteFramebuffers(1, &cropFbo_);
  if (cropTexture_) glDeleteTextures(1, &cropTexture_);
  fbo_ = texture_ = cropFbo_ = cropTexture_ = 0;
  width_ = height_ = cropWidth_ = cropHeight_ = 0;
}
