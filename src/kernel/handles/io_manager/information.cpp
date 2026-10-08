// NtQueryInformationFile/NtSetInformationFile-style queries and updates,
// volume information and delete-pending state.

#include "kernel/handles/io_manager/io_manager_internal.hpp"

namespace xenon::kernel {
namespace {

[[nodiscard]] std::uint64_t stable_path_hash(std::string_view path) noexcept {
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  auto key = filesystem::guest_path_key(path);
  std::uint64_t hash = kOffset;
  for (const unsigned char ch : key) {
    hash ^= ch;
    hash *= kPrime;
  }
  return hash;
}

[[nodiscard]] std::string parent_guest_path(std::string_view path) {
  const auto slash = path.find_last_of('\\');
  if (slash == std::string_view::npos) return {};
  if (slash == 0) return "\\";
  return std::string(path.substr(0, slash));
}

}  // namespace

IoStatus KernelIoManager::query_information(Handle handle,
                                            FileInformationClass info_class,
                                            FileInformation& out_info) const {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;

  filesystem::FileInfo stat_info{};
  switch (info_class) {
    case FileInformationClass::Basic: {
      status = file->stat(stat_info);
      if (!status.succeeded()) return status;
      FileBasicInformation info{};
      info.last_write_time = stat_info.last_write_time;
      info.attributes = stat_info.attributes;
      out_info = info;
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FileBasicInformation)};
    }
    case FileInformationClass::Standard: {
      status = file->stat(stat_info);
      if (!status.succeeded()) return status;
      FileStandardInformation info{};
      info.allocation_size = stat_info.allocation_size;
      info.end_of_file = stat_info.size;
      info.delete_pending = file->delete_pending();
      info.directory = stat_info.is_directory;
      out_info = info;
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FileStandardInformation)};
    }
    case FileInformationClass::Internal:
      out_info = FileInternalInformation{stable_path_hash(file->path())};
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FileInternalInformation)};
    case FileInformationClass::Position:
      out_info = FilePositionInformation{file->position()};
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FilePositionInformation)};
    case FileInformationClass::Name: {
      FileNameInformation info{file->path()};
      const auto length = info.name.size();
      out_info = std::move(info);
      return {KernelIoCode::Success, filesystem::FsError::None, length};
    }
    case FileInformationClass::NetworkOpen:
      status = file->stat(stat_info);
      if (!status.succeeded()) return status;
      out_info = FileNetworkOpenInformation{stat_info};
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FileNetworkOpenInformation)};
    case FileInformationClass::Mode:
      out_info = FileModeInformation{file->synchronous()};
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FileModeInformation)};
    case FileInformationClass::Alignment:
      out_info = FileAlignmentInformation{0};
      return {KernelIoCode::Success, filesystem::FsError::None,
              sizeof(FileAlignmentInformation)};
    case FileInformationClass::Disposition:
    case FileInformationClass::Allocation:
    case FileInformationClass::EndOfFile:
    case FileInformationClass::Rename:
    case FileInformationClass::Completion:
      return {KernelIoCode::Unsupported};
  }
  return {KernelIoCode::InvalidParameter};
}

