#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path settingsPath() { return configDir() / "settings.conf"; }

}  // namespace

fs::path configDir() {
  fs::path base;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    base = fs::path(home ? home : ".") / ".config";
  }
  const fs::path dir = base / "tuxvrdome";
  // Once: take over what was kept under the player's former name.
  static bool checked = false;
  if (!checked) {
    checked = true;
    std::error_code ec;
    const fs::path old = base / "vrplayer";
    if (!fs::exists(dir, ec) && fs::is_directory(old, ec)) fs::rename(old, dir, ec);
  }
  return dir;
}

Settings loadSettings() {
  Settings s;
  std::ifstream in(settingsPath());
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "language") s.language = value == "en" ? Language::English : Language::German;
    else if (key == "theme") s.theme = value == "material" ? Theme::Material : Theme::Glass;
    else if (key == "floating_subtitles_2d") s.floatingSubtitles = value == "1";
    else if (key == "max_video_width") s.maxVideoWidth = std::atoi(value.c_str());
    else if (key == "lying_mode") s.lyingMode = value == "1";
    else if (key == "background") s.background = value != "0";
    else if (key == "menu_follows") s.menuFollows = value != "0";
    else if (key == "subtitle_size") s.subtitleSize = std::atoi(value.c_str());
    else if (key == "subtitle_height") s.subtitleRaise = std::atoi(value.c_str());
    else if (key == "subtitle_distance_cm") s.subtitleDistance = std::atoi(value.c_str());
  }
  return s;
}

void saveSettings(const Settings& s) {
  const fs::path path = settingsPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "Could not save settings\n");
    return;
  }
  out << "language=" << (s.language == Language::English ? "en" : "de") << "\n";
  out << "theme=" << (s.theme == Theme::Material ? "material" : "glass") << "\n";
  out << "floating_subtitles_2d=" << (s.floatingSubtitles ? 1 : 0) << "\n";
  out << "max_video_width=" << s.maxVideoWidth << "\n";
  out << "lying_mode=" << (s.lyingMode ? 1 : 0) << "\n";
  out << "background=" << (s.background ? 1 : 0) << "\n";
  out << "menu_follows=" << (s.menuFollows ? 1 : 0) << "\n";
  out << "subtitle_size=" << s.subtitleSize << "\n";
  out << "subtitle_height=" << s.subtitleRaise << "\n";
  out << "subtitle_distance_cm=" << s.subtitleDistance << "\n";
}
