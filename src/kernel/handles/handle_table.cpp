#include "xenon/kernel/handle_table.hpp"

#include <limits>
#include <utility>
#include <vector>

namespace xenon::kernel {

HandleTable::~HandleTable() { clear(); }

Handle HandleTable::allocate_handle_locked() {
  constexpr Handle kFirstHandle = 4;
  constexpr Handle kLastHandle = 0x7FFFFFFCu;

  const auto start = next_handle_;
  do {
    const Handle candidate = next_handle_;
    if (next_handle_ >= kLastHandle) {
      next_handle_ = kFirstHandle;
    } else {
      next_handle_ += 4;
    }
    if (!entries_.contains(candidate)) return candidate;
  } while (next_handle_ != start);
  return kInvalidHandle;
}

KernelIoCode HandleTable::insert(std::shared_ptr<KernelObject> object,
                                 std::uint32_t granted_access,
                                 HandleFlags flags, Handle& out_handle) {
  out_handle = kInvalidHandle;
  if (!object) return KernelIoCode::InvalidParameter;

  std::scoped_lock lock(mutex_);
  const auto handle = allocate_handle_locked();
  if (handle == kInvalidHandle) return KernelIoCode::AccessDenied;
  entries_.emplace(handle, Entry{object, granted_access, flags});
  object->retain_handle();
  out_handle = handle;
  return KernelIoCode::Success;
}

KernelIoCode HandleTable::lookup(Handle handle, HandleView& out_view) const {
  out_view = {};
  if (handle == kInvalidHandle) return KernelIoCode::InvalidHandle;

  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(handle);
  if (it == entries_.end()) return KernelIoCode::InvalidHandle;
  out_view.object = it->second.object;
  out_view.granted_access = it->second.granted_access;
  out_view.flags = it->second.flags;
  return KernelIoCode::Success;
}

KernelIoCode HandleTable::duplicate(Handle source,
                                    const DuplicateHandleOptions& options,
                                    Handle& out_handle) {
  out_handle = kInvalidHandle;
  if (source == kInvalidHandle) return KernelIoCode::InvalidHandle;

  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(source);
  if (it == entries_.end()) return KernelIoCode::InvalidHandle;

  const auto source_access = it->second.granted_access;
  const auto desired_access = options.same_access ? source_access : options.desired_access;
  if ((desired_access & ~source_access) != 0) return KernelIoCode::AccessDenied;

  const auto object = it->second.object;
  const auto handle = allocate_handle_locked();
  if (handle == kInvalidHandle) return KernelIoCode::AccessDenied;
  entries_.emplace(handle, Entry{object, desired_access, options.flags});
  object->retain_handle();
  out_handle = handle;
  return KernelIoCode::Success;
}

KernelIoCode HandleTable::close(Handle handle, bool force) {
  std::shared_ptr<KernelObject> object;
  {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(handle);
    if (it == entries_.end()) return KernelIoCode::InvalidHandle;
    if (!force && has_flag(it->second.flags, HandleFlags::ProtectFromClose)) {
      return KernelIoCode::ProtectedHandle;
    }
    object = std::move(it->second.object);
    entries_.erase(it);
  }
  object->release_handle();
  return KernelIoCode::Success;
}

KernelIoCode HandleTable::set_flags(Handle handle, HandleFlags flags) {
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(handle);
  if (it == entries_.end()) return KernelIoCode::InvalidHandle;
  it->second.flags = flags;
  return KernelIoCode::Success;
}

std::size_t HandleTable::size() const {
  std::scoped_lock lock(mutex_);
  return entries_.size();
}

void HandleTable::clear() {
  std::vector<std::shared_ptr<KernelObject>> objects;
  {
    std::scoped_lock lock(mutex_);
    objects.reserve(entries_.size());
    for (auto& [handle, entry] : entries_) {
      static_cast<void>(handle);
      objects.push_back(std::move(entry.object));
    }
    entries_.clear();
  }
  for (const auto& object : objects) {
    if (object) object->release_handle();
  }
}

}  // namespace xenon::kernel
