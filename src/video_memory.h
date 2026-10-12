#pragma once

#include "video_format.h"

#include <map>
#include <string>

// What the player remembers about one video.
struct VideoSettings {
  bool hasFormat = false;  // the format was chosen by hand
  VideoFormat format;
  int brightness = 0, contrast = 0, saturation = 0;  // mpv's -100..100
  float zoom = 1.0f;  // 180°/360° video: magnification
  int volume = -1;    // 0..100 if changed while it played, -1 = not remembered

  bool pictureIsDefault() const {
    return brightness == 0 && contrast == 0 && saturation == 0 && zoom == 1.0f;
  }
};

// Per-video settings in videos.conf in configDir(). Videos are stored
// only as a hash of their path or YouTube id, so the file holds no names.
class VideoMemory {
 public:
  void load();
  VideoSettings get(const std::string& location) const;
  void set(const std::string& location, const VideoSettings& settings);

 private:
  void save() const;
  std::map<std::string, VideoSettings> entries_;  // by hash
};

// How the file browser shows a folder (its order and list or gallery), in
// folders.conf in configDir(); folders are stored only as hashes too.
struct FolderView {
  int sort = 0;  // Menu's SortMode
  bool gallery = false;
};

class FolderMemory {
 public:
  void load();
  // False if nothing is remembered for the folder.
  bool get(const std::string& folder, FolderView& view) const;
  void set(const std::string& folder, const FolderView& view);

 private:
  void save() const;
  std::map<std::string, FolderView> entries_;  // by hash
};
