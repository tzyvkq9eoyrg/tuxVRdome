#include "favorites.h"

#include "settings.h"  // configDir

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path favoritesPath() { return configDir() / "favorites.conf"; }

}  // namespace

Favorites::Favorites() {
  std::ifstream in(favoritesPath());
  std::string line;
  while (std::getline(in, line))
    if (!line.empty() && !contains(line)) paths_.push_back(line);
}

bool Favorites::contains(const std::string& path) const {
  return std::find(paths_.begin(), paths_.end(), path) != paths_.end();
}

void Favorites::toggle(const std::string& path) {
  const auto it = std::find(paths_.begin(), paths_.end(), path);
  if (it != paths_.end())
    paths_.erase(it);
  else
    paths_.push_back(path);
  save();
}

void Favorites::save() const {
  const fs::path path = favoritesPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "Could not save favourites\n");
    return;
  }
  for (const std::string& p : paths_) out << p << "\n";
}
