#include "file_ops.h"
#include "file_util.h"
#include "frame_analysis.h"
#include "frame_grabber.h"
#include "frame_timer.h"
#include "gl_util.h"
#include "menu.h"
#include "photo_preloader.h"
#include "renderer.h"
#include "settings.h"
#include "stream_format.h"
#include "subtitle_overlay.h"
#include "thumbnailer.h"
#include "video_format.h"
#include "video_memory.h"
#include "video_player.h"
#include "vr_input.h"
#include "vr_space.h"
#include "xmath.h"
#include "xr_context.h"
#include "youtube.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace {

constexpr float kLaserLength = 3.0f;     // meters, when not hitting the panel
constexpr float kSurroundSubtitleDistance = 2.5f;
constexpr float kSurroundSubtitleWidth = 2.0f;
constexpr float kSurroundSubtitleDrop = 0.55f;
constexpr float kTurnStep = kPi / 6;  // 30 degrees per stick flick

struct App {
  VideoPlayer* player = nullptr;
  std::string currentFile;
  ScreenPlacement screen;
  VideoFormat format;
  bool formatChosenByUser = false;  // stops auto-detection from overriding the user
  bool rememberFormat = false;      // the user changed the format by key: store it
  // YouTube videos without a remembered format: the format follows YouTube's
  // metadata, the search filter (kind) and the frame shape.
  bool youTubeAuto = false;
  YouTubeVideo::Kind youTubeKind = YouTubeVideo::Kind::Other;
  VideoSettings video;  // what is remembered about the running video
  LoopMode loop = LoopMode::File;  // repeat the video, unless chosen otherwise
  double sleepAt = 0;   // glfwGetTime() when the sleep timer pauses; 0 = off
  int slideshowSeconds = 0;  // photos: show the next one after this long; 0 = off
  double nextSlideAt = 0;    // glfwGetTime() when it does
  bool recenter = true;
  double recenterAt = 0;  // glfwGetTime() of a recenter counting down; 0 = none
  double volumeAtOpen = -1;  // the volume when the current video started
  bool flipVideo = false;
  bool toggleMenu = false;
  bool seekBackKey = false;     // arrow keys held down
  bool seekForwardKey = false;
  bool turnLeftKey = false;     // A / D held down
  bool turnRightKey = false;
  int streamWidth = 0;          // quality limit of the running stream, 0 = best
  int videoStep = 0;            // +1 / -1: switch to the next / previous video
  // While the menu's search field is open, the keyboard types into it.
  bool textInput = false;
  std::string typed;
  int backspaces = 0;
  bool submit = false;
  bool quit = false;
  double lastEscape = -10;  // glfwGetTime() of the last Esc; a second one soon after quits
};

std::string utf8(unsigned int codepoint) {
  std::string s;
  if (codepoint < 0x80) {
    s += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    s += static_cast<char>(0xC0 | (codepoint >> 6));
    s += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    s += static_cast<char>(0xE0 | (codepoint >> 12));
    s += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (codepoint >> 18));
    s += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return s;
}

void printUsage(const char* argv0) {
  std::printf("Usage: %s [--hwdec=API] [--no-mirror] [--timing] [--xr-test] [--version] [video file or URL]\n"
              "Without a video the file browser opens.\n",
              argv0);
}

void printControls() {
  std::printf(
      "\nControllers:\n"
      "  Trigger        open menu / click in menu / close menu (pointing elsewhere)\n"
      "  Left menu      open or close menu\n"
      "  A              play / pause\n"
      "  B / Y (upper)  recenter\n"
      "  Right stick    left/right: seek (hold to go faster)\n"
      "  Left stick     left/right: turn the view by 30 degrees\n"
      "  Stick up/down  volume, or scroll lists\n"
      "  Grip           hold to move the screen or the menu; with stick up/down: screen size\n"
      "\nKeyboard (the desktop window must have focus):\n"
      "  Tab          open or close menu\n"
      "  Space        play / pause\n"
      "  Left/Right   seek (hold to go faster)\n"
      "  A / D        turn the view left / right\n"
      "  Up/Down      next / previous video in the folder\n"
      "  + / -        volume\n"
      "  M            mute\n"
      "  P            projection: flat / 180 / 360 / fisheye 190 / fisheye 200 / 360 cubemap\n"
      "  L            stereo layout: mono / side-by-side / top-bottom\n"
      "  C            curved / flat screen\n"
      "  W/S          screen closer / farther\n"
      "  Q/E          screen smaller / larger\n"
      "  R            recenter in front of you\n"
      "  F            flip video vertically\n"
      "  Esc Esc      quit (press twice)\n\n");
}

void printFormat(const VideoFormat& f, const char* reason) {
  std::printf("Format (%s): %s, %s\n", reason, projectionName(f.projection), stereoName(f.stereo));
}

// Text from the desktop clipboard, on one line.
std::string clipboardText(GLFWwindow* window) {
  const char* clip = glfwGetClipboardString(window);
  std::string text = clip ? clip : "";
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

void onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
  App& app = *static_cast<App*>(glfwGetWindowUserPointer(window));
  if (app.textInput) {
    // Letters arrive through onChar; only editing keys matter here.
    if (action == GLFW_RELEASE) return;
    if (key == GLFW_KEY_V && (mods & GLFW_MOD_CONTROL)) app.typed += clipboardText(window);
    else if (key == GLFW_KEY_BACKSPACE) ++app.backspaces;
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) app.submit = true;
    else if ((key == GLFW_KEY_ESCAPE || key == GLFW_KEY_TAB) && action == GLFW_PRESS)
      app.toggleMenu = true;
    return;
  }
  // Seeking repeats on its own while the key is held, so track press/release.
  if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) {
    const bool down = action != GLFW_RELEASE;
    (key == GLFW_KEY_LEFT ? app.seekBackKey : app.seekForwardKey) = down;
    return;
  }
  if (key == GLFW_KEY_A || key == GLFW_KEY_D) {
    const bool down = action != GLFW_RELEASE;
    (key == GLFW_KEY_A ? app.turnLeftKey : app.turnRightKey) = down;
    return;
  }
  if (action != GLFW_PRESS) return;

  VideoPlayer& player = *app.player;
  ScreenPlacement& screen = app.screen;
  switch (key) {
    case GLFW_KEY_ESCAPE: {
      // Quitting takes two presses, so a stray Esc doesn't end the video.
      const double now = glfwGetTime();
      if (now - app.lastEscape < 1.5) {
        app.quit = true;
      } else {
        app.lastEscape = now;
        std::printf("Press Esc again to quit\n");
      }
      break;
    }
    case GLFW_KEY_TAB: app.toggleMenu = true; break;
    case GLFW_KEY_SPACE: player.command("cycle pause"); break;
    case GLFW_KEY_UP: app.videoStep = 1; break;
    case GLFW_KEY_DOWN: app.videoStep = -1; break;
    case GLFW_KEY_M: player.command("cycle mute"); break;
    case GLFW_KEY_P:
      app.format.projection = nextProjection(app.format.projection);
      app.rememberFormat = true;
      printFormat(app.format, "manual");
      break;
    case GLFW_KEY_L:
      app.format.stereo = nextStereo(app.format.stereo);
      app.rememberFormat = true;
      printFormat(app.format, "manual");
      break;
    case GLFW_KEY_C: screen.curved = !screen.curved; break;
    case GLFW_KEY_R: app.recenter = true; break;
    case GLFW_KEY_F:
      app.flipVideo = !app.flipVideo;
      std::printf("Video flip: %s\n", app.flipVideo ? "on" : "off");
      break;
    case GLFW_KEY_W: screen.distance = std::max(0.5f, screen.distance - 0.25f); break;
    case GLFW_KEY_S: screen.distance = std::min(20.0f, screen.distance + 0.25f); break;
    case GLFW_KEY_Q: screen.width = std::max(0.5f, screen.width / 1.1f); break;
    case GLFW_KEY_E: screen.width = std::min(30.0f, screen.width * 1.1f); break;
    default: break;
  }
}

