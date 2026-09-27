#include "xenon/kernel/io_manager.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include "xenon/filesystem/path.hpp"

namespace xenon::kernel {
namespace {

[[nodiscard]] bool access_is_shared(filesystem::FileAccess access,
                                    filesystem::ShareAccess share) noexcept {
  if (filesystem::has_access(access, filesystem::FileAccess::Read) &&
      !filesystem::has_share(share, filesystem::ShareAccess::Read)) {
    return false;
  }
  if (filesystem::has_access(access, filesystem::FileAccess::Write) &&
      !filesystem::has_share(share, filesystem::ShareAccess::Write)) {
    return false;
  }
  if (filesystem::has_access(access, filesystem::FileAccess::Delete) &&
      !filesystem::has_share(share, filesystem::ShareAccess::Delete)) {
    return false;
  }
  return true;
}

[[nodiscard]] std::uint32_t access_bits(filesystem::FileAccess access) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(access));
}

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

struct KernelIoManager::OpenShareState
    : public std::enable_shared_from_this<KernelIoManager::OpenShareState> {
  struct Record {
    std::uint64_t id{};
    filesystem::FileAccess access{filesystem::FileAccess::None};
    filesystem::ShareAccess share{filesystem::ShareAccess::All};
    bool delete_pending{};
  };

  struct PathState {
    std::vector<Record> records{};
    bool sticky_delete_pending{};
    std::shared_ptr<filesystem::Device> device{};
    std::string relative_path{};
  };

  mutable std::mutex mutex{};
  std::unordered_map<std::string, PathState> paths{};
  std::uint64_t next_id{1};

  class Lease final : public FileObjectLease {
   public:
    Lease(std::shared_ptr<OpenShareState> state, std::string key, std::uint64_t id)
        : state_(std::move(state)), key_(std::move(key)), id_(id) {}
    ~Lease() override { release(); }

    KernelIoCode set_delete_pending(bool value) override {
      std::scoped_lock lock(mutex_);
      const auto state = state_.lock();
      if (!state || released_) return KernelIoCode::InvalidHandle;
      return state->set_delete_pending(key_, id_, value);
    }

    bool delete_pending() const override {
      std::scoped_lock lock(mutex_);
      const auto state = state_.lock();
      return state && !released_ && state->delete_pending(key_, id_);
    }

    KernelIoCode rename_to(const filesystem::ResolvedPath& target,
                           bool replace_existing) override {
      std::scoped_lock lock(mutex_);
      const auto state = state_.lock();
      if (!state || released_) return KernelIoCode::InvalidHandle;
      std::string new_key;
      const auto result =
          state->rename(key_, id_, target, replace_existing, new_key);
      if (result == KernelIoCode::Success) key_ = std::move(new_key);
      return result;
    }

    void release() noexcept override {
      std::string key;
      {
        std::scoped_lock lock(mutex_);
        if (released_) return;
        released_ = true;
        key = key_;
      }
      const auto state = state_.lock();
      if (state) state->release(std::move(key), id_);
    }

   private:
    std::weak_ptr<OpenShareState> state_{};
    mutable std::mutex mutex_{};
    std::string key_{};
    std::uint64_t id_{};
    bool released_{};
  };

  [[nodiscard]] KernelIoCode reserve(
      const filesystem::ResolvedPath& resolved,
      filesystem::FileAccess access, filesystem::ShareAccess share,
      std::shared_ptr<FileObjectLease>& out_lease);
  [[nodiscard]] KernelIoCode set_delete_pending(std::string_view key,
                                                std::uint64_t id,
                                                bool value);
  [[nodiscard]] bool delete_pending(std::string_view key,
                                    std::uint64_t id) const;
  [[nodiscard]] KernelIoCode rename(std::string_view key, std::uint64_t id,
                                    const filesystem::ResolvedPath& target,
                                    bool replace_existing,
                                    std::string& out_new_key);
  void release(std::string key, std::uint64_t id) noexcept;
};



