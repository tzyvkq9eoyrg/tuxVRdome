#include "file_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace {

bool hasExtension(const fs::path& p, std::initializer_list<const char*> extensions) {
  const std::string ext = lowerCase(p.extension().string());
  return std::any_of(extensions.begin(), extensions.end(), [&](const char* e) { return ext == e; });
}

}  // namespace

std::string lowerCase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool isVideoFile(const fs::path& p) {
  return hasExtension(p, {".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v", ".ts", ".m2ts", ".wmv",
                          ".flv", ".mpg", ".mpeg"});
}

bool isPhotoFile(const fs::path& p) {
  return hasExtension(p, {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif", ".tif", ".tiff",
                          ".avif", ".heic", ".jxl"});
}

bool isSubtitleFile(const fs::path& p) {
  return hasExtension(p, {".srt", ".ass", ".ssa", ".vtt", ".sub", ".sup"});
}

bool fileNameLess(const std::string& a, const std::string& b) {
  return lowerCase(a) < lowerCase(b);
}

namespace {

// The last listing, reused for a few seconds: a slideshow asks for the next
// photo every few seconds, and listing a big folder on a network share takes
// a noticeable moment.
struct Listing {
  fs::path folder;
  bool photos = false;
  std::chrono::steady_clock::time_point when;
  std::vector<std::string> names;
};
Listing cache;

// The videos (or photos) in the folder of `current`, in file browser order.
std::vector<std::string> sameKindNames(const fs::path& current) {
  const bool photos = isPhotoFile(current);
  const auto now = std::chrono::steady_clock::now();
  if (cache.folder == current.parent_path() && cache.photos == photos &&
      now - cache.when < std::chrono::seconds(10))
    return cache.names;
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& item : fs::directory_iterator(current.parent_path(), ec)) {
    std::error_code typeError;
    const std::string name = item.path().filename().string();
    if (!name.empty() && name[0] != '.' && item.is_regular_file(typeError) &&
        (photos ? isPhotoFile(item.path()) : isVideoFile(item.path())))
      names.push_back(name);
  }
  std::sort(names.begin(), names.end(), fileNameLess);
  cache = {current.parent_path(), photos, now, names};
  return names;
}

}  // namespace

void forgetFolderListing() { cache = Listing(); }

std::string firstMedia(const std::string& current) {
  const std::vector<std::string> names = sameKindNames(current);
  return names.empty() ? std::string() : (fs::path(current).parent_path() / names.front()).string();
}

std::string siblingMedia(const std::string& current, int step) {
  const fs::path currentPath(current);
  const std::vector<std::string> names = sameKindNames(currentPath);
  const auto it = std::find(names.begin(), names.end(), currentPath.filename().string());
  if (it == names.end()) return {};
  const long index = std::distance(names.begin(), it) + step;
  if (index < 0 || index >= static_cast<long>(names.size())) return {};
  return (currentPath.parent_path() / names[index]).string();
}
