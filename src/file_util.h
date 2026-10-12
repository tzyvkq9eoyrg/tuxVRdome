#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::string lowerCase(std::string s);
bool isVideoFile(const std::filesystem::path& p);
bool isPhotoFile(const std::filesystem::path& p);
bool isSubtitleFile(const std::filesystem::path& p);

// Case-insensitive name order, as shown in the file browser.
bool fileNameLess(const std::string& a, const std::string& b);

// The file `step` places after (or before, if negative) `current` in its
// folder, counting only files of the same kind: videos for a video, photos
// for a photo. An empty string if there is none.
std::string siblingMedia(const std::string& current, int step);

// The first file of the same kind as `current` in its folder, or an empty string.
std::string firstMedia(const std::string& current);

// Both reuse a folder's listing for a few seconds; call this after changing
// a folder (e.g. deleting a file) so the next call lists it afresh.
void forgetFolderListing();