KernelIoCode KernelIoManager::OpenShareState::reserve(
    const filesystem::ResolvedPath& resolved, filesystem::FileAccess access,
    filesystem::ShareAccess share, std::shared_ptr<FileObjectLease>& out_lease) {
  out_lease.reset();
  const auto key = filesystem::guest_path_key(resolved.canonical_path);
  std::scoped_lock lock(mutex);
  auto& path = paths[key];
  if (path.sticky_delete_pending ||
      std::any_of(path.records.begin(), path.records.end(),
                  [](const auto& record) { return record.delete_pending; })) {
    if (path.records.empty()) paths.erase(key);
    return KernelIoCode::DeletePending;
  }
  for (const auto& record : path.records) {
    if (!access_is_shared(access, record.share) ||
        !access_is_shared(record.access, share)) {
      if (path.records.empty()) paths.erase(key);
      return KernelIoCode::SharingViolation;
    }
  }

  path.device = resolved.device;
  path.relative_path = resolved.relative_path;
  const auto id = next_id++;
  path.records.push_back({id, access, share, false});
  out_lease = std::make_shared<Lease>(shared_from_this(), key, id);
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::OpenShareState::set_delete_pending(
    std::string_view key, std::uint64_t id, bool value) {
  std::scoped_lock lock(mutex);
  const auto it = paths.find(std::string(key));
  if (it == paths.end()) return KernelIoCode::InvalidHandle;
  auto& path = it->second;
  const auto record = std::find_if(path.records.begin(), path.records.end(),
                                   [&](const auto& item) { return item.id == id; });
  if (record == path.records.end()) return KernelIoCode::InvalidHandle;
  if (!value && path.sticky_delete_pending) return KernelIoCode::DeletePending;
  record->delete_pending = value;
  return KernelIoCode::Success;
}

bool KernelIoManager::OpenShareState::delete_pending(std::string_view key,
                                                     std::uint64_t id) const {
  std::scoped_lock lock(mutex);
  const auto it = paths.find(std::string(key));
  if (it == paths.end()) return false;
  if (it->second.sticky_delete_pending) return true;
  const auto record = std::find_if(
      it->second.records.begin(), it->second.records.end(),
      [&](const auto& item) { return item.id == id; });
  return record != it->second.records.end() && record->delete_pending;
}

KernelIoCode KernelIoManager::OpenShareState::rename(
    std::string_view key, std::uint64_t id,
    const filesystem::ResolvedPath& target, bool replace_existing,
    std::string& out_new_key) {
  out_new_key.clear();
  if (!target.device) return KernelIoCode::InvalidParameter;
  const auto target_key = filesystem::guest_path_key(target.canonical_path);

  std::scoped_lock lock(mutex);
  auto source_it = paths.find(std::string(key));
  if (source_it == paths.end()) return KernelIoCode::InvalidHandle;
  auto& source = source_it->second;
  const auto record_it = std::find_if(
      source.records.begin(), source.records.end(),
      [&](const auto& item) { return item.id == id; });
  if (record_it == source.records.end()) return KernelIoCode::InvalidHandle;
  if (source.records.size() != 1) return KernelIoCode::SharingViolation;
  if (source.sticky_delete_pending || record_it->delete_pending) {
    return KernelIoCode::DeletePending;
  }
  if (!source.device || source.device.get() != target.device.get()) {
    return KernelIoCode::CrossDevice;
  }

  if (target_key != key) {
    const auto target_it = paths.find(target_key);
    if (target_it != paths.end() &&
        (!target_it->second.records.empty() ||
         target_it->second.sticky_delete_pending)) {
      return KernelIoCode::SharingViolation;
    }
  }

  const auto fs_error = source.device->rename(
      source.relative_path, target.relative_path, replace_existing);
  if (fs_error != filesystem::FsError::None) {
    return from_filesystem_error(fs_error).code;
  }

  PathState moved = std::move(source);
  moved.device = target.device;
  moved.relative_path = target.relative_path;
  paths.erase(source_it);
  paths[target_key] = std::move(moved);
  out_new_key = target_key;
  return KernelIoCode::Success;
}

void KernelIoManager::OpenShareState::release(std::string key,
                                              std::uint64_t id) noexcept {
  std::shared_ptr<filesystem::Device> device;
  std::string relative_path;
  bool should_delete = false;
  {
    std::scoped_lock lock(mutex);
    const auto it = paths.find(key);
    if (it == paths.end()) return;
    auto& path = it->second;
    const auto record = std::find_if(path.records.begin(), path.records.end(),
                                     [&](const auto& item) { return item.id == id; });
    if (record == path.records.end()) return;
    const bool record_pending = record->delete_pending;
    path.records.erase(record);
    if (record_pending && !path.records.empty()) path.sticky_delete_pending = true;

    if (path.records.empty()) {
      should_delete = record_pending || path.sticky_delete_pending;
      device = path.device;
      relative_path = path.relative_path;
      paths.erase(it);
    }
  }

  if (should_delete && device) {
    const auto error = device->remove(relative_path);
    static_cast<void>(error);
  }
}

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

KernelIoManager::~KernelIoManager() { handles_.clear(); }

bool KernelIoManager::access_allowed(std::uint32_t granted,
                                     filesystem::FileAccess required) {
  return (granted & access_bits(required)) == access_bits(required);
}

IoStatus KernelIoManager::lookup_file(
    Handle handle, HandleView& out_view,
    std::shared_ptr<KernelFileObject>& out_file) const {
  out_view = {};
  out_file.reset();
  const auto lookup = handles_.lookup(handle, out_view);
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
  const auto insert = handles_.insert(object, access_bits(options.file.access),
                                      options.handle_flags, out_handle);
  if (insert != KernelIoCode::Success) {
    lease->release();
    return {insert};
  }

  if (options.delete_on_close) {
    const auto pending = object->set_delete_pending(true);
    if (pending != KernelIoCode::Success) {
      static_cast<void>(handles_.close(out_handle, true));
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
  return handles_.duplicate(source, options, out_handle);
}

KernelIoCode KernelIoManager::close(Handle handle, bool force) {
  return handles_.close(handle, force);
}

KernelIoCode KernelIoManager::set_handle_flags(Handle handle,
                                               HandleFlags flags) {
  return handles_.set_flags(handle, flags);
}

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

IoStatus KernelIoManager::begin_request(Handle handle, IoOperation operation,
                                        std::uint64_t context,
                                        std::shared_ptr<IoRequest>& out_request) {
  out_request.reset();
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  out_request = file->begin_request(operation, context);
  const auto snapshot = out_request->snapshot();
  if (snapshot.state == IoRequestState::Cancelled) return snapshot.result;
  return {KernelIoCode::Pending, filesystem::FsError::None, 0, out_request->id()};
}

KernelIoCode KernelIoManager::complete_request(Handle handle,
                                               std::uint64_t request_id,
                                               IoStatus result) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  const auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status.code;
  return file->complete_request(request_id, result);
}

KernelIoCode KernelIoManager::cancel_request(Handle handle,
                                             std::uint64_t request_id) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  const auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status.code;
  return file->cancel_request(request_id);
}

KernelIoCode KernelIoManager::create_completion_port(Handle& out_handle) {
  auto port = std::make_shared<IoCompletionPort>();
  return handles_.insert(port, 0xFFFFFFFFu, HandleFlags::None, out_handle);
}

KernelIoCode KernelIoManager::associate_completion_port(
    Handle file_handle, Handle port_handle, std::uint64_t key) {
  HandleView file_view;
  std::shared_ptr<KernelFileObject> file;
  const auto file_status = lookup_file(file_handle, file_view, file);
  if (!file_status.succeeded()) return file_status.code;

  HandleView port_view;
  const auto lookup = handles_.lookup(port_handle, port_view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!port_view.object || port_view.object->type() != ObjectType::IoCompletionPort) {
    return KernelIoCode::InvalidObjectType;
  }
  file->associate_completion_port(
      std::static_pointer_cast<IoCompletionPort>(port_view.object), key);
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::remove_completion(
    Handle port_handle, CompletionPacket& out_packet,
    std::chrono::milliseconds timeout) {
  HandleView view;
  const auto lookup = handles_.lookup(port_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::IoCompletionPort) {
    return KernelIoCode::InvalidObjectType;
  }
  auto port = std::static_pointer_cast<IoCompletionPort>(view.object);
  const bool removed = timeout.count() == 0
                           ? port->try_remove(out_packet)
                           : port->remove_for(timeout, out_packet);
  return removed ? KernelIoCode::Success : KernelIoCode::Timeout;
}

KernelIoCode KernelIoManager::create_event(bool manual_reset,
                                            bool initial_state,
                                            Handle& out_handle) {
  auto event = std::make_shared<KernelEvent>(manual_reset, initial_state);
  return handles_.insert(event, 0xFFFFFFFFu, HandleFlags::None, out_handle);
}

KernelIoCode KernelIoManager::set_event(Handle event_handle) {
  HandleView view;
  const auto lookup = handles_.lookup(event_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::Event) {
    return KernelIoCode::InvalidObjectType;
  }
  std::static_pointer_cast<KernelEvent>(view.object)->set();
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::reset_event(Handle event_handle) {
  HandleView view;
  const auto lookup = handles_.lookup(event_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::Event) {
    return KernelIoCode::InvalidObjectType;
  }
  std::static_pointer_cast<KernelEvent>(view.object)->reset();
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::wait_event(Handle event_handle,
                                         std::chrono::milliseconds timeout,
                                         bool& out_signaled) {
  out_signaled = false;
  HandleView view;
  const auto lookup = handles_.lookup(event_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::Event) {
    return KernelIoCode::InvalidObjectType;
  }
  out_signaled =
      std::static_pointer_cast<KernelEvent>(view.object)->wait_for(timeout);
  return out_signaled ? KernelIoCode::Success : KernelIoCode::Timeout;
}

}  // namespace xenon::kernel
