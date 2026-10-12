#include "file_ops.h"

#include <gio/gio.h>

bool canTrash(const std::string& path) {
  GFile* file = g_file_new_for_path(path.c_str());
  GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH,
                                      G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
  const bool result =
      info && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH);
  if (info) g_object_unref(info);
  g_object_unref(file);
  return result;
}

RemoveResult removeFile(const std::string& path, bool permanent, std::string& error) {
  GFile* file = g_file_new_for_path(path.c_str());
  GError* gerror = nullptr;
  const gboolean ok = permanent ? g_file_delete(file, nullptr, &gerror)
                                : g_file_trash(file, nullptr, &gerror);
  if (!ok && gerror) error = gerror->message;
  if (gerror) g_error_free(gerror);
  g_object_unref(file);
  if (!ok) return RemoveResult::Failed;
  return permanent ? RemoveResult::Deleted : RemoveResult::Trashed;
}
