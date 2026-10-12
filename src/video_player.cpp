#include "video_player.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void* getProcAddress(void*, const char* name) {
  return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
    text.replace(pos, from.size(), to);
}

// Keeps file names and folder structure out of the console output: the
// loaded file becomes <video>, any other absolute path becomes <path>.
std::string redactPaths(std::string text, const std::string& videoPath) {
  replaceAll(text, videoPath, "<video>");
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const char prev = i > 0 ? text[i - 1] : ' ';
    const bool pathStart = text[i] == '/' && (std::isspace(static_cast<unsigned char>(prev)) ||
                                              std::strchr("'\"(=:[", prev) != nullptr);
    if (!pathStart) {
      out += text[i++];
      continue;
    }
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
           text[i] != '\'' && text[i] != '"')
      ++i;
    out += "<path>";
  }
  return out;
}

// Reads mpv's "chapter-list" property: an array of maps with time and title.
std::vector<Chapter> parseChapters(const mpv_node& list) {
  std::vector<Chapter> chapters;
  if (list.format != MPV_FORMAT_NODE_ARRAY) return chapters;
  for (int i = 0; i < list.u.list->num; ++i) {
    const mpv_node& item = list.u.list->values[i];
    if (item.format != MPV_FORMAT_NODE_MAP) continue;
    Chapter c;
    for (int k = 0; k < item.u.list->num; ++k) {
      const std::string key = item.u.list->keys[k];
      const mpv_node& v = item.u.list->values[k];
      if (key == "time" && v.format == MPV_FORMAT_DOUBLE) c.time = v.u.double_;
      else if (key == "title" && v.format == MPV_FORMAT_STRING) c.title = v.u.string;
    }
    chapters.push_back(c);
  }
  return chapters;
}

// Reads mpv's "track-list" property: an array of maps, one per track.
std::vector<Track> parseTracks(const mpv_node& list) {
  std::vector<Track> tracks;
  if (list.format != MPV_FORMAT_NODE_ARRAY) return tracks;
  for (int i = 0; i < list.u.list->num; ++i) {
    const mpv_node& item = list.u.list->values[i];
    if (item.format != MPV_FORMAT_NODE_MAP) continue;
    Track t;
    for (int k = 0; k < item.u.list->num; ++k) {
      const std::string key = item.u.list->keys[k];
      const mpv_node& v = item.u.list->values[k];
      if (key == "id" && v.format == MPV_FORMAT_INT64) t.id = v.u.int64;
      else if (key == "type" && v.format == MPV_FORMAT_STRING) t.type = v.u.string;
      else if (key == "title" && v.format == MPV_FORMAT_STRING) t.title = v.u.string;
      else if (key == "lang" && v.format == MPV_FORMAT_STRING) t.lang = v.u.string;
      else if (key == "codec" && v.format == MPV_FORMAT_STRING) t.codec = v.u.string;
      else if (key == "selected" && v.format == MPV_FORMAT_FLAG) t.selected = v.u.flag != 0;
    }
    tracks.push_back(t);
  }
  return tracks;
}

}  // namespace

VideoPlayer::~VideoPlayer() {
  if (render_) mpv_render_context_free(render_);
  releaseTarget();
  if (mpv_) mpv_terminate_destroy(mpv_);
}