IoStatus KernelIoManager::set_information(Handle handle,
                                          FileInformationClass info_class,
                                          const FileInformation& info,
                                          std::uint64_t context) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;

  switch (info_class) {
    case FileInformationClass::Position: {
      const auto* value = std::get_if<FilePositionInformation>(&info);
      if (!value) return {KernelIoCode::InvalidParameter};
      return file->set_position(value->current_byte_offset);
    }
    case FileInformationClass::Disposition: {
      const auto* value = std::get_if<FileDispositionInformation>(&info);
      if (!value) return {KernelIoCode::InvalidParameter};
      if (!access_allowed(view.granted_access, filesystem::FileAccess::Delete)) {
        return {KernelIoCode::AccessDenied};
      }
      return {file->set_delete_pending(value->delete_file)};
    }
    case FileInformationClass::EndOfFile: {
      const auto* value = std::get_if<FileEndOfFileInformation>(&info);
      if (!value) return {KernelIoCode::InvalidParameter};
      if (!access_allowed(view.granted_access, filesystem::FileAccess::Write)) {
        return {KernelIoCode::AccessDenied};
      }
      return file->resize(value->end_of_file, context);
    }
    case FileInformationClass::Allocation: {
      const auto* value = std::get_if<FileAllocationInformation>(&info);
      if (!value) return {KernelIoCode::InvalidParameter};
      filesystem::FileInfo stat_info{};
      status = file->stat(stat_info);
      if (!status.succeeded()) return status;
      // Xenon's generic Device API does not currently expose preallocation
      // separate from EOF. Do not silently resize the file because that would
      // give FILE_ALLOCATION_INFORMATION the wrong NT semantics.
      if (value->allocation_size < stat_info.size) {
        return {KernelIoCode::InvalidParameter};
      }
      return {KernelIoCode::Unsupported};
    }
    case FileInformationClass::Basic: {
      const auto* value = std::get_if<FileBasicInformation>(&info);
      if (!value) return {KernelIoCode::InvalidParameter};
      if (!access_allowed(view.granted_access, filesystem::FileAccess::Write)) {
        return {KernelIoCode::AccessDenied};
      }
      return file->set_basic_information(*value);
    }
    case FileInformationClass::Rename: {
      const auto* value = std::get_if<FileRenameInformation>(&info);
      if (!value || value->file_name.empty()) {
        return {KernelIoCode::InvalidParameter};
      }
      if (!access_allowed(view.granted_access, filesystem::FileAccess::Delete)) {
        return {KernelIoCode::AccessDenied};
      }

      std::string destination;
      if (value->root_directory != kInvalidHandle) {
        const auto root_status = resolve_rooted_path(
            value->root_directory, value->file_name, destination);
        if (root_status != KernelIoCode::Success) return {root_status};
      } else if (filesystem::guest_path_is_absolute(value->file_name)) {
        destination = value->file_name;
      } else {
        const auto parent = parent_guest_path(file->path());
        if (parent.empty()) return {KernelIoCode::InvalidParameter};
        std::string combined = parent;
        if (!combined.empty() && combined.back() != '\\') combined.push_back('\\');
        combined += value->file_name;
        const auto error = filesystem::normalize_guest_path(
            combined, destination, false);
        if (error != filesystem::FsError::None) {
          return from_filesystem_error(error);
        }
      }

      filesystem::ResolvedPath target;
      const auto resolve_error = vfs_->resolve(destination, target);
      if (resolve_error != filesystem::FsError::None) {
        return from_filesystem_error(resolve_error);
      }
      std::scoped_lock open_lock(open_mutex_);
      return file->rename_to(target, value->replace_if_exists);
    }
    case FileInformationClass::Completion: {
      const auto* value = std::get_if<FileCompletionInformation>(&info);
      if (!value) return {KernelIoCode::InvalidParameter};
      const auto result = associate_completion_port(
          handle, value->completion_port, value->key);
      return {result};
    }
    case FileInformationClass::Standard:
    case FileInformationClass::Internal:
    case FileInformationClass::Name:
    case FileInformationClass::NetworkOpen:
    case FileInformationClass::Mode:
    case FileInformationClass::Alignment:
      break;
  }
  return {KernelIoCode::Unsupported};
}

IoStatus KernelIoManager::query_volume_information(
    Handle handle, VolumeInformation& out_info) const {
  out_info = {};
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  const auto resolved = file->resolved_path();
  if (!resolved.device) return {KernelIoCode::InvalidHandle};
  filesystem::DiskSpace space{};
  const auto error = resolved.device->disk_space(space);
  if (error != filesystem::FsError::None) return from_filesystem_error(error);
  out_info.name = std::string(resolved.device->filesystem_name());
  out_info.read_only = resolved.device->read_only();
  out_info.filesystem_attributes = resolved.device->filesystem_attributes();
  out_info.component_name_max_length =
      resolved.device->component_name_max_length();
  out_info.sectors_per_allocation_unit =
      resolved.device->sectors_per_allocation_unit();
  out_info.bytes_per_sector = resolved.device->bytes_per_sector();
  out_info.space = space;
  return {KernelIoCode::Success, filesystem::FsError::None,
          sizeof(VolumeInformation)};
}

KernelIoCode KernelIoManager::set_delete_pending(Handle handle, bool value) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  const auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status.code;
  if (!access_allowed(view.granted_access, filesystem::FileAccess::Delete)) {
    return KernelIoCode::AccessDenied;
  }
  return file->set_delete_pending(value);
}

bool KernelIoManager::delete_pending(Handle handle) const {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  const auto status = lookup_file(handle, view, file);
  return status.succeeded() && file->delete_pending();
}

}  // namespace xenon::kernel