// + and - by the typed character, so they work on any keyboard layout and
// on the number pad. Held keys repeat through the system's key repeat.
void onChar(GLFWwindow* window, unsigned int codepoint) {
  App& app = *static_cast<App*>(glfwGetWindowUserPointer(window));
  if (app.textInput) {
    app.typed += utf8(codepoint);
    return;
  }
  if (codepoint == '+') app.player->command("add volume 5");
  else if (codepoint == '-') app.player->command("add volume -5");
}

// Copies the left eye into the desktop window as a preview.
void mirrorToWindow(GLFWwindow* window, GLuint srcFbo, int srcW, int srcH, bool srgb) {
  int winW = 0, winH = 0;
  glfwGetFramebufferSize(window, &winW, &winH);
  if (winW <= 0 || winH <= 0) return;
  const float scale = std::min(float(winW) / srcW, float(winH) / srcH);
  const int w = int(srcW * scale), h = int(srcH * scale);
  const int x = (winW - w) / 2, y = (winH - h) / 2;

  if (srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glViewport(0, 0, winW, winH);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
  glBlitFramebuffer(0, 0, srcW, srcH, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
  glDisable(GL_FRAMEBUFFER_SRGB);
}

bool isStream(const std::string& location) { return location.find("://") != std::string::npos; }

// Where floating subtitles go: on the bottom edge of the flat screen, or for
// surround video below the line of sight, moving with the head's heading
// (in lying mode they stay put with the recentered view instead). The user's
// subtitle size, raise (percent of the picture) and distance apply.
class SubtitlePlacement {
 public:
  Mat4 update(const App& app, float frameAspect, Vec3 headPos, float headYaw,
              const Settings& settings) {
    const ScreenPlacement& s = app.screen;
    const float scale = settings.subtitleSize / 100.0f;
    // 0 is where the user found them best: 30% of the picture lower than the
    // first placement (on the flat screen: below the picture).
    const float raise = (settings.subtitleRaise - 30) / 100.0f;
    if (app.format.projection == Projection::Flat) {
      const bool sbs = app.format.stereo == StereoLayout::SideBySide;
      const bool tb = app.format.stereo == StereoLayout::TopBottom;
      const float eyeAspect = frameAspect * (sbs ? 0.5f : 1.0f) / (tb ? 0.5f : 1.0f);
      const float screenH = s.width / eyeAspect;
      const float w = s.width * 0.8f * scale, h = w / SubtitleOverlay::kAspect;
      const Vec3 local = {0.0f, -screenH / 2 + h / 2 + screenH * (0.04f + raise),
                          -(s.distance - 0.05f)};
      return translation(s.anchorPosition) * s.rotation() * translation(local) * scaling(w, h, 1);
    }
    const float distance = settings.subtitleDistance / 100.0f;
    const float k = distance / kSurroundSubtitleDistance;  // keep the size as seen
    const float w = kSurroundSubtitleWidth * scale * k, h = w / SubtitleOverlay::kAspect;
    const float drop = (kSurroundSubtitleDrop - raise * kSurroundSubtitleDistance) * k;
    if (settings.lyingMode) {
      return translation(headPos) * s.rotation() * translation({0.0f, -drop, -distance}) *
             scaling(w, h, 1);
    }
    // Right along with the head's heading, at once.
    Vec3 center = headPos + forwardFromYaw(headYaw) * distance;
    center.y -= drop;
    return translation(center) * rotationY(headYaw) * scaling(w, h, 1);
  }
};

std::string clockTime(double seconds) {
  const int total = static_cast<int>(std::max(0.0, seconds));
  char buf[32];
  if (total >= 3600)
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
  else
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
  return buf;
}

struct Options {
  std::string file;
  std::string hwdec = "nvdec,nvdec-copy";
  bool mirror = true;
  bool timing = false;
  bool xrTest = false;
};

int run(GLFWwindow* window, const Options& opts) {
  VideoPlayer player;
  if (!player.init(opts.hwdec)) return 1;
  XrContext xr;
  xr.setTestPattern(opts.xrTest);
  if (!xr.init()) return 1;
  SceneRenderer renderer;
  if (!renderer.init()) return 1;
  Menu menu;
  if (!menu.init()) return 1;
  SubtitleOverlay subtitles;
  if (!subtitles.init()) return 1;
  SubtitleOverlay loadingText;  // "Loading …" shown instead of the video while a file opens
  if (!loadingText.init()) return 1;
  // A second mpv for the seek bar preview (optional). It decodes with
  // copy-back, so its frames can be shrunk on the CPU.
  FrameGrabber preview;
  const bool previewReady = preview.init(opts.hwdec == "no" ? "no" : "nvdec-copy", 640, 640);
  SubtitleOverlay previewTime;  // time under the preview frame
  if (!previewTime.init()) return 1;
  SubtitleOverlay countdownText;  // "Recentering in 2 …"
  if (!countdownText.init()) return 1;
  // The preview of 180°/360° video is rendered as a view into this texture.
  struct {
    GLuint texture = 0, fbo = 0;
    int width = 480, height = 270;
  } previewView;
  glGenTextures(1, &previewView.texture);
  glBindTexture(GL_TEXTURE_2D, previewView.texture);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, previewView.width, previewView.height);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &previewView.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, previewView.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previewView.texture, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  // Glass menus: a mipmapped copy of the eye image behind them, for the blur.
  struct GlassBackdrop {
    GLuint texture = 0, fbo = 0;
    int width = 0, height = 0;
    void resize(int w, int h) {
      release();
      width = w;
      height = h;
      const int levels = 1 + static_cast<int>(std::floor(std::log2(std::max(w, h))));
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_2D, 0);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    void release() {
      if (fbo) glDeleteFramebuffers(1, &fbo);
      if (texture) glDeleteTextures(1, &texture);
      fbo = texture = 0;
    }
  } glassBackdrop;
  // Shown while the preview's first frame is still loading.
  CairoCanvas previewPlaceholder;
  if (!previewPlaceholder.init(320, 180)) return 1;
  cairo_set_source_rgba(previewPlaceholder.cr(), 0.12, 0.12, 0.15, 1.0);
  cairo_paint(previewPlaceholder.cr());
  previewPlaceholder.upload();
  Thumbnailer thumbnailer;
  thumbnailer.start();
  menu.setThumbnailer(&thumbnailer);
  PhotoPreloader photoPreloader;  // the next photo, decoded ahead
  photoPreloader.start();
  VideoMemory memory;
  memory.load();
  Settings settings = loadSettings();
  player.setMaxStreamWidth(settings.maxVideoWidth);
  player.setSubtitleStyle(settings.subtitleSize / 100.0, settings.subtitleRaise);
  player.command("set loop-file inf");  // App::loop starts as "repeat the video"
  FolderMemory folders;
  folders.load();
  menu.setFolderMemory(&folders);
  {
    const std::string version = ytDlpVersion();
    std::printf("yt-dlp: %s\n", version.empty() ? "not found (YouTube will not work)" : version.c_str());
  }

  App app;
  app.player = &player;
  app.streamWidth = settings.maxVideoWidth;
  glfwSetWindowUserPointer(window, &app);
  glfwSetKeyCallback(window, onKey);
  glfwSetCharCallback(window, onChar);
  printControls();

  // A video being deleted in the background.
  struct Removal {
    std::future<std::pair<RemoveResult, std::string>> result;
    std::string path;
    bool permanent = false;
    bool wasPlaying = false;
    std::string next;  // what to play afterwards, if it was playing
  } removal;
  // Whether a video's folder has a trash, asked in the background before the
  // delete dialog opens (slow on network shares).
  struct TrashCheck {
    std::future<bool> result;
    std::string path;
  } trashCheck;
  // YouTube's format list for the playing video, fetched in the background.
  struct FormatProbe {
    std::future<std::vector<StreamFormatInfo>> result;
    std::string url;
    std::vector<StreamFormatInfo> formats;
    std::vector<std::pair<int, int>> sizes;  // for the quality menu
  } probe;
  // Holding the grip moves the screen or the menu with the hand.
  struct Grab {
    int hand = -1;  // -1 = nothing grabbed
    bool menu = false;
    Mat4 startInverse;  // inverse of the hand pose at the grab
    Vec3 startPoint;    // centre of the grabbed thing at the grab
    Vec3 head;          // screen: head position at the grab, the new anchor
  } grab;
  double previewTarget = -1;  // seconds the preview frame was asked for
  bool menuFollowing = false;  // the menu glides back in front of the viewer
  bool menuHeightKept = false;  // moved by hand: it follows at this height above the eyes
  float menuHeight = 0.0f;
  // YouTube videos: what a look at the picture said, and when to look (0 = not planned).
  FrameAnalysis picture;
  bool photoSized = false;  // the current photo's size is known (its format guessed)
  double analysisAt = 0;
  int analysisTries = 0;
  // The seek preview of a stream may come in another layout than the video
  // (a smaller format); it gets its own look.
  VideoFormat previewFormat;
  bool previewFormatKnown = false;
  bool wasAtEnd = false;
  double lastFrameTime = glfwGetTime();

  MenuPanels panel;
  SubtitlePlacement subtitlePlacement;
  // Without a video, start in the file browser as soon as the head is tracked.
  bool showBrowserAtStart = opts.file.empty();
  int activeHand = 1;  // the hand whose pointer drives the menu
  int knownWidth = 0, knownHeight = 0;
  HoldRepeat seekBack(0.4, 0.2), seekForward(0.4, 0.2);
  HoldRepeat volumeUp(0.4, 0.15), volumeDown(0.4, 0.15);
  HoldRepeat turnLeft(0.5, 0.35), turnRight(0.5, 0.35);

  // Remembers the volume of the video being left, if it was changed while it played.
  auto rememberVolume = [&] {
    if (app.currentFile.empty() || isPhotoFile(app.currentFile) || app.volumeAtOpen < 0) return;
    const double volume = player.playback().volume;
    if (std::abs(volume - app.volumeAtOpen) < 0.5) return;
    app.video.volume = static_cast<int>(std::lround(volume));
    memory.set(app.currentFile, app.video);
  };
  // Opens a video with what is remembered about it. True if its format was
  // remembered.
  auto openVideo = [&](const std::string& path) {
    rememberVolume();
    app.currentFile = path;
    app.formatChosenByUser = false;
    app.youTubeAuto = false;
    app.format = guessVideoFormat(path, 0, 0);
    app.video = memory.get(path);
    if (app.video.hasFormat) {
      app.format = app.video.format;
      app.formatChosenByUser = true;
      printFormat(app.format, "remembered");
    }
    app.screen.zoom = app.video.zoom;
    player.setPicture(app.video.brightness, app.video.contrast, app.video.saturation);
    if (app.video.volume >= 0)
      player.command(("set volume " + std::to_string(app.video.volume)).c_str());
    app.volumeAtOpen = app.video.volume >= 0 ? app.video.volume : player.playback().volume;
    player.command("set ab-loop-a no");
    player.command("set ab-loop-b no");
    knownWidth = knownHeight = 0;  // re-detect the format once the size is known
    // Every new stream starts at the default quality; the dropdown only
    // changes the running one.
    app.streamWidth = settings.maxVideoWidth;
    player.setMaxStreamWidth(app.streamWidth);
    if (isPhotoFile(path)) {
      // Photos are shown by photoPreloader, not mpv: decoded in the
      // background, the next one (after the last the first, as the slideshow
      // goes) ahead, so switching shows it at once.
      player.stop();
      std::string next = siblingMedia(path, 1);
      if (next.empty()) next = firstMedia(path);
      photoPreloader.request(path, next);
      photoSized = false;
    } else {
      player.load(path);
    }
    // Streams take seconds to open; prepare the seek bar preview right away.
    // Local files open fast enough when the pointer first reaches the bar.
    if (previewReady && isStream(path))
      preview.open(path, "0");
    else
      preview.close();
    previewTarget = 0;
    previewFormatKnown = false;
    app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;  // each photo gets its full time
    picture = FrameAnalysis();
    analysisAt = 0;
    return app.video.hasFormat;
  };
  // Stores the current format for this video (after the user chose it).
  auto rememberFormat = [&] {
    app.formatChosenByUser = true;
    app.youTubeAuto = false;
    app.video.hasFormat = true;
    app.video.format = app.format;
    memory.set(app.currentFile, app.video);
  };
  auto applyPicture = [&] {
    player.setPicture(app.video.brightness, app.video.contrast, app.video.saturation);
    app.screen.zoom = app.video.zoom;
    memory.set(app.currentFile, app.video);
  };
  if (!opts.file.empty()) openVideo(opts.file);
  // Set below: runs a menu command (needed here for YouTube playlists).
  std::function<void(const MenuCommand&)> runCommand;
  auto stepVideo = [&](int step) {
    if (app.currentFile.empty()) return;
    if (isStream(app.currentFile)) {  // within the listed playlist, if any
      const MenuCommand c = menu.playlistNeighbor(app.currentFile, step, false);
      if (c.type != MenuCommand::None) {
        runCommand(c);
        app.recenter = false;  // keep the view, like for local files
      }
      return;
    }
    const std::string next = siblingMedia(app.currentFile, step);
    if (next.empty())
      std::printf("No %s video in this folder\n", step > 0 ? "next" : "previous");
    else
      openVideo(next);
  };

  auto execute = [&](const MenuCommand& c) {
    switch (c.type) {
      case MenuCommand::TogglePause: player.command("cycle pause"); break;
      case MenuCommand::Seek: player.command(("seek " + std::to_string(c.value)).c_str()); break;
      case MenuCommand::SeekTo: player.seekTo(c.value); break;
      case MenuCommand::ToggleMute: player.command("cycle mute"); break;
      case MenuCommand::SetVolume:
        player.command(("set volume " + std::to_string(static_cast<int>(c.value))).c_str());
        break;
      case MenuCommand::PreviousVideo: stepVideo(-1); break;
      case MenuCommand::NextVideo: stepVideo(1); break;
      case MenuCommand::SetProjection:
        app.format.projection = static_cast<Projection>(static_cast<int>(c.value));
        rememberFormat();
        printFormat(app.format, "manual");
        break;
      case MenuCommand::SetStereo:
        app.format.stereo = static_cast<StereoLayout>(static_cast<int>(c.value));
        rememberFormat();
        printFormat(app.format, "manual");
        break;
      case MenuCommand::SetSpeed:
        player.command(("set speed " + std::to_string(c.value)).c_str());
        break;
      case MenuCommand::SetLoop:
        app.loop = static_cast<LoopMode>(static_cast<int>(c.value));
        player.command(app.loop == LoopMode::File ? "set loop-file inf" : "set loop-file no");
        break;
      case MenuCommand::ToggleAbLoop: player.command("ab-loop"); break;
      case MenuCommand::SetSleepTimer:
        app.sleepAt = c.value > 0 ? glfwGetTime() + c.value * 60 : 0;
        break;
      case MenuCommand::SetBrightness:
        app.video.brightness = static_cast<int>(c.value);
        applyPicture();
        break;
      case MenuCommand::SetContrast:
        app.video.contrast = static_cast<int>(c.value);
        applyPicture();
        break;
      case MenuCommand::SetSaturation:
        app.video.saturation = static_cast<int>(c.value);
        applyPicture();
        break;
      case MenuCommand::SetZoom:
        app.video.zoom = static_cast<float>(c.value / 100.0);
        applyPicture();
        break;
      case MenuCommand::ResetPicture:
        app.video.brightness = app.video.contrast = app.video.saturation = 0;
        app.video.zoom = 1.0f;
        applyPicture();
        break;
      case MenuCommand::Paste: menu.typeText(clipboardText(window)); break;
      case MenuCommand::SetSlideshow:
        app.slideshowSeconds = static_cast<int>(c.value);
        app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;
        break;
      case MenuCommand::SetTheme:
        settings.theme = static_cast<Theme>(static_cast<int>(c.value));
        saveSettings(settings);
        break;
      case MenuCommand::ToggleCurved: app.screen.curved = !app.screen.curved; break;
      case MenuCommand::Recenter:
        // From the menu: two seconds to look where "front" should be.
        app.recenterAt = glfwGetTime() + 2.0;
        break;
      case MenuCommand::ToggleBackground:
        settings.background = !settings.background;
        saveSettings(settings);
        break;
      case MenuCommand::ToggleMenuFollows:
        settings.menuFollows = !settings.menuFollows;
        saveSettings(settings);
        break;
      case MenuCommand::SetSubtitleSize:
      case MenuCommand::SetSubtitleRaise:
      case MenuCommand::SetSubtitleDistance:
        (c.type == MenuCommand::SetSubtitleSize    ? settings.subtitleSize
         : c.type == MenuCommand::SetSubtitleRaise ? settings.subtitleRaise
                                                   : settings.subtitleDistance) =
            static_cast<int>(c.value);
        saveSettings(settings);
        player.setSubtitleStyle(settings.subtitleSize / 100.0, settings.subtitleRaise);
        break;
      case MenuCommand::Quit: app.quit = true; break;
      case MenuCommand::OpenFile:
        openVideo(c.path);
        panel.open = false;
        break;
      case MenuCommand::SelectAudio:
        player.command(("set aid " + std::to_string(static_cast<int64_t>(c.value))).c_str());
        break;
      case MenuCommand::SelectSubtitle:
        player.command(c.value > 0
                           ? ("set sid " + std::to_string(static_cast<int64_t>(c.value))).c_str()
                           : "set sid no");
        break;
      case MenuCommand::LoadSubtitle: player.addSubtitle(c.path); break;
      case MenuCommand::SetLanguage:
        settings.language = static_cast<Language>(static_cast<int>(c.value));
        saveSettings(settings);
        break;
      case MenuCommand::ToggleFloatingSubtitles:
        settings.floatingSubtitles = !settings.floatingSubtitles;
        saveSettings(settings);
        break;
      case MenuCommand::SetQuality:  // default for streams started from now on
        settings.maxVideoWidth = static_cast<int>(c.value);
        saveSettings(settings);
        break;
      case MenuCommand::SetStreamQuality:  // the running stream only
        app.streamWidth = static_cast<int>(c.value);
        player.setMaxStreamWidth(app.streamWidth);
        // Streams pick their quality when loading, so reload at the same spot.
        if (isStream(app.currentFile)) player.reloadAt(player.playback().position);
        break;
      case MenuCommand::ToggleLyingMode:
        settings.lyingMode = !settings.lyingMode;
        saveSettings(settings);
        app.recenter = true;  // apply right away
        break;
      case MenuCommand::RequestDelete:
        if (trashCheck.result.valid()) break;  // already asking
        trashCheck.path = c.path;
        trashCheck.result = std::async(std::launch::async, [path = c.path] { return canTrash(path); });
        break;
      case MenuCommand::DeleteFile: {
        if (removal.result.valid()) break;  // one deletion at a time
        removal.path = c.path;
        removal.permanent = c.value > 0;
        removal.wasPlaying = c.path == app.currentFile;
        removal.next.clear();
        preview.close();  // it may have the file open too
        if (removal.wasPlaying) {
          // Pick what plays next while the file is still listed, then close it:
          // some network shares refuse to delete open files.
          removal.next = siblingMedia(c.path, 1);
          if (removal.next.empty()) removal.next = siblingMedia(c.path, -1);
          player.stop();
          app.currentFile.clear();
        }
        // In the background: a slow network share must not freeze the headset.
        removal.result = std::async(std::launch::async, [path = c.path, permanent = removal.permanent] {
          std::string error;
          const RemoveResult result = removeFile(path, permanent, error);
          return std::make_pair(result, error);
        });
        break;
      }
      case MenuCommand::PlayYouTube: {
        // The search filter already says what kind of VR video it is.
        const auto kind = static_cast<YouTubeVideo::Kind>(static_cast<int>(c.value));
        if (!openVideo(c.path)) {
          app.youTubeAuto = true;
          app.youTubeKind = kind;
          std::string source;
          app.format = youTubeFormat(kind, {}, c.path, 0, 0, nullptr, source);
          app.formatChosenByUser = true;  // the YouTube rules decide, not the file name
        }
        // Ask YouTube for its format list; it may name the 360° projection.
        probe.url = c.path;
        probe.formats.clear();
        probe.sizes.clear();
        probe.result = std::async(std::launch::async, [url = c.path] { return youTubeFormats(url); });
        app.recenter = true;
        panel.open = false;
        break;
      }
      case MenuCommand::None: break;
    }
  };
  runCommand = execute;

  bool xrQuit = false;
  FrameTimer timer(opts.timing);
  while (!xrQuit && !app.quit && !glfwWindowShouldClose(window)) {
    timer.start();
    glfwPollEvents();
    xr.pollEvents(xrQuit);
    player.pollEvents();
    timer.lap(FrameTimer::Events);
    if (player.shutdownRequested()) break;

    if (app.videoStep != 0) {
      stepVideo(app.videoStep);
      app.videoStep = 0;
    }

    // End of the video: with "repeat folder", go on with the next one, after
    // the last one with the first.
    // For YouTube, "folder" means the listed playlist.
    const bool atEnd = player.playback().endReached;
    if (atEnd && !wasAtEnd && app.loop == LoopMode::Folder && !app.currentFile.empty()) {
      if (isStream(app.currentFile)) {
        const MenuCommand next = menu.playlistNeighbor(app.currentFile, 1, true);
        if (next.type != MenuCommand::None) {
          execute(next);
          app.recenter = false;  // keep the view
        }
      } else {
        std::string next = siblingMedia(app.currentFile, 1);
        if (next.empty()) next = firstMedia(app.currentFile);
        if (!next.empty()) openVideo(next);
      }
    }
    wasAtEnd = atEnd;
    // A single stream has no folder to repeat.
    if (app.loop == LoopMode::Folder && isStream(app.currentFile) &&
        !menu.inPlaylist(app.currentFile))
      app.loop = LoopMode::Off;
    // Slideshow: the next photo in the folder, after the last one the first.
    if (app.slideshowSeconds > 0 && isPhotoFile(app.currentFile) &&
        glfwGetTime() >= app.nextSlideAt) {
      std::string next = siblingMedia(app.currentFile, 1);
      if (next.empty()) next = firstMedia(app.currentFile);
      if (!next.empty()) openVideo(next);
      app.nextSlideAt = glfwGetTime() + app.slideshowSeconds;
    }
    if (app.sleepAt > 0 && glfwGetTime() >= app.sleepAt) {
      player.command("set pause yes");
      app.sleepAt = 0;
      std::printf("Sleep timer: paused\n");
    }

    // A deletion running in the background has finished.
    if (removal.result.valid() &&
        removal.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      const auto [result, error] = removal.result.get();
      if (result == RemoveResult::Failed) {
        if (removal.wasPlaying) openVideo(removal.path);  // it is still there
        if (removal.permanent)
          menu.showDeleteError(error);
        else
          menu.askDeleteAfterTrashFailed(removal.path);
      } else {
        std::printf("Video %s\n", result == RemoveResult::Trashed ? "moved to the trash" : "deleted");
        forgetFolderListing();
        menu.refreshDirectory();
        if (removal.wasPlaying) {
          if (!removal.next.empty())
            openVideo(removal.next);
          else
            showBrowserAtStart = true;  // nothing left in the folder: pick something new
        }
      }
    }

    // Background answers: trash available, YouTube's format list.
    if (trashCheck.result.valid() &&
        trashCheck.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      menu.askDelete(trashCheck.path, trashCheck.result.get());
    if (probe.result.valid() &&
        probe.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      probe.formats = probe.result.get();
      // The sizes to offer in the quality menu: from 1080p up, each width
      // once, largest first.
      probe.sizes.clear();
      for (const StreamFormatInfo& f : probe.formats)
        if (f.width >= 1920 && f.height >= 1080 &&
            std::none_of(probe.sizes.begin(), probe.sizes.end(),
                         [&](const std::pair<int, int>& s) { return s.first == f.width; }))
          probe.sizes.emplace_back(f.width, f.height);
      std::sort(probe.sizes.begin(), probe.sizes.end(), std::greater<>());
      if (probe.url == app.currentFile) knownWidth = -1;  // decide the format again
    }

    // Re-guess the format once the real frame size is known.
    if (app.rememberFormat) {  // changed by key
      rememberFormat();
      app.rememberFormat = false;
    }
    if (player.width() != knownWidth || player.height() != knownHeight) {
      knownWidth = player.width();
      knownHeight = player.height();
      if (knownWidth > 0 && app.youTubeAuto) {
        std::string source;
        app.format = youTubeFormat(app.youTubeKind,
                                   probe.url == app.currentFile ? probe.formats
                                                                : std::vector<StreamFormatInfo>{},
                                   app.currentFile, knownWidth, knownHeight, &picture, source);
        std::printf("Format source: %s\n", source.c_str());
        // Look at the picture once it shows (unless that already happened).
        if (!picture.valid && analysisAt == 0) {
          analysisAt = glfwGetTime() + 1.0;
          analysisTries = 0;
        }
        printFormat(app.format, "detected");
      } else if (knownWidth > 0 && !app.formatChosenByUser) {
        app.format = guessVideoFormat(app.currentFile, knownWidth, knownHeight);
        printFormat(app.format, "detected");
      }
    }

    // A second after a YouTube video shows, look at its picture to tell 3D
    // and cubemap apart; a dark or flat frame (a fade-in) is looked at later.
    if (analysisAt > 0 && glfwGetTime() >= analysisAt && app.youTubeAuto && !player.loading() &&
        player.texture()) {
      const int level =
          std::max(0, static_cast<int>(std::floor(std::log2(player.width() / 480.0))));
      picture = analyzeFrame(player.texture(), player.width(), player.height(), level);
      std::printf("Picture: halves left/right %.2f, top/bottom %.2f, seam %.1f%s\n",
                  picture.sideBySide, picture.topBottom, picture.seam,
                  picture.valid ? "" : " (too dark, again later)");
      if (picture.valid) {
        analysisAt = 0;
        std::string source;
        app.format = youTubeFormat(app.youTubeKind,
                                   probe.url == app.currentFile ? probe.formats
                                                                : std::vector<StreamFormatInfo>{},
                                   app.currentFile, player.width(), player.height(), &picture,
                                   source);
        std::printf("Format source: %s\n", source.c_str());
        printFormat(app.format, "detected");
      } else {
        analysisAt = ++analysisTries < 6 ? glfwGetTime() + 2.0 : 0;
      }
    }

    // 3D and surround video always get floating subtitles (mpv's would be cut
    // in half or end up behind you); for 2D video the setting decides.
    const bool floatingSubtitles =
        settings.floatingSubtitles ||
        app.format.stereo != StereoLayout::Mono || app.format.projection != Projection::Flat;
    player.setSubtitlesInFrame(!floatingSubtitles);
    subtitles.setText(floatingSubtitles ? player.playback().subtitleText : "");
    loadingText.setText(strings(settings.language).loading);
    // Until the new file's first frame arrives, the texture still holds the old
    // video, which would show cut apart in the new format; show nothing instead.
    const bool photo = isPhotoFile(app.currentFile);
    const bool loading = !photo && player.loading() && !app.currentFile.empty();
    GLuint videoTexture = loading ? 0 : player.texture();
    float frameAspect = player.aspect();
    int photoWidth = 0, photoHeight = 0;
    photoPreloader.update();
    if (photo) {
      videoTexture = photoPreloader.texture(app.currentFile, photoWidth, photoHeight);
      if (videoTexture) {
        frameAspect = static_cast<float>(photoWidth) / photoHeight;
        if (!photoSized) {  // the size is known now: guess the format
          photoSized = true;
          std::printf("Photo size: %dx%d\n", photoWidth, photoHeight);
          if (!app.formatChosenByUser) {
            app.format = guessVideoFormat(app.currentFile, photoWidth, photoHeight);
            printFormat(app.format, "detected");
          }
        }
      }
    }
    renderer.setBackground(settings.background);
    // Photos: brightness etc. are applied while drawing (mpv does it for videos).
    renderer.setPictureAdjustment(photo ? app.video.brightness : 0, photo ? app.video.contrast : 0,
                                  photo ? app.video.saturation : 0);
    const bool stillLoading = !app.currentFile.empty() && videoTexture == 0 && (photo || loading);

    if (!xr.isRunning()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    const bool shouldRender = xr.beginFrame();
    timer.lap(FrameTimer::XrWait);
    player.render();
    if (previewReady) preview.update();
    timer.lap(FrameTimer::Mpv);

    if (shouldRender) {
      const double now = glfwGetTime();
      Vec3 headPos;
      Quat headRot;
      const bool headValid = xr.headPose(headPos, headRot);
      const Mat4 frame = headFrame(headRot, settings.lyingMode);
      if (app.recenterAt > 0 && now >= app.recenterAt) {  // the menu's countdown is over
        app.recenter = true;
        app.recenterAt = 0;
      }
      if (app.recenter && headValid) {
        app.screen.anchorPosition = headPos;
        app.screen.anchorRotation = frame;
        app.screen.turn = 0.0f;
        if (panel.open) panel.placeInFrontOf(headPos, frame);  // the menu comes along
        menuHeightKept = false;
        app.recenter = false;
      }

      // --- Controller and keyboard input ---
      const ControllerState& input = xr.controllers();
      for (int h = 0; h < 2; ++h)
        if (input.hands[h].selectPressed) activeHand = h;
      const bool triggerPressed = input.hands[0].selectPressed || input.hands[1].selectPressed;

      bool openedNow = false;
      if (showBrowserAtStart && headValid) {
        showBrowserAtStart = false;
        panel.open = true;
        panel.placeInFrontOf(headPos, frame);
        menu.showFileBrowser();
      } else if (input.menuPressed || app.toggleMenu || (!panel.open && triggerPressed)) {
        panel.open = !panel.open;
        openedNow = panel.open;
        if (panel.open) {
          // Always open on the player page; sub pages (search, files) keep their state.
          menu.showMainPage();
          if (headValid) panel.placeInFrontOf(headPos, frame);
          menuHeightKept = false;
        }
      }
      app.toggleMenu = false;
      if (input.playPausePressed) player.command("cycle pause");
      if (input.recenterPressed) app.recenter = true;

      // Grip: grabs the menu if the hand points at it, else the flat screen.
      // The grabbed thing follows the hand and keeps facing the viewer.
      const float dt = static_cast<float>(std::min(0.1, now - lastFrameTime));
      lastFrameTime = now;
      const Vec3 up = transformDir(frame, {0.0f, 1.0f, 0.0f});
      if (grab.hand >= 0 && !input.hands[grab.hand].grip) grab.hand = -1;
      for (int h = 0; h < 2 && grab.hand < 0 && headValid; ++h) {
        const HandState& g = input.hands[h];
        if (!g.gripPressed || !g.poseValid) continue;
        const Vec3 dir = normalize(transformDir(rotation(g.aimOrientation), {0, 0, -1}));
        float u, v, d;
        const bool onMenu =
            panel.open && (panel.main(menu.mainVisibleHeight()).hit(g.aimPosition, dir, u, v, d) ||
                           (menu.formatPanelOpen() && panel.format().hit(g.aimPosition, dir, u, v, d)));
        if (onMenu) {
          grab.menu = true;
          grab.startPoint = panel.anchor;
        } else if (app.format.projection == Projection::Flat && player.hasVideo()) {
          grab.menu = false;
          grab.startPoint =
              transformPoint(translation(app.screen.anchorPosition) * app.screen.rotation(),
                             {0.0f, 0.0f, -app.screen.distance});
          // Keep the screen's own centre of rotation, so grabbing it does not tilt it.
          grab.head = app.screen.anchorPosition;
        } else {
          continue;
        }
        grab.hand = h;
        grab.startInverse = poseInverse(g.aimPosition, g.aimOrientation);
      }
      if (grab.hand >= 0 && input.hands[grab.hand].poseValid && headValid) {
        const HandState& g = input.hands[grab.hand];
        const Vec3 point = transformPoint(
            poseMatrix(g.aimPosition, g.aimOrientation) * grab.startInverse, grab.startPoint);
        Mat4 basis;
        if (grab.menu) {
          if (facingBasis(normalize(headPos - point), up, basis)) {
            panel.anchor = point;
            panel.orientation = basis;
          }
          // Moved by hand: following the gaze keeps this height (relative to the eyes).
          menuHeightKept = true;
          menuHeight = dot(panel.anchor - headPos, up);
        } else {
          const Vec3 offset = point - grab.head;
          const float distance = length(offset);
          if (distance > 0.3f && facingBasis(offset * (-1.0f / distance), up, basis)) {
            app.screen.anchorPosition = grab.head;
            app.screen.anchorRotation = basis;
            app.screen.turn = 0.0f;
            app.screen.distance = std::clamp(distance, 0.5f, 20.0f);
          }
          // Stick up/down on the holding hand: screen size.
          if (std::abs(g.stickY) > 0.2f)
            app.screen.width =
                std::clamp(app.screen.width * std::exp(g.stickY * dt * 0.8f), 0.5f, 30.0f);
        }
      }

      // The menu comes along when you turn far away from it (unless switched
      // off or held), gliding back in front of you.
      // Not while a pointer is on it: then it is in use.
      bool pointedAt = false;
      for (const HandState& h : input.hands) {
        if (!h.poseValid || !panel.open) continue;
        const Vec3 dir = normalize(transformDir(rotation(h.aimOrientation), {0, 0, -1}));
        float u, v, d;
        pointedAt = pointedAt || panel.main(menu.mainVisibleHeight()).hit(h.aimPosition, dir, u, v, d) ||
                    (menu.formatPanelOpen() && panel.format().hit(h.aimPosition, dir, u, v, d));
      }
      if (settings.menuFollows && panel.open && grab.hand < 0 && headValid && !pointedAt) {
        // Sideways only: the menu sits below eye level, looking down at it is fine.
        auto flat = [&](Vec3 v) { return normalize(v - up * dot(v, up)); };
        const Vec3 toPanel = flat(panel.anchor - headPos);
        const Vec3 gaze = flat(transformDir(rotation(headRot), {0.0f, 0.0f, -1.0f}));
        if (dot(toPanel, gaze) < 0.866f) menuFollowing = true;  // more than 30° away
        if (menuFollowing) {
          MenuPanels target = panel;
          target.placeInFrontOf(headPos, frame);
          if (menuHeightKept)
            target.anchor = target.anchor + up * (menuHeight - dot(target.anchor - headPos, up));
          panel.anchor = panel.anchor + (target.anchor - panel.anchor) * 0.08f;
          Mat4 basis;
          if (facingBasis(normalize(headPos - panel.anchor), up, basis)) panel.orientation = basis;
          if (length(target.anchor - panel.anchor) < 0.02f) menuFollowing = false;
        }
      } else {
        menuFollowing = false;
      }

      // Left stick sideways turns the view, right stick sideways seeks; up/down
      // on either changes the volume or scrolls lists. Only the dominant axis counts.
      StickDirections left = stickDirections(input.hands[0]);
      StickDirections right = stickDirections(input.hands[1]);
      if (grab.hand == 0) left.up = left.down = false;  // that stick sizes the screen
      if (grab.hand == 1) right.up = right.down = false;
      const bool stickUp = left.up || right.up;
      const bool stickDown = left.down || right.down;
      const bool listMode = panel.open && menu.listOpen();

      if (seekBack.update(right.left || app.seekBackKey, now))
        player.command(("seek -" + std::to_string(seekStep(seekBack.heldSeconds(now)))).c_str());
      if (seekForward.update(right.right || app.seekForwardKey, now))
        player.command(("seek " + std::to_string(seekStep(seekForward.heldSeconds(now)))).c_str());
      if (turnLeft.update(left.left || app.turnLeftKey, now))
        app.screen.turn = wrapAngle(app.screen.turn - kTurnStep);
      if (turnRight.update(left.right || app.turnRightKey, now))
        app.screen.turn = wrapAngle(app.screen.turn + kTurnStep);
      if (volumeUp.update(!listMode && stickUp, now)) player.command("add volume 5");
      if (volumeDown.update(!listMode && stickDown, now)) player.command("add volume -5");
      if (listMode) {
        // Lists glide: the further the stick is tilted, the faster (up to
        // ~14 rows a second), with a small dead zone around the middle.
        float tilt = 0.0f;
        for (int i = 0; i < 2; ++i) {
          const HandState& h = input.hands[i];
          if (grab.hand == i || std::abs(h.stickY) <= std::abs(h.stickX)) continue;
          if (std::abs(h.stickY) > std::abs(tilt)) tilt = h.stickY;
        }
        const float amount = std::max(0.0f, (std::abs(tilt) - 0.15f) / 0.85f);
        if (amount > 0) menu.scroll((tilt > 0 ? -1.0 : 1.0) * amount * amount * 14.0 * dt);
      }

      // Pointer ray against the menu panels; the nearer hit wins.
      const HandState& hand = input.hands[activeHand];
      Vec3 rayOrigin, rayDir, hitPoint;
      float hitU = 0, hitV = 0;
      int hitPanelId = -1;
      if (panel.open && hand.poseValid) {
        rayOrigin = hand.aimPosition;
        rayDir = normalize(transformDir(rotation(hand.aimOrientation), {0, 0, -1}));
        float u, v, distance, nearest = 1e9f;
        if (panel.main(menu.mainVisibleHeight()).hit(rayOrigin, rayDir, u, v, distance)) {
          nearest = distance;
          hitPanelId = Menu::kMain;
          hitU = u;
          hitV = v;
        }
        if (menu.formatPanelOpen() && panel.format().hit(rayOrigin, rayDir, u, v, distance) &&
            distance < nearest) {
          nearest = distance;
          hitPanelId = Menu::kFormat;
          hitU = u;
          hitV = v;
        }
        if (hitPanelId >= 0) hitPoint = rayOrigin + rayDir * nearest;
      }
      const bool hitPanel = hitPanelId >= 0;

      MenuView view;
      view.playback = player.playback();
      view.format = app.format;
      view.curvedScreen = app.screen.curved;
      view.currentFile = app.currentFile;
      view.language = settings.language;
      view.theme = settings.theme;
      view.floatingSubtitles = settings.floatingSubtitles;
      view.maxVideoWidth = settings.maxVideoWidth;
      view.streaming = isStream(app.currentFile);
      view.streamWidth = app.streamWidth;
      if (probe.url == app.currentFile) view.streamSizes = probe.sizes;
      view.videoWidth = photo ? photoWidth : player.width();
      view.videoHeight = photo ? photoHeight : player.height();
      if (photo) view.playback.title = std::filesystem::path(app.currentFile).filename().string();
      view.lyingMode = settings.lyingMode;
      view.background = settings.background;
      view.menuFollows = settings.menuFollows;
      view.subtitleSize = settings.subtitleSize;
      view.subtitleRaise = settings.subtitleRaise;
      view.subtitleDistance = settings.subtitleDistance;
      view.loop = app.loop;
      view.photo = !app.currentFile.empty() && isPhotoFile(app.currentFile);
      view.slideshowSeconds = app.slideshowSeconds;
      view.sleepMinutesLeft =
          app.sleepAt > 0 ? static_cast<int>(std::ceil((app.sleepAt - now) / 60.0)) : 0;
      view.brightness = app.video.brightness;
      view.contrast = app.video.contrast;
      view.saturation = app.video.saturation;
      view.zoom = app.video.zoom;
      menu.update(view);
      menu.setPointer(hitPanelId, hitU, hitV);
      app.textInput = panel.open && menu.textInputActive();
      menu.typeText(app.typed);
      for (; app.backspaces > 0; --app.backspaces) menu.backspace();
      if (app.submit) execute(menu.submitText());
      app.typed.clear();
      app.submit = false;
      // Trigger on the panel clicks it; anywhere else closes the menu.
      if (panel.open && hand.selectPressed && !openedNow) {
        if (hitPanel)
          execute(menu.click());
        else
          panel.open = false;
      }
      // Sliders follow the pointer while the trigger stays down.
      execute(menu.drag(panel.open && hand.select));

      // Seek bar preview: a frame of the video where the pointer is.
      double hoverFraction = 0;
      const bool previewing = previewReady && panel.open && !loading &&
                              !app.currentFile.empty() && menu.seekHover(hoverFraction);
      if (previewing) {
        const double target = hoverFraction * player.playback().duration;
        if (preview.path() != app.currentFile) {
          preview.open(app.currentFile, std::to_string(target));
          previewTarget = target;
        } else if (std::abs(target - previewTarget) > 0.5) {
          preview.seek(target);
          previewTarget = target;
        }
        // One eye of a 3D video.
        const bool sbs = app.format.stereo == StereoLayout::SideBySide;
        const bool tb = app.format.stereo == StereoLayout::TopBottom;
        preview.setCrop(0, 0, sbs ? 0.5f : 1.0f, tb ? 0.5f : 1.0f);
        // Until the first frame arrives, a placeholder says it is loading.
        previewTime.setText(preview.hasFrame() ? clockTime(target)
                                               : clockTime(target) + "  ·  " +
                                                     strings(settings.language).loading);
      }

      // --- Drawing ---
      // Fetch the textures first: a click may have changed the page and with it the panel size.
      const GLuint menuTexture = panel.open ? menu.texture(Menu::kMain) : 0;
      const GLuint formatTexture = panel.open ? menu.texture(Menu::kFormat) : 0;
      const PanelGeometry mainPanel = panel.main(menu.mainVisibleHeight());
      const float mainVisibleV = static_cast<float>(menu.mainVisibleHeight()) / Menu::kMainHeight;
      PanelHover mainHover, formatHover;
      const bool mainHovered = menu.hoverRect(Menu::kMain, mainHover.rect, mainHover.radius,
                                              mainHover.danger);
      const bool formatHovered = menu.hoverRect(Menu::kFormat, formatHover.rect,
                                                formatHover.radius, formatHover.danger);
      // Glass menus show the scene blurred behind them.
      const bool glassMenu = panel.open && menu.glass();
      const bool showSubtitles = !subtitles.empty() && headValid && !loading;
      const GLuint subtitleTexture = showSubtitles ? subtitles.texture() : 0;
      const Mat4 subtitleModel =
          subtitlePlacement.update(app, player.aspect(), headPos, yawFromQuat(headRot), settings);
      const bool showLoading = stillLoading && headValid;
      const GLuint loadingTexture = showLoading ? loadingText.texture() : 0;
      Mat4 loadingModel;
      constexpr float kTextW = 1.4f, kTextH = kTextW / SubtitleOverlay::kAspect;
      if (showLoading) {
        // Raised a little: the text sits at the bottom of its texture.
        const Vec3 center = headPos + transformDir(frame, {0.0f, 0.1f, -2.0f});
        loadingModel = translation(center) * frame * scaling(kTextW, kTextH, 1);
      }
      // The recenter countdown, a little above that, following the gaze.
      const bool showCountdown = app.recenterAt > 0 && headValid;
      Mat4 countdownModel;
      if (showCountdown) {
        char text[64];
        std::snprintf(text, sizeof(text), strings(settings.language).recenterIn,
                      static_cast<int>(std::ceil(app.recenterAt - now)));
        countdownText.setText(text);
        const Mat4 gaze = rotation(headRot);
        const Vec3 center = headPos + transformDir(gaze, {0.0f, 0.25f, -2.0f});
        countdownModel = translation(center) * gaze * scaling(kTextW, kTextH, 1);
      }
      // The preview floats above the main panel, over the pointed-at spot.
      // Flat video shows (one eye of) the frame; 180°/360° video the view
      // straight ahead, rendered like in the headset.
      GLuint previewTexture = previewPlaceholder.texture();
      float aspect = 16.0f / 9.0f;
      if (previewing && preview.hasFrame() && app.youTubeAuto && !previewFormatKnown) {
        // The preview's own (smaller) stream may be packed differently: look at it too.
        const FrameAnalysis look = analyzeFrame(preview.texture(), preview.width(),
                                                preview.height(), 0);
        if (look.valid) {
          std::string source;
          previewFormat = youTubeFormat(app.youTubeKind,
                                        probe.url == app.currentFile ? probe.formats
                                                                     : std::vector<StreamFormatInfo>{},
                                        app.currentFile, preview.videoWidth(),
                                        preview.videoHeight(), &look, source);
          previewFormatKnown = true;
        }
      }
      const VideoFormat shownFormat =
          app.youTubeAuto && previewFormatKnown ? previewFormat : app.format;
      if (previewing && preview.hasFrame()) {
        if (shownFormat.projection == Projection::Flat) {
          previewTexture = preview.croppedTexture();
          aspect = static_cast<float>(preview.croppedWidth()) / preview.croppedHeight();
        } else {
          // 90° wide, looking at the video's front. Drawn upside down (down
          // and up swapped): the FBO's first row is the bottom, but panels
          // show a texture's first row at the top.
          EyeMatrices ahead;
          ahead.tanLeft = -1.0f;
          ahead.tanRight = 1.0f;
          ahead.tanDown = 9.0f / 16.0f;
          ahead.tanUp = -9.0f / 16.0f;
          glBindFramebuffer(GL_FRAMEBUFFER, previewView.fbo);
          glViewport(0, 0, previewView.width, previewView.height);
          renderer.drawEye(ahead, 0, true, preview.texture(),
                           static_cast<float>(preview.width()) / preview.height(), shownFormat,
                           app.flipVideo, ScreenPlacement());
          glBindFramebuffer(GL_FRAMEBUFFER, 0);
          previewTexture = previewView.texture;
        }
      }
      const bool showPreview = previewing && previewTexture != 0;
      Mat4 previewModel, previewTimeModel;
      if (showPreview) {
        float h = 0.16f, w = h * aspect;
        if (w > 0.36f) {
          w = 0.36f;
          h = w / aspect;
        }
        const float trackW = mainPanel.width * (Menu::kMainWidth - 80.0f) / Menu::kMainWidth;
        const float maxX = (mainPanel.width - w) / 2;
        const float x = std::clamp(static_cast<float>(hoverFraction - 0.5) * trackW, -maxX, maxX);
        const float y = mainPanel.height / 2 + 0.02f + h / 2;
        const Mat4 base = translation(mainPanel.center) * mainPanel.orientation;
        previewModel = base * translation({x, y, 0.01f}) * scaling(w, h, 1);
        const float lw = 0.3f, lh = lw / SubtitleOverlay::kAspect;
        previewTimeModel = base * translation({x, y - h / 2 + lh / 2, 0.012f}) * scaling(lw, lh, 1);
      }
      const Vec3 laserEnd = hitPanel ? hitPoint : rayOrigin + rayDir * kLaserLength;
      for (int i = 0; i < xr.eyeCount(); ++i) {
        xr.bindEye(i);
        renderer.drawEye(xr.eye(i), i, xr.srgbSwapchain(), videoTexture, frameAspect,
                         app.format, app.flipVideo, app.screen);
        if (showSubtitles)
          renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), subtitleTexture, subtitleModel, false,
                             0, 0);
        if (showLoading)
          renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), loadingTexture, loadingModel, false, 0,
                             0);
        if (showCountdown)
          renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), countdownText.texture(),
                             countdownModel, false, 0, 0);
        if (panel.open) {
          GLuint backdrop = 0;
          if (glassMenu) {
            // Copy what this eye shows so far into a mipmapped texture and blur
            // it: that is what shows through the glass.
            const int w = xr.eyeWidth(i), h = xr.eyeHeight(i);
            if (!glassBackdrop.texture || glassBackdrop.width != w || glassBackdrop.height != h)
              glassBackdrop.resize(w, h);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, xr.eyeFramebuffer(i));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, glassBackdrop.fbo);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, glassBackdrop.texture);
            glGenerateMipmap(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            backdrop = renderer.blurBackdrop(glassBackdrop.texture, w, h);
            xr.bindEye(i);
          }
          renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), menuTexture, mainPanel.model(),
                             hitPanelId == Menu::kMain, hitU, hitV, mainVisibleV,
                             mainHovered ? &mainHover : nullptr, backdrop, Menu::kGlassAlpha);
          if (menu.formatPanelOpen())
            renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), formatTexture, panel.format().model(),
                               hitPanelId == Menu::kFormat, hitU, hitV, 1.0f,
                               formatHovered ? &formatHover : nullptr, backdrop,
                               Menu::kGlassAlpha);
          if (showPreview) {
            renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), previewTexture, previewModel, false,
                               0, 0);
            renderer.drawPanel(xr.eye(i), xr.srgbSwapchain(), previewTime.texture(),
                               previewTimeModel, false, 0, 0);
          }
          if (hand.poseValid) renderer.drawLaser(xr.eye(i), xr.srgbSwapchain(), rayOrigin, laserEnd);
        }
        if (i == 0 && opts.mirror)
          mirrorToWindow(window, xr.eyeFramebuffer(0), xr.eyeWidth(0), xr.eyeHeight(0),
                         xr.srgbSwapchain());
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    timer.lap(FrameTimer::Draw);
    xr.endFrame();
    timer.lap(FrameTimer::XrEnd);
    timer.move(FrameTimer::XrEnd, FrameTimer::Fence, xr.lastFenceWaitMs());
    if (shouldRender && opts.mirror) glfwSwapBuffers(window);
    timer.lap(FrameTimer::Swap);
    timer.finishFrame();
  }
  rememberVolume();
  glDeleteFramebuffers(1, &previewView.fbo);
  glDeleteTextures(1, &previewView.texture);
  glassBackdrop.release();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Line by line, so a log of the output is complete even if the player is killed.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--hwdec=", 0) == 0) {
      opts.hwdec = arg.substr(8);
    } else if (arg == "--no-mirror") {
      opts.mirror = false;
    } else if (arg == "--timing") {
      opts.timing = true;
    } else if (arg == "--xr-test") {
      opts.xrTest = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg == "--version") {
      std::printf("tuxVRdome %s\n", TUXVRDOME_VERSION);
      return 0;
    } else {
      opts.file = arg;
    }
  }
  // yt-dlp installed with pipx (usually newer than the distribution's) comes first.
  if (const char* home = std::getenv("HOME")) {
    const char* path = std::getenv("PATH");
    const std::string local = std::string(home) + "/.local/bin";
    setenv("PATH", (local + ":" + (path ? path : "/usr/bin:/bin")).c_str(), 1);
  }
  // The file browser starts next to the video, which needs an absolute path.
  if (!opts.file.empty() && opts.file.find("://") == std::string::npos) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(opts.file, ec);
    if (!ec) opts.file = absolute.string();
  }

  glfwSetErrorCallback([](int code, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
  });
  if (!glfwInit()) return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "tuxVRdome", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(0);  // the headset sets the pace, not the desktop monitor

  const int rc = run(window, opts);

  glfwDestroyWindow(window);
  glfwTerminate();
  return rc;
}
