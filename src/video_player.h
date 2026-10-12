#pragma once

#include "gl_util.h"

#include <cstdint>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_render_context;

struct Track {
  int64_t id = 0;
  std::string type;  // "audio", "sub" or "video"
  std::string title;
  std::string lang;
  std::string codec;
  bool selected = false;
};

struct Chapter {
  double time = 0;  // seconds
  std::string title;
};

struct PlaybackState {
  double position = 0;  // seconds
  double duration = 0;  // seconds, 0 if unknown
  double bufferedUntil = 0;  // seconds: how far ahead the stream is already downloaded
  bool paused = false;
  double volume = 100;
  bool muted = false;
  std::string title;
  std::vector<Track> tracks;
  std::string subtitleText;  // the subtitle line showing right now, as plain text
  double speed = 1.0;
  double abLoopA = -1, abLoopB = -1;  // A-B loop points in seconds, < 0 = not set
  bool endReached = false;            // stopped at the end of the file
  // What is playing, for the menu's info line.
  std::string videoCodec;   // e.g. "hevc"
  std::string pixelFormat;  // e.g. "p010" (10 bit)
  double fps = 0;
  std::string audioCodec;   // e.g. "opus"
  int audioChannels = 0;
  std::vector<Chapter> chapters;
  int chapter = -1;  // the current one, -1 = before the first / none
};

// Decodes media with libmpv (hardware decoding via NVDEC) and renders each new
// video frame, including mpv's OSD, into an OpenGL texture.
// All methods must be called on the thread that owns the GL context.
class VideoPlayer {
 public:
  ~VideoPlayer();

  bool init(const std::string& hwdec);
  void load(const std::string& path);
  // Stops playback and closes the file (e.g. before deleting it).
  void stop();
  // Runs an mpv input command, e.g. "osd-msg cycle pause".
  void command(const char* cmd);
  void seekTo(double seconds);
  // Loads an external subtitle file and selects it.
  void addSubtitle(const std::string& path);
  // Whether mpv draws subtitles into the video frame.
  void setSubtitlesInFrame(bool visible);
  // Size (1 = normal) and height (percent of the picture above the usual
  // place) of the subtitles mpv draws into the frame.
  void setSubtitleStyle(double scale, int raise);
  // Limits the width of streamed videos (YouTube etc.); 0 = best available.
  // Applies to the next load.
  void setMaxStreamWidth(int width);
  // Loads the current file again, continuing at `seconds`.
  void reloadAt(double seconds);
  // Picture adjustments, -100..100 each (0 = unchanged).
  void setPicture(int brightness, int contrast, int saturation);
  const PlaybackState& playback() const { return playback_; }
  void pollEvents();
  // Renders the next frame into texture() if mpv has one ready.
  void render();

  bool hasVideo() const { return fbo_ != 0; }
  // True from load() until the new file's first frame is in texture(); the
  // texture still holds the previous video meanwhile.
  bool loading() const { return loading_; }
  GLuint texture() const { return texture_; }
  int width() const { return width_; }
  int height() const { return height_; }
  float aspect() const {
    return height_ > 0 ? static_cast<float>(width_) / height_ : 16.0f / 9.0f;
  }
  bool shutdownRequested() const { return shutdown_; }

 private:
  void updateVideoSize();
  void releaseTarget();

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  GLuint texture_ = 0;
  GLuint fbo_ = 0;
  int width_ = 0;
  int height_ = 0;
  PlaybackState playback_;
  std::string path_;  // of the loaded file, for redacting it from log output
  bool subtitlesInFrame_ = true;
  bool loading_ = false;
  bool fileLoaded_ = false;     // the new file has been opened
  bool newVideoSized_ = false;  // ...and its video has been configured
  double loadedAt_ = 0;         // seconds, steady clock
  // Streams: YouTube now and then refuses a fetched link (HTTP 403); then the
  // stream is opened again (yt-dlp fetches fresh links) where it stopped.
  int retries_ = 0;
  double resumeAt_ = 0;  // last known position, kept when the file ends
  bool framePending_ = false;
  bool shutdown_ = false;
};
