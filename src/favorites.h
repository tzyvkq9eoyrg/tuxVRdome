#pragma once

#include <string>
#include <vector>

// Favourite folders and files of the file browser, kept in
// favorites.conf in configDir() (one path per line; to find them again,
// unlike the other memories, they have to be stored as paths).
class Favorites {
 public:
  Favorites();
  bool contains(const std::string& path) const;
  // Adds the path if it is not a favourite yet, else removes it; saves.
  void toggle(const std::string& path);
  // In the order they were added.
  const std::vector<std::string>& paths() const { return paths_; }

 private:
  void save() const;
  std::vector<std::string> paths_;
};
