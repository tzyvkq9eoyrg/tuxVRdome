#pragma once

#include "strings.h"

#include <filesystem>

// The look of the VR menus.
enum class Theme { Glass, Material };

// Where the player keeps its files: ~/.config/tuxvrdome (or under
// $XDG_CONFIG_HOME). A folder from the former name "vrplayer" is moved there.
std::filesystem::path configDir();

// User preferences, kept in settings.conf in configDir().
struct Settings {
  Theme theme = Theme::Glass;
  Language language = Language::English;
  bool floatingSubtitles = false;  // 2D video: subtitles float in front (3D/surround always do)
  int maxVideoWidth = 0;          // default YouTube quality: width limit, 0 = best available
  bool lyingMode = false;         // recentering also takes the head's tilt (watching lying down)
  bool background = true;         // night sky around the flat screen
  bool menuFollows = true;        // the menu comes along when you turn far away
  int subtitleSize = 100;         // percent
  int subtitleRaise = 0;          // how much higher than usual, percent of the picture
  int subtitleDistance = 150;     // floating subtitles of surround video: centimetres away
};

Settings loadSettings();
void saveSettings(const Settings& settings);
