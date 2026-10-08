// Read, write, seek, resize, flush, stat and directory queries on open files.

#include "kernel/handles/io_manager/io_manager_internal.hpp"

namespace xenon::kernel {

IoStatus KernelIoManager::read(Handle handle, std::span<std::byte> destination,
                               std::optional<std::uint64_t> byte_offset,
                               bool update_position, std::uint64_t context) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  if (!access_allowed(view.granted_access, filesystem::FileAccess::Read)) {
    return {KernelIoCode::AccessDenied};
  }
  return file->read(destination, byte_offset, update_position, context);
}

IoStatus KernelIoManager::write(Handle handle,
                                std::span<const std::byte> source,
                                std::optional<std::uint64_t> byte_offset,
                                bool update_position, std::uint64_t context) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  if (!access_allowed(view.granted_access, filesystem::FileAccess::Write)) {
    return {KernelIoCode::AccessDenied};
  }
  return file->write(source, byte_offset, update_position, context);
}

IoStatus KernelIoManager::seek(Handle handle, std::int64_t offset,
                               filesystem::SeekOrigin origin) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  return file->seek(offset, origin);
}

IoStatus KernelIoManager::resize(Handle handle, std::uint64_t new_size,
                                 std::uint64_t context) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  if (!access_allowed(view.granted_access, filesystem::FileAccess::Write)) {
    return {KernelIoCode::AccessDenied};
  }
  return file->resize(new_size, context);
}

IoStatus KernelIoManager::flush(Handle handle, std::uint64_t context) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  return file->flush(context);
}

IoStatus KernelIoManager::stat(Handle handle,
                               filesystem::FileInfo& out_info) const {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  return file->stat(out_info);
}

IoStatus KernelIoManager::stat_path(
    std::string_view guest_path, filesystem::FileInfo& out_info) const {
  out_info = {};
  if (!vfs_ || guest_path.empty()) return {KernelIoCode::InvalidParameter};
  return from_filesystem_error(vfs_->stat(guest_path, out_info));
}

IoStatus KernelIoManager::stat_path_at(
    Handle root_directory, std::string_view guest_path,
    filesystem::FileInfo& out_info) const {
  std::string resolved_path;
  const auto resolved =
      resolve_rooted_path(root_directory, guest_path, resolved_path);
  if (resolved != KernelIoCode::Success) return {resolved};
  return stat_path(resolved_path, out_info);
}

IoStatus KernelIoManager::query_directory(
    Handle handle, const filesystem::DirectoryQuery* query, bool restart,
    std::size_t max_entries, std::vector<filesystem::DirectoryEntry>& out,
    bool& out_end, std::uint64_t context) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  if (!access_allowed(view.granted_access, filesystem::FileAccess::Read)) {
    return {KernelIoCode::AccessDenied};
  }
  return file->query_directory(query, restart, max_entries, out, out_end, context);
}

}  // namespace xenon::kernel
