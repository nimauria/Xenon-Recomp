// KernelIoManager: status mapping, path resolution, open/close and
// handle flags.

#include "kernel/handles/io_manager/io_manager_internal.hpp"

namespace xenon::kernel {
namespace {

[[nodiscard]] std::uint32_t access_bits(filesystem::FileAccess access) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(access));
}

}  // namespace

std::string_view to_string(KernelIoCode code) noexcept {
  switch (code) {
    case KernelIoCode::Success: return "success";
    case KernelIoCode::Pending: return "pending";
    case KernelIoCode::Cancelled: return "cancelled";
    case KernelIoCode::Timeout: return "timeout";
    case KernelIoCode::InvalidHandle: return "invalid_handle";
    case KernelIoCode::InvalidObjectType: return "invalid_object_type";
    case KernelIoCode::InvalidParameter: return "invalid_parameter";
    case KernelIoCode::AccessDenied: return "access_denied";
    case KernelIoCode::ProtectedHandle: return "protected_handle";
    case KernelIoCode::NotFound: return "not_found";
    case KernelIoCode::AlreadyExists: return "already_exists";
    case KernelIoCode::NotDirectory: return "not_directory";
    case KernelIoCode::IsDirectory: return "is_directory";
    case KernelIoCode::DirectoryNotEmpty: return "directory_not_empty";
    case KernelIoCode::SharingViolation: return "sharing_violation";
    case KernelIoCode::Unsupported: return "unsupported";
    case KernelIoCode::CrossDevice: return "cross_device";
    case KernelIoCode::NoMoreFiles: return "no_more_files";
    case KernelIoCode::DeletePending: return "delete_pending";
    case KernelIoCode::FilesystemError: return "filesystem_error";
  }
  return "unknown";
}

IoStatus from_filesystem_error(filesystem::FsError error,
                               std::size_t information) noexcept {
  KernelIoCode code = KernelIoCode::FilesystemError;
  switch (error) {
    case filesystem::FsError::None:
      code = KernelIoCode::Success;
      break;
    case filesystem::FsError::NotFound:
      code = KernelIoCode::NotFound;
      break;
    case filesystem::FsError::AlreadyExists:
      code = KernelIoCode::AlreadyExists;
      break;
    case filesystem::FsError::AccessDenied:
    case filesystem::FsError::ReadOnly:
      code = KernelIoCode::AccessDenied;
      break;
    case filesystem::FsError::NotDirectory:
      code = KernelIoCode::NotDirectory;
      break;
    case filesystem::FsError::IsDirectory:
      code = KernelIoCode::IsDirectory;
      break;
    case filesystem::FsError::DirectoryNotEmpty:
      code = KernelIoCode::DirectoryNotEmpty;
      break;
    case filesystem::FsError::SharingViolation:
      code = KernelIoCode::SharingViolation;
      break;
    case filesystem::FsError::Unsupported:
      code = KernelIoCode::Unsupported;
      break;
    case filesystem::FsError::CrossDevice:
      code = KernelIoCode::CrossDevice;
      break;
    case filesystem::FsError::InvalidPath:
    case filesystem::FsError::InvalidArgument:
    case filesystem::FsError::TooManyLinks:
      code = KernelIoCode::InvalidParameter;
      break;
    case filesystem::FsError::IoError:
      code = KernelIoCode::FilesystemError;
      break;
  }
  return {code, error, code == KernelIoCode::Success ? information : 0, 0};
}

KernelIoManager::KernelIoManager(std::shared_ptr<filesystem::VirtualFileSystem> vfs)
    : vfs_(std::move(vfs)), share_state_(std::make_shared<OpenShareState>()) {}

KernelIoManager::~KernelIoManager() {
  // Only the private table is ours to clear; a shared process table outlives us.
  if (!shared_handles_) handles_.clear();
}

bool KernelIoManager::access_allowed(std::uint32_t granted,
                                     filesystem::FileAccess required) {
  return (granted & access_bits(required)) == access_bits(required);
}

IoStatus KernelIoManager::lookup_file(
    Handle handle, HandleView& out_view,
    std::shared_ptr<KernelFileObject>& out_file) const {
  out_view = {};
  out_file.reset();
  const auto lookup = table().lookup(handle, out_view);
  if (lookup != KernelIoCode::Success) return {lookup};
  if (!out_view.object || out_view.object->type() != ObjectType::File) {
    return {KernelIoCode::InvalidObjectType};
  }
  out_file = std::static_pointer_cast<KernelFileObject>(out_view.object);
  if (out_file->closed()) return {KernelIoCode::InvalidHandle};
  return {KernelIoCode::Success};
}

KernelIoCode KernelIoManager::resolve_rooted_path(
    Handle root_directory, std::string_view guest_path,
    std::string& out_path) const {
  out_path.clear();
  if (guest_path.empty()) return KernelIoCode::InvalidParameter;
  if (root_directory == kInvalidHandle) {
    out_path.assign(guest_path);
    return KernelIoCode::Success;
  }
  if (filesystem::guest_path_is_absolute(guest_path)) {
    return KernelIoCode::InvalidParameter;
  }

  HandleView view;
  std::shared_ptr<KernelFileObject> root;
  const auto status = lookup_file(root_directory, view, root);
  if (!status.succeeded()) return status.code;
  if (!root->is_directory()) return KernelIoCode::NotDirectory;

  std::string combined = root->path();
  if (!combined.empty() && combined.back() != '\\') combined.push_back('\\');
  combined.append(guest_path);
  const auto error = filesystem::normalize_guest_path(combined, out_path, false);
  return error == filesystem::FsError::None
             ? KernelIoCode::Success
             : from_filesystem_error(error).code;
}

