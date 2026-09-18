#include "xenon/filesystem/virtual_file_system.hpp"

#include <algorithm>
#include <mutex>

#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {
namespace {

constexpr std::size_t kMaximumSymbolicLinkDepth = 8;

[[nodiscard]] bool alias_token(std::string_view path, std::string& out_alias,
                               std::string_view& out_suffix) {
  const auto colon = path.find(':');
  const auto separator = path.find('\\');
  if (colon == std::string_view::npos ||
      (separator != std::string_view::npos && separator < colon)) {
    return false;
  }
  out_alias.assign(path.substr(0, colon + 1));
  out_suffix = path.substr(colon + 1);
  return true;
}

}  // namespace

VirtualFileSystem::VirtualFileSystem() = default;
VirtualFileSystem::~VirtualFileSystem() = default;

FsError VirtualFileSystem::register_device(std::shared_ptr<Device> device) {
  if (!device) return FsError::InvalidArgument;
  const auto init = device->initialize();
  if (init != FsError::None) return init;

  std::unique_lock lock(mutex_);
  const auto duplicate = std::find_if(
      devices_.begin(), devices_.end(), [&](const auto& current) {
        return guest_path_equal(current->mount_point(), device->mount_point());
      });
  if (duplicate != devices_.end()) return FsError::AlreadyExists;
  devices_.push_back(std::move(device));
  return FsError::None;
}

FsError VirtualFileSystem::unregister_device(std::string_view mount_point) {
  std::string normalized;
  const auto error = normalize_guest_path(mount_point, normalized, false);
  if (error != FsError::None) return error;

  std::unique_lock lock(mutex_);
  const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
    return guest_path_equal(device->mount_point(), normalized);
  });
  if (it == devices_.end()) return FsError::NotFound;
  devices_.erase(it);
  return FsError::None;
}

void VirtualFileSystem::clear_devices() {
  std::unique_lock lock(mutex_);
  devices_.clear();
}

std::vector<MountInfo> VirtualFileSystem::mounts() const {
  std::shared_lock lock(mutex_);
  std::vector<MountInfo> result;
  result.reserve(devices_.size());
  for (const auto& device : devices_) {
    result.push_back({device->mount_point(), device->read_only()});
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return guest_path_key(lhs.mount_point) < guest_path_key(rhs.mount_point);
  });
  return result;
}

FsError VirtualFileSystem::register_symbolic_link(std::string_view alias,
                                                   std::string_view target) {
  std::string normalized_alias;
  if (alias.empty()) return FsError::InvalidPath;
  normalized_alias.assign(alias);
  std::replace(normalized_alias.begin(), normalized_alias.end(), '/', '\\');
  if (normalized_alias.back() != ':') return FsError::InvalidPath;
  if (normalized_alias.find('\\') != std::string::npos ||
      normalized_alias.find(':') != normalized_alias.size() - 1) {
    return FsError::InvalidPath;
  }

  std::string normalized_target;
  const auto target_error = normalize_guest_path(target, normalized_target, false);
  if (target_error != FsError::None) return target_error;

  std::unique_lock lock(mutex_);
  const auto key = guest_path_key(normalized_alias);
  if (symbolic_links_.contains(key)) return FsError::AlreadyExists;
  symbolic_links_.emplace(key, std::move(normalized_target));
  return FsError::None;
}

FsError VirtualFileSystem::unregister_symbolic_link(std::string_view alias) {
  std::unique_lock lock(mutex_);
  const auto erased = symbolic_links_.erase(guest_path_key(alias));
  return erased ? FsError::None : FsError::NotFound;
}

void VirtualFileSystem::clear_symbolic_links() {
  std::unique_lock lock(mutex_);
  symbolic_links_.clear();
}

std::vector<SymbolicLinkInfo> VirtualFileSystem::symbolic_links() const {
  std::shared_lock lock(mutex_);
  std::vector<SymbolicLinkInfo> result;
  result.reserve(symbolic_links_.size());
  for (const auto& [alias, target] : symbolic_links_) {
    result.push_back({alias, target});
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return guest_path_key(lhs.alias) < guest_path_key(rhs.alias);
  });
  return result;
}

FsError VirtualFileSystem::set_working_directory(std::string_view guest_path) {
  if (guest_path.empty()) {
    std::unique_lock lock(mutex_);
    working_directory_.clear();
    return FsError::None;
  }

  std::string normalized;
  const auto error = normalize_guest_path(guest_path, normalized, false);
  if (error != FsError::None) return error;
  std::unique_lock lock(mutex_);
  working_directory_ = std::move(normalized);
  return FsError::None;
}

