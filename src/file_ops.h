#pragma once

#include <string>

// Whether the file's location supports moving it to the trash. Local disks
// usually do; network shares (SMB, NFS, gvfs mounts) usually do not.
bool canTrash(const std::string& path);

enum class RemoveResult { Trashed, Deleted, Failed };

// Moves the file to the trash, or deletes it for good with permanent = true.
// On failure, error holds a short reason. May block on slow network shares.
RemoveResult removeFile(const std::string& path, bool permanent, std::string& error);