IoStatus KernelIoManager::open(std::string_view guest_path,
                               const KernelOpenOptions& options,
                               Handle& out_handle,
                               filesystem::OpenAction* out_action) {
  out_handle = kInvalidHandle;
  if (out_action) *out_action = filesystem::OpenAction::None;
  if (!vfs_ || guest_path.empty()) return {KernelIoCode::InvalidParameter};
  if (options.delete_on_close &&
      !filesystem::has_access(options.file.access, filesystem::FileAccess::Delete)) {
    return {KernelIoCode::AccessDenied};
  }

  std::scoped_lock open_lock(open_mutex_);

  filesystem::ResolvedPath resolved;
  auto fs_error = vfs_->resolve(guest_path, resolved);
  if (fs_error != filesystem::FsError::None) return from_filesystem_error(fs_error);

  if (resolved.device->read_only() &&
      (filesystem::has_access(options.file.access, filesystem::FileAccess::Write) ||
       filesystem::has_access(options.file.access, filesystem::FileAccess::Delete))) {
    return {KernelIoCode::AccessDenied};
  }

  filesystem::FileInfo info{};
  const auto stat_error = resolved.device->stat(resolved.relative_path, info);
  const bool exists = stat_error == filesystem::FsError::None;
  if (!exists && stat_error != filesystem::FsError::NotFound) {
    return from_filesystem_error(stat_error);
  }

  bool directory = exists && info.is_directory;
  if (exists) {
    if (options.kind == OpenKind::File && directory) {
      return from_filesystem_error(filesystem::FsError::IsDirectory);
    }
    if (options.kind == OpenKind::Directory && !directory) {
      return from_filesystem_error(filesystem::FsError::NotDirectory);
    }
  }

  std::shared_ptr<FileObjectLease> lease;
  auto lease_status = share_state_->reserve(resolved, options.file.access,
                                            options.file.share, lease);
  if (lease_status != KernelIoCode::Success) return {lease_status};

  filesystem::OpenAction action = filesystem::OpenAction::None;
  std::unique_ptr<filesystem::FileHandle> file;

  if (exists && directory) {
    if (options.file.disposition == filesystem::CreateDisposition::Create) {
      lease->release();
      return from_filesystem_error(filesystem::FsError::AlreadyExists);
    }
    if (options.file.disposition == filesystem::CreateDisposition::Overwrite ||
        options.file.disposition == filesystem::CreateDisposition::OverwriteIf ||
        options.file.disposition == filesystem::CreateDisposition::Supersede) {
      lease->release();
      return {KernelIoCode::InvalidParameter};
    }
    action = filesystem::OpenAction::Opened;
  } else if (!exists && options.kind == OpenKind::Directory) {
    switch (options.file.disposition) {
      case filesystem::CreateDisposition::Open:
      case filesystem::CreateDisposition::Overwrite:
        lease->release();
        return from_filesystem_error(filesystem::FsError::NotFound);
      case filesystem::CreateDisposition::Create:
      case filesystem::CreateDisposition::OpenIf:
      case filesystem::CreateDisposition::Supersede:
      case filesystem::CreateDisposition::OverwriteIf:
        fs_error = resolved.device->create_directory(resolved.relative_path, false);
        if (fs_error != filesystem::FsError::None) {
          lease->release();
          return from_filesystem_error(fs_error);
        }
        directory = true;
        action = filesystem::OpenAction::Created;
        break;
    }
  } else {
    if (!exists && options.kind == OpenKind::Any) directory = false;
    fs_error = resolved.device->open(resolved.relative_path, options.file, file, &action);
    if (fs_error != filesystem::FsError::None) {
      lease->release();
      return from_filesystem_error(fs_error);
    }
  }

  auto object = std::make_shared<KernelFileObject>(
      resolved, options.file.access, options.file.share, options.synchronous,
      directory, std::move(file), lease);
  const auto insert = table().insert(object, access_bits(options.file.access),
                                      options.handle_flags, out_handle);
  if (insert != KernelIoCode::Success) {
    lease->release();
    return {insert};
  }

  if (options.delete_on_close) {
    const auto pending = object->set_delete_pending(true);
    if (pending != KernelIoCode::Success) {
      static_cast<void>(table().close(out_handle, true));
      out_handle = kInvalidHandle;
      return {pending};
    }
  }

  if (out_action) *out_action = action;
  return {KernelIoCode::Success};
}

IoStatus KernelIoManager::open_at(Handle root_directory,
                                  std::string_view guest_path,
                                  const KernelOpenOptions& options,
                                  Handle& out_handle,
                                  filesystem::OpenAction* out_action) {
  std::string resolved_path;
  const auto status = resolve_rooted_path(root_directory, guest_path, resolved_path);
  if (status != KernelIoCode::Success) {
    out_handle = kInvalidHandle;
    if (out_action) *out_action = filesystem::OpenAction::None;
    return {status};
  }
  return open(resolved_path, options, out_handle, out_action);
}

KernelIoCode KernelIoManager::duplicate(
    Handle source, const DuplicateHandleOptions& options, Handle& out_handle) {
  return table().duplicate(source, options, out_handle);
}

KernelIoCode KernelIoManager::close(Handle handle, bool force) {
  return table().close(handle, force);
}

KernelIoCode KernelIoManager::set_handle_flags(Handle handle,
                                               HandleFlags flags) {
  return table().set_flags(handle, flags);
}

}  // namespace xenon::kernel
