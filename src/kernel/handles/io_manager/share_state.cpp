// Sharing, delete-pending and rename bookkeeping for open guest paths.

#include "kernel/handles/io_manager/io_manager_internal.hpp"

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

}  // namespace

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

}  // namespace xenon::kernel