bool VideoPlayer::init(const std::string& hwdec) {
  // libmpv refuses to start unless numbers are formatted the C way.
  std::setlocale(LC_NUMERIC, "C");

  mpv_ = mpv_create();
  if (!mpv_) {
    std::fprintf(stderr, "mpv_create failed\n");
    return false;
  }
  mpv_set_option_string(mpv_, "vo", "libmpv");
  mpv_set_option_string(mpv_, "hwdec", hwdec.c_str());
  mpv_set_option_string(mpv_, "keep-open", "yes");
  // A still image (e.g. a cover) stays rather than ending playback after a second.
  mpv_set_option_string(mpv_, "image-display-duration", "inf");
  mpv_set_option_string(mpv_, "idle", "yes");
  // Output linear light into a float texture, so mipmapping is correct and the
  // sRGB swapchain does the final encoding. HDR is tone-mapped by mpv.
  mpv_set_option_string(mpv_, "target-trc", "linear");
  mpv_set_option_string(mpv_, "target-prim", "bt.709");
  // HDR video on the headset's SDR displays: the BT.2390 tone curve, with
  // the brightness measured per scene.
  // (The render API draws with vo_gpu, which knows fewer curves than gpu-next.)
  mpv_set_option_string(mpv_, "tone-mapping", "bt.2390");
  mpv_set_option_string(mpv_, "hdr-compute-peak", "yes");
  // Keep the videos' own metadata (titles, descriptions) out of the log.
  mpv_set_option_string(mpv_, "display-tags", "");
  // FFmpeg's demuxer warns about harmless things (e.g. fonts attached to MKV
  // files, "could not find codec parameters"); only its errors are of use.
  mpv_set_option_string(mpv_, "msg-level", "ffmpeg/demuxer=error");
  mpv_set_option_string(mpv_, "dither-depth", "no");
  // Required when rendering without blocking for the target time.
  mpv_set_option_string(mpv_, "video-timing-offset", "0");
  // mpv's OSD would be cut in half on stereo video; the VR menu replaces it.
  mpv_set_option_string(mpv_, "osd-level", "0");
  // YouTube and other web links are resolved through yt-dlp.
  mpv_set_option_string(mpv_, "ytdl", "yes");
  // A large read-ahead buffer, so 8K streams survive slow moments.
  mpv_set_option_string(mpv_, "cache", "yes");
  mpv_set_option_string(mpv_, "demuxer-max-bytes", "1536MiB");
  mpv_set_option_string(mpv_, "demuxer-max-back-bytes", "256MiB");
  mpv_request_log_messages(mpv_, "info");

  if (mpv_initialize(mpv_) < 0) {
    std::fprintf(stderr, "mpv_initialize failed\n");
    return false;
  }
  mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "volume", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "mute", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "media-title", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "track-list", MPV_FORMAT_NODE);
  // Available even while mpv itself does not draw the subtitles.
  mpv_observe_property(mpv_, 0, "sub-text", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "speed", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "ab-loop-a", MPV_FORMAT_STRING);  // "no" or seconds
  mpv_observe_property(mpv_, 0, "ab-loop-b", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "eof-reached", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, 0, "video-format", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "video-params/pixelformat", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "container-fps", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, 0, "audio-codec-name", MPV_FORMAT_STRING);
  mpv_observe_property(mpv_, 0, "audio-params/channel-count", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, 0, "chapter-list", MPV_FORMAT_NODE);
  mpv_observe_property(mpv_, 0, "chapter", MPV_FORMAT_INT64);

  mpv_opengl_init_params glInit{getProcAddress, nullptr};
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  if (mpv_render_context_create(&render_, mpv_, params) < 0) {
    std::fprintf(stderr, "mpv_render_context_create failed\n");
    return false;
  }
  return true;
}

static double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void VideoPlayer::load(const std::string& path) {
  path_ = path;
  loading_ = true;
  fileLoaded_ = false;
  newVideoSized_ = false;
  retries_ = 0;
  resumeAt_ = 0;
  const char* cmd[] = {"loadfile", path.c_str(), nullptr};
  mpv_command(mpv_, cmd);
}

void VideoPlayer::stop() {
  const char* cmd[] = {"stop", nullptr};
  mpv_command(mpv_, cmd);
  path_.clear();
  loading_ = false;
  releaseTarget();  // no stale frame from the closed file
}

void VideoPlayer::command(const char* cmd) {
  const int err = mpv_command_string(mpv_, cmd);
  if (err < 0) std::fprintf(stderr, "mpv command '%s' failed: %s\n", cmd, mpv_error_string(err));
}

void VideoPlayer::seekTo(double seconds) {
  const std::string target = std::to_string(seconds);
  const char* cmd[] = {"seek", target.c_str(), "absolute", nullptr};
  mpv_command(mpv_, cmd);
}

void VideoPlayer::addSubtitle(const std::string& path) {
  const char* cmd[] = {"sub-add", path.c_str(), "select", nullptr};
  mpv_command(mpv_, cmd);
}

void VideoPlayer::setSubtitlesInFrame(bool visible) {
  if (visible == subtitlesInFrame_) return;
  subtitlesInFrame_ = visible;
  mpv_set_property_string(mpv_, "sub-visibility", visible ? "yes" : "no");
}

