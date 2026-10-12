#include "video_memory.h"

#include "settings.h"  // configDir

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path configFile(const char* name) { return configDir() / name; }

fs::path memoryPath() { return configFile("videos.conf"); }

// A YouTube video is the same whichever link led to it.
std::string canonical(const std::string& location) {
  for (const char* marker : {"youtube.com/watch?v=", "youtu.be/", "youtube.com/shorts/"}) {
    const size_t pos = location.find(marker);
    if (pos == std::string::npos) continue;
    const size_t start = pos + std::string(marker).size();
    const size_t end = location.find_first_of("&?#/", start);
    return "youtube:" + location.substr(start, end == std::string::npos ? end : end - start);
  }
  return location;
}

// FNV-1a, 64 bit, as hex.
std::string hashOf(const std::string& location) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : canonical(location)) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

}  // namespace

void VideoMemory::load() {
  entries_.clear();
  std::ifstream in(memoryPath());
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string hash;
    int hasFormat = 0, projection = 0, stereo = 0;
    VideoSettings s;
    if (!(fields >> hash >> hasFormat >> projection >> stereo >> s.brightness >> s.contrast >>
          s.saturation >> s.zoom))
      continue;
    if (!(fields >> s.volume)) s.volume = -1;  // written before volumes were remembered
    if (projection < 0 || projection >= static_cast<int>(Projection::kCount) || stereo < 0 ||
        stereo >= static_cast<int>(StereoLayout::kCount))
      continue;
    s.hasFormat = hasFormat != 0;
    s.format.projection = static_cast<Projection>(projection);
    s.format.stereo = static_cast<StereoLayout>(stereo);
    entries_[hash] = s;
  }
}

VideoSettings VideoMemory::get(const std::string& location) const {
  const auto it = entries_.find(hashOf(location));
  return it != entries_.end() ? it->second : VideoSettings{};
}

void VideoMemory::set(const std::string& location, const VideoSettings& settings) {
  if (location.empty()) return;
  const std::string hash = hashOf(location);
  if (!settings.hasFormat && settings.pictureIsDefault() && settings.volume < 0)
    entries_.erase(hash);
  else
    entries_[hash] = settings;
  save();
}

void FolderMemory::load() {
  entries_.clear();
  std::ifstream in(configFile("folders.conf"));
  std::string hash;
  FolderView view;
  int gallery = 0;
  while (in >> hash >> view.sort >> gallery) {
    view.gallery = gallery != 0;
    entries_[hash] = view;
  }
}

bool FolderMemory::get(const std::string& folder, FolderView& view) const {
  const auto it = entries_.find(hashOf(folder));
  if (it == entries_.end()) return false;
  view = it->second;
  return true;
}

void FolderMemory::set(const std::string& folder, const FolderView& view) {
  entries_[hashOf(folder)] = view;
  save();
}

void FolderMemory::save() const {
  const fs::path path = configFile("folders.conf");
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  for (const auto& [hash, view] : entries_)
    out << hash << ' ' << view.sort << ' ' << (view.gallery ? 1 : 0) << '\n';
}

void VideoMemory::save() const {
  const fs::path path = memoryPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "Could not save video settings\n");
    return;
  }
  out << "# hash  format-chosen projection stereo brightness contrast saturation zoom volume\n";
  for (const auto& [hash, s] : entries_)
    out << hash << ' ' << (s.hasFormat ? 1 : 0) << ' ' << static_cast<int>(s.format.projection)
        << ' ' << static_cast<int>(s.format.stereo) << ' ' << s.brightness << ' ' << s.contrast
        << ' ' << s.saturation << ' ' << s.zoom << ' ' << s.volume << '\n';
}