std::string VirtualFileSystem::working_directory() const {
  std::shared_lock lock(mutex_);
  return working_directory_;
}

FsError VirtualFileSystem::make_absolute(std::string_view guest_path,
                                         std::string& out_path) const {
  std::string normalized;
  auto error = normalize_guest_path(guest_path, normalized, true);
  if (error != FsError::None) return error;
  if (guest_path_is_absolute(normalized)) {
    out_path = std::move(normalized);
    return FsError::None;
  }

  std::string working;
  {
    std::shared_lock lock(mutex_);
    working = working_directory_;
  }
  if (working.empty()) return FsError::InvalidPath;
  return normalize_guest_path(working + "\\" + normalized, out_path, false);
}

FsError VirtualFileSystem::expand_symbolic_links(std::string& path) const {
  for (std::size_t depth = 0; depth < kMaximumSymbolicLinkDepth; ++depth) {
    std::string alias;
    std::string_view suffix;
    if (!alias_token(path, alias, suffix)) return FsError::None;

    std::string target;
    {
      std::shared_lock lock(mutex_);
      const auto it = symbolic_links_.find(guest_path_key(alias));
      if (it == symbolic_links_.end()) return FsError::NotFound;
      target = it->second;
    }

    std::string combined = target;
    if (!suffix.empty()) {
      if (combined.back() != '\\' && suffix.front() != '\\') combined.push_back('\\');
      combined.append(suffix);
    }
    const auto error = normalize_guest_path(combined, path, false);
    if (error != FsError::None) return error;
  }

  std::string alias;
  std::string_view suffix;
  return alias_token(path, alias, suffix) ? FsError::TooManyLinks
                                          : FsError::None;
}

FsError VirtualFileSystem::resolve(std::string_view guest_path,
                                   ResolvedPath& out_path) const {
  out_path = {};
  std::string absolute;
  auto error = make_absolute(guest_path, absolute);
  if (error != FsError::None) return error;
  error = expand_symbolic_links(absolute);
  if (error != FsError::None) return error;

  std::shared_ptr<Device> best;
  {
    std::shared_lock lock(mutex_);
    for (const auto& device : devices_) {
      if (!guest_path_has_prefix(absolute, device->mount_point())) continue;
      if (!best || device->mount_point().size() > best->mount_point().size()) {
        best = device;
      }
    }
  }
  if (!best) return FsError::NotFound;

  std::string relative;
  if (absolute.size() > best->mount_point().size()) {
    relative = absolute.substr(best->mount_point().size());
    while (!relative.empty() && relative.front() == '\\') relative.erase(relative.begin());
  }

  out_path.device = std::move(best);
  out_path.canonical_path = std::move(absolute);
  out_path.relative_path = std::move(relative);
  return FsError::None;
}

FsError VirtualFileSystem::stat(std::string_view guest_path,
                                FileInfo& out_info) const {
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->stat(resolved.relative_path, out_info);
}

FsError VirtualFileSystem::open(std::string_view guest_path,
                                const OpenOptions& options,
                                std::unique_ptr<FileHandle>& out_file,
                                OpenAction* out_action) const {
  out_file.reset();
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->open(resolved.relative_path, options, out_file, out_action);
}

FsError VirtualFileSystem::list(
    std::string_view guest_path,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->list(resolved.relative_path, out_entries);
}

FsError VirtualFileSystem::query_directory(
    std::string_view guest_path, const DirectoryQuery& query,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->query_directory(resolved.relative_path, query,
                                          out_entries);
}

FsError VirtualFileSystem::create_directory(std::string_view guest_path,
                                            bool recursive) const {
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->create_directory(resolved.relative_path, recursive);
}

FsError VirtualFileSystem::remove(std::string_view guest_path) const {
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->remove(resolved.relative_path);
}

FsError VirtualFileSystem::rename(std::string_view old_guest_path,
                                  std::string_view new_guest_path,
                                  bool replace_existing) const {
  ResolvedPath source;
  auto error = resolve(old_guest_path, source);
  if (error != FsError::None) return error;
  ResolvedPath destination;
  error = resolve(new_guest_path, destination);
  if (error != FsError::None) return error;
  if (source.device.get() != destination.device.get()) return FsError::CrossDevice;
  return source.device->rename(source.relative_path, destination.relative_path,
                               replace_existing);
}

FsError VirtualFileSystem::disk_space(std::string_view guest_path,
                                      DiskSpace& out_space) const {
  ResolvedPath resolved;
  const auto error = resolve(guest_path, resolved);
  if (error != FsError::None) return error;
  return resolved.device->disk_space(out_space);
}

}  // namespace xenon::filesystem