void VideoPlayer::setSubtitleStyle(double scale, int raise) {
  mpv_set_property_string(mpv_, "sub-scale", std::to_string(scale).c_str());
  // sub-pos: 100 = the bottom edge, smaller = higher up.
  mpv_set_property_string(mpv_, "sub-pos", std::to_string(std::clamp(100 - raise, 0, 150)).c_str());
}

void VideoPlayer::setMaxStreamWidth(int width) {
  // Without a limit, leave the choice to yt-dlp's own default: forcing a
  // format can pick streams YouTube refuses to serve (HTTP 403).
  const std::string w = std::to_string(width);
  const std::string format =
      width > 0 ? "bestvideo[width<=?" + w + "]+bestaudio/best[width<=?" + w + "]/best" : "";
  mpv_set_property_string(mpv_, "ytdl-format", format.c_str());
}

void VideoPlayer::setPicture(int brightness, int contrast, int saturation) {
  mpv_set_property_string(mpv_, "brightness", std::to_string(brightness).c_str());
  mpv_set_property_string(mpv_, "contrast", std::to_string(contrast).c_str());
  mpv_set_property_string(mpv_, "saturation", std::to_string(saturation).c_str());
}

void VideoPlayer::reloadAt(double seconds) {
  if (path_.empty()) return;
  loading_ = true;
  fileLoaded_ = false;
  newVideoSized_ = false;
  const std::string start = "start=" + std::to_string(seconds);
  const char* cmd[] = {"loadfile", path_.c_str(), "replace", "-1", start.c_str(), nullptr};
  mpv_command(mpv_, cmd);
}

void VideoPlayer::pollEvents() {
  // Files without video never deliver a first frame; stop waiting for one.
  if (loading_ && fileLoaded_ && nowSeconds() - loadedAt_ > 10.0) loading_ = false;
  while (true) {
    mpv_event* ev = mpv_wait_event(mpv_, 0);
    if (ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
      case MPV_EVENT_LOG_MESSAGE: {
        auto* msg = static_cast<mpv_event_log_message*>(ev->data);
        std::fprintf(stderr, "[mpv/%s] %s", msg->prefix, redactPaths(msg->text, path_).c_str());
        break;
      }
      case MPV_EVENT_FILE_LOADED:
        fileLoaded_ = true;
        loadedAt_ = nowSeconds();
        break;
      case MPV_EVENT_VIDEO_RECONFIG:
        updateVideoSize();
        if (fileLoaded_) newVideoSized_ = true;
        break;
      case MPV_EVENT_PROPERTY_CHANGE: {
        const auto* prop = static_cast<mpv_event_property*>(ev->data);
        const std::string name = prop->name;
        if (prop->format == MPV_FORMAT_DOUBLE) {
          const double v = *static_cast<double*>(prop->data);
          if (name == "time-pos") playback_.position = resumeAt_ = v;
          else if (name == "duration") playback_.duration = v;
          else if (name == "demuxer-cache-time") playback_.bufferedUntil = v;
          else if (name == "volume") playback_.volume = v;
          else if (name == "speed") playback_.speed = v;
          else if (name == "container-fps") playback_.fps = v;
        } else if (prop->format == MPV_FORMAT_INT64) {
          const int v = static_cast<int>(*static_cast<int64_t*>(prop->data));
          if (name == "audio-params/channel-count") playback_.audioChannels = v;
          else if (name == "chapter") playback_.chapter = v;
        } else if (prop->format == MPV_FORMAT_FLAG) {
          const bool v = *static_cast<int*>(prop->data) != 0;
          if (name == "pause") playback_.paused = v;
          else if (name == "mute") playback_.muted = v;
          else if (name == "eof-reached") playback_.endReached = v;
        } else if (prop->format == MPV_FORMAT_STRING) {
          const char* s = *static_cast<char**>(prop->data);
          if (name == "media-title") playback_.title = s;
          else if (name == "sub-text") playback_.subtitleText = s;
          else if (name == "video-format") playback_.videoCodec = s;
          else if (name == "video-params/pixelformat") playback_.pixelFormat = s;
          else if (name == "audio-codec-name") playback_.audioCodec = s;
          else if (name == "ab-loop-a" || name == "ab-loop-b") {
            char* end = nullptr;
            const double t = std::strtod(s, &end);
            (name == "ab-loop-a" ? playback_.abLoopA : playback_.abLoopB) = end != s ? t : -1;
          }
        } else if (prop->format == MPV_FORMAT_NODE) {
          if (name == "track-list") playback_.tracks = parseTracks(*static_cast<mpv_node*>(prop->data));
          else if (name == "chapter-list")
            playback_.chapters = parseChapters(*static_cast<mpv_node*>(prop->data));
        } else if (prop->format == MPV_FORMAT_NONE) {  // property became unavailable
          if (name == "time-pos") playback_.position = 0;
          else if (name == "duration") playback_.duration = 0;
          else if (name == "demuxer-cache-time") playback_.bufferedUntil = 0;
          else if (name == "track-list") playback_.tracks.clear();
          else if (name == "sub-text") playback_.subtitleText.clear();
          else if (name == "eof-reached") playback_.endReached = false;
          else if (name == "video-format") playback_.videoCodec.clear();
          else if (name == "video-params/pixelformat") playback_.pixelFormat.clear();
          else if (name == "container-fps") playback_.fps = 0;
          else if (name == "audio-codec-name") playback_.audioCodec.clear();
          else if (name == "audio-params/channel-count") playback_.audioChannels = 0;
          else if (name == "chapter-list") playback_.chapters.clear();
          else if (name == "chapter") playback_.chapter = -1;
        }
        break;
      }
      case MPV_EVENT_END_FILE: {
        auto* end = static_cast<mpv_event_end_file*>(ev->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR) {
          std::fprintf(stderr, "Playback failed: %s\n", mpv_error_string(end->error));
          if (path_.find("://") != std::string::npos && retries_ < 3) {
            ++retries_;
            std::fprintf(stderr, "Stream: trying again (%d/3)\n", retries_);
            reloadAt(resumeAt_);
          } else {
            loading_ = false;
          }
        }
        break;
      }
      case MPV_EVENT_SHUTDOWN:
        shutdown_ = true;
        break;
      default:
        break;
    }
  }
}

void VideoPlayer::updateVideoSize() {
  int64_t w = 0, h = 0;
  if (mpv_get_property(mpv_, "dwidth", MPV_FORMAT_INT64, &w) < 0 ||
      mpv_get_property(mpv_, "dheight", MPV_FORMAT_INT64, &h) < 0 || w <= 0 || h <= 0) {
    releaseTarget();
    return;
  }
  if (fbo_ && w == width_ && h == height_) return;
  releaseTarget();

  GLint maxSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
  if (w > maxSize || h > maxSize) {
    const double s = std::min(double(maxSize) / w, double(maxSize) / h);
    w = static_cast<int64_t>(w * s);
    h = static_cast<int64_t>(h * s);
  }
  width_ = static_cast<int>(w);
  height_ = static_cast<int>(h);

  // Mipmaps keep an 8K frame from shimmering when shown smaller than native.
  const int levels = 1 + static_cast<int>(std::floor(std::log2(std::max(width_, height_))));
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA16F, width_, height_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 16.0f);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr, "Video framebuffer incomplete (0x%x)\n", status);
    releaseTarget();
    return;
  }
  framePending_ = true;
  std::printf("Video size: %dx%d\n", width_, height_);
}

void VideoPlayer::releaseTarget() {
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (texture_) glDeleteTextures(1, &texture_);
  fbo_ = 0;
  texture_ = 0;
  width_ = height_ = 0;
}

void VideoPlayer::render() {
  if (!render_) return;
  if (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME) framePending_ = true;
  if (!framePending_ || !fbo_) return;
  framePending_ = false;

  mpv_opengl_fbo fbo{static_cast<int>(fbo_), width_, height_, GL_RGBA16F};
  int flipY = 0;  // texel row 0 = top of the image
  int block = 0;  // never stall the XR frame loop
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flipY},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(render_, params);
  if (newVideoSized_) loading_ = false;  // this was the new video's first frame

  glBindTexture(GL_TEXTURE_2D, texture_);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
}
