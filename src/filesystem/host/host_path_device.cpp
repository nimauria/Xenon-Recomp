#include "xenon/filesystem/host_path_device.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {

struct HostOpenRegistry {
  struct Record {
    std::uint64_t id{};
    FileAccess access{FileAccess::None};
    ShareAccess share{ShareAccess::All};
  };

  std::mutex mutex{};
  std::unordered_map<std::string, std::vector<Record>> records{};
  std::uint64_t next_id{1};
};

namespace {

[[nodiscard]] char ascii_lower(char value) noexcept {
  if (value >= 'A' && value <= 'Z') return static_cast<char>(value - 'A' + 'a');
  return value;
}

[[nodiscard]] bool ascii_iequal(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower(lhs[i]) != ascii_lower(rhs[i])) return false;
  }
  return true;
}

[[nodiscard]] std::vector<std::string> split_relative(std::string_view path) {
  std::vector<std::string> parts;
  std::size_t cursor = 0;
  while (cursor <= path.size()) {
    const auto next = path.find('\\', cursor);
    const auto end = next == std::string_view::npos ? path.size() : next;
    if (end != cursor) parts.emplace_back(path.substr(cursor, end - cursor));
    if (next == std::string_view::npos) break;
    cursor = next + 1;
  }
  return parts;
}

[[nodiscard]] FsError map_error(const std::error_code& ec) noexcept {
  if (!ec) return FsError::None;
  if (ec == std::errc::no_such_file_or_directory) return FsError::NotFound;
  if (ec == std::errc::file_exists) return FsError::AlreadyExists;
  if (ec == std::errc::permission_denied) return FsError::AccessDenied;
  if (ec == std::errc::directory_not_empty) return FsError::DirectoryNotEmpty;
  if (ec == std::errc::not_a_directory) return FsError::NotDirectory;
  if (ec == std::errc::is_a_directory) return FsError::IsDirectory;
  return FsError::IoError;
}

[[nodiscard]] std::string open_key(const std::filesystem::path& path) {
  auto key = path.lexically_normal().generic_string();
  std::transform(key.begin(), key.end(), key.begin(), ascii_lower);
  return key;
}

[[nodiscard]] bool access_is_shared(FileAccess access,
                                    ShareAccess share) noexcept {
  if (has_access(access, FileAccess::Read) &&
      !has_share(share, ShareAccess::Read)) {
    return false;
  }
  if (has_access(access, FileAccess::Write) &&
      !has_share(share, ShareAccess::Write)) {
    return false;
  }
  if (has_access(access, FileAccess::Delete) &&
      !has_share(share, ShareAccess::Delete)) {
    return false;
  }
  return true;
}

[[nodiscard]] FsError reserve_open(
    const std::shared_ptr<HostOpenRegistry>& registry, std::string_view key,
    const OpenOptions& options, std::uint64_t& out_id) {
  std::scoped_lock lock(registry->mutex);
  auto& records = registry->records[std::string(key)];
  for (const auto& record : records) {
    if (!access_is_shared(options.access, record.share) ||
        !access_is_shared(record.access, options.share)) {
      return FsError::SharingViolation;
    }
  }
  out_id = registry->next_id++;
  records.push_back({out_id, options.access, options.share});
  return FsError::None;
}

void release_open(const std::shared_ptr<HostOpenRegistry>& registry,
                  std::string_view key, std::uint64_t id) noexcept {
  if (!registry || id == 0) return;
  std::scoped_lock lock(registry->mutex);
  const auto it = registry->records.find(std::string(key));
  if (it == registry->records.end()) return;
  auto& records = it->second;
  records.erase(std::remove_if(records.begin(), records.end(),
                               [&](const auto& record) {
                                 return record.id == id;
                               }),
                records.end());
  if (records.empty()) registry->records.erase(it);
}

[[nodiscard]] bool delete_is_shared(
    const std::shared_ptr<HostOpenRegistry>& registry,
    std::string_view key) noexcept {
  std::scoped_lock lock(registry->mutex);
  const auto it = registry->records.find(std::string(key));
  if (it == registry->records.end()) return true;
  return std::all_of(it->second.begin(), it->second.end(), [](const auto& record) {
    return has_share(record.share, ShareAccess::Delete);
  });
}

[[nodiscard]] FileInfo make_info(const std::filesystem::path& path,
                                 std::error_code& ec) {
  FileInfo info{};
  const auto status = std::filesystem::status(path, ec);
  if (ec) return info;
  info.is_directory = std::filesystem::is_directory(status);
  info.read_only = (status.permissions() & std::filesystem::perms::owner_write) ==
                   std::filesystem::perms::none;
  if (info.is_directory) {
    info.attributes |= FileAttributeDirectory;
  } else {
    info.attributes |= FileAttributeNormal | FileAttributeArchive;
  }
  if (info.read_only) info.attributes |= FileAttributeReadOnly;
  if (!info.is_directory && std::filesystem::is_regular_file(status)) {
    info.size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    info.allocation_size = info.size;
  }
  info.last_write_time = std::filesystem::last_write_time(path, ec);
  return info;
}

class HostFileHandle final : public FileHandle {
 public:
  HostFileHandle(std::filesystem::path path, FileAccess access,
                 std::fstream stream, std::shared_ptr<HostOpenRegistry> registry,
                 std::string open_key_value, std::uint64_t open_id)
      : path_(std::move(path)),
        access_(access),
        stream_(std::move(stream)),
        registry_(std::move(registry)),
        open_key_(std::move(open_key_value)),
        open_id_(open_id) {}

  ~HostFileHandle() override { release_open(registry_, open_key_, open_id_); }

  [[nodiscard]] FsError read(std::span<std::byte> destination,
                             std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    const auto error = read_at_locked(position_, destination, bytes_read);
    if (error == FsError::None) position_ += bytes_read;
    return error;
  }

  [[nodiscard]] FsError write(std::span<const std::byte> source,
                              std::size_t& bytes_written) override {
    std::scoped_lock lock(mutex_);
    const auto error = write_at_locked(position_, source, bytes_written);
    if (error == FsError::None) position_ += bytes_written;
    return error;
  }

  [[nodiscard]] FsError read_at(std::uint64_t offset,
                                std::span<std::byte> destination,
                                std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    return read_at_locked(offset, destination, bytes_read);
  }

  [[nodiscard]] FsError write_at(std::uint64_t offset,
                                 std::span<const std::byte> source,
                                 std::size_t& bytes_written) override {
    std::scoped_lock lock(mutex_);
    return write_at_locked(offset, source, bytes_written);
  }

  [[nodiscard]] FsError seek(std::int64_t offset, SeekOrigin origin,
                             std::uint64_t& new_position) override {
    std::scoped_lock lock(mutex_);
    std::int64_t base = 0;
    if (origin == SeekOrigin::Current) {
      if (position_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return FsError::InvalidArgument;
      }
      base = static_cast<std::int64_t>(position_);
    } else if (origin == SeekOrigin::End) {
      std::error_code ec;
      const auto end = std::filesystem::file_size(path_, ec);
      if (ec || end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return ec ? map_error(ec) : FsError::InvalidArgument;
      }
      base = static_cast<std::int64_t>(end);
    }

    if (offset < 0) {
      const auto magnitude =
          static_cast<std::uint64_t>(-(offset + 1)) + std::uint64_t{1};
      if (magnitude > static_cast<std::uint64_t>(base)) {
        return FsError::InvalidArgument;
      }
    } else if (base > std::numeric_limits<std::int64_t>::max() - offset) {
      return FsError::InvalidArgument;
    }
    const auto candidate = base + offset;
    position_ = static_cast<std::uint64_t>(candidate);
    new_position = position_;
    return FsError::None;
  }

  [[nodiscard]] std::uint64_t tell() const noexcept override {
    std::scoped_lock lock(mutex_);
    return position_;
  }

  [[nodiscard]] FsError size(std::uint64_t& out_size) const override {
    std::scoped_lock lock(mutex_);
    std::error_code ec;
    out_size = std::filesystem::file_size(path_, ec);
    return map_error(ec);
  }

  [[nodiscard]] FsError resize(std::uint64_t new_size) override {
    if (!has_access(access_, FileAccess::Write)) return FsError::AccessDenied;
    std::scoped_lock lock(mutex_);
    stream_.flush();
    std::error_code ec;
    std::filesystem::resize_file(path_, new_size, ec);
    if (ec) return map_error(ec);
    if (position_ > new_size) position_ = new_size;
    return FsError::None;
  }

  [[nodiscard]] FsError flush() override {
    std::scoped_lock lock(mutex_);
    if (!has_access(access_, FileAccess::Write)) return FsError::None;
    stream_.flush();
    return stream_ ? FsError::None : FsError::IoError;
  }

 private:
  [[nodiscard]] FsError read_at_locked(std::uint64_t offset,
                                       std::span<std::byte> destination,
                                       std::size_t& bytes_read) {
    bytes_read = 0;
    if (!has_access(access_, FileAccess::Read)) return FsError::AccessDenied;
    if (destination.empty()) return FsError::None;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      return FsError::InvalidArgument;
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return FsError::IoError;
    stream_.read(reinterpret_cast<char*>(destination.data()),
                 static_cast<std::streamsize>(destination.size()));
    bytes_read = static_cast<std::size_t>(stream_.gcount());
    if (stream_.bad()) return FsError::IoError;
    stream_.clear();
    return FsError::None;
  }

  [[nodiscard]] FsError write_at_locked(std::uint64_t offset,
                                        std::span<const std::byte> source,
                                        std::size_t& bytes_written) {
    bytes_written = 0;
    if (!has_access(access_, FileAccess::Write)) return FsError::AccessDenied;
    if (source.empty()) return FsError::None;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      return FsError::InvalidArgument;
    }

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return FsError::IoError;
    stream_.write(reinterpret_cast<const char*>(source.data()),
                  static_cast<std::streamsize>(source.size()));
    if (!stream_) return FsError::IoError;
    bytes_written = source.size();
    return FsError::None;
  }

  std::filesystem::path path_{};
  FileAccess access_{};
  mutable std::mutex mutex_{};
  std::fstream stream_{};
  std::shared_ptr<HostOpenRegistry> registry_{};
  std::string open_key_{};
  std::uint64_t open_id_{};
  std::uint64_t position_{};
};

}  // namespace

HostPathDevice::HostPathDevice(std::string mount_point,
                               std::filesystem::path host_root,
                               HostPathDeviceOptions options)
    : Device(std::move(mount_point), options.read_only),
      configured_root_(std::move(host_root)),
      options_(options),
      open_registry_(std::make_shared<HostOpenRegistry>()) {}

HostPathDevice::~HostPathDevice() = default;

FsError HostPathDevice::initialize() {
  if (initialized_) return FsError::None;
  if (configured_root_.empty()) return FsError::InvalidPath;

  std::error_code ec;
  if (!std::filesystem::exists(configured_root_, ec)) {
    if (ec) return map_error(ec);
    if (!options_.create_root || options_.read_only) return FsError::NotFound;
    if (!std::filesystem::create_directories(configured_root_, ec) && ec) {
      return map_error(ec);
    }
  }
  if (!std::filesystem::is_directory(configured_root_, ec)) {
    return ec ? map_error(ec) : FsError::NotDirectory;
  }

  root_ = std::filesystem::weakly_canonical(configured_root_, ec);
  if (ec) return map_error(ec);
  initialized_ = true;
  return FsError::None;
}

bool HostPathDevice::path_is_within_root(const std::filesystem::path& path) const {
  auto root_it = root_.begin();
  auto path_it = path.begin();
  for (; root_it != root_.end(); ++root_it, ++path_it) {
    if (path_it == path.end()) return false;
#ifdef _WIN32
    if (!ascii_iequal(root_it->string(), path_it->string())) return false;
#else
    if (*root_it != *path_it) return false;
#endif
  }
  return true;
}

FsError HostPathDevice::resolve_existing(std::string_view relative_path,
                                         std::filesystem::path& out_path) const {
  if (!initialized_) return FsError::IoError;
  std::string normalized;
  if (relative_path.empty()) {
    out_path = root_;
    return FsError::None;
  }
  const auto normalize_error = normalize_guest_path(relative_path, normalized, true);
  if (normalize_error != FsError::None || guest_path_is_absolute(normalized)) {
    return FsError::InvalidPath;
  }

  auto current = root_;
  for (const auto& part : split_relative(normalized)) {
    std::filesystem::path candidate = current / std::filesystem::path(part);
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec)) {
      if (ec) return map_error(ec);
      if (!options_.case_insensitive) return FsError::NotFound;
      if (!std::filesystem::is_directory(current, ec)) {
        return ec ? map_error(ec) : FsError::NotDirectory;
      }
      bool found = false;
      for (std::filesystem::directory_iterator it(current, ec), end; !ec && it != end;
           it.increment(ec)) {
        if (ascii_iequal(it->path().filename().string(), part)) {
          candidate = it->path();
          found = true;
          break;
        }
      }
      if (ec) return map_error(ec);
      if (!found) return FsError::NotFound;
    }

    current = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) return map_error(ec);
    if (!path_is_within_root(current)) return FsError::AccessDenied;
  }

  out_path = std::move(current);
  return FsError::None;
}

FsError HostPathDevice::resolve_for_creation(std::string_view relative_path,
                                             std::filesystem::path& out_path,
                                             bool allow_existing) const {
  if (relative_path.empty()) return FsError::InvalidPath;
  std::string normalized;
  const auto normalize_error = normalize_guest_path(relative_path, normalized, true);
  if (normalize_error != FsError::None || guest_path_is_absolute(normalized)) {
    return FsError::InvalidPath;
  }

  const auto parts = split_relative(normalized);
  if (parts.empty()) return FsError::InvalidPath;

  std::string parent_relative;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (!parent_relative.empty()) parent_relative.push_back('\\');
    parent_relative += parts[i];
  }

  std::filesystem::path parent;
  const auto parent_error = resolve_existing(parent_relative, parent);
  if (parent_error != FsError::None) return parent_error;
  std::error_code ec;
  if (!std::filesystem::is_directory(parent, ec)) {
    return ec ? map_error(ec) : FsError::NotDirectory;
  }

  std::filesystem::path existing;
  const auto existing_error = resolve_existing(normalized, existing);
  if (existing_error == FsError::None) {
    if (!allow_existing) return FsError::AlreadyExists;
    out_path = std::move(existing);
    return FsError::None;
  }
  if (existing_error != FsError::NotFound) return existing_error;

  out_path = parent / std::filesystem::path(parts.back());
  const auto canonical_parent = std::filesystem::weakly_canonical(out_path.parent_path(), ec);
  if (ec) return map_error(ec);
  if (!path_is_within_root(canonical_parent)) return FsError::AccessDenied;
  return FsError::None;
}

FsError HostPathDevice::stat(std::string_view relative_path,
                             FileInfo& out_info) const {
  std::filesystem::path path;
  const auto error = resolve_existing(relative_path, path);
  if (error != FsError::None) return error;
  std::error_code ec;
  out_info = make_info(path, ec);
  return map_error(ec);
}

FsError HostPathDevice::open(std::string_view relative_path,
                             const OpenOptions& options,
                             std::unique_ptr<FileHandle>& out_file,
                             OpenAction* out_action) {
  out_file.reset();
  if (out_action) *out_action = OpenAction::None;
  if (options.access == FileAccess::None) return FsError::InvalidArgument;
  const bool wants_write = has_access(options.access, FileAccess::Write);
  if (read_only_ && wants_write) return FsError::ReadOnly;

  std::filesystem::path path;
  FsError error = FsError::None;
  switch (options.disposition) {
    case CreateDisposition::Open:
    case CreateDisposition::Overwrite:
      error = resolve_existing(relative_path, path);
      break;
    case CreateDisposition::Create:
      error = resolve_for_creation(relative_path, path, false);
      break;
    case CreateDisposition::Supersede:
    case CreateDisposition::OpenIf:
    case CreateDisposition::OverwriteIf:
      error = resolve_for_creation(relative_path, path, true);
      break;
  }
  if (error != FsError::None) return error;

  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) return map_error(ec);
  const bool disposition_will_mutate =
      options.disposition == CreateDisposition::Supersede ||
      options.disposition == CreateDisposition::Create ||
      options.disposition == CreateDisposition::Overwrite ||
      options.disposition == CreateDisposition::OverwriteIf ||
      (options.disposition == CreateDisposition::OpenIf && !exists);
  if (disposition_will_mutate && !wants_write) return FsError::AccessDenied;
  if (read_only_ && disposition_will_mutate) return FsError::ReadOnly;

  if (exists && std::filesystem::is_directory(path, ec)) {
    return FsError::IsDirectory;
  }
  if (ec) return map_error(ec);

  OpenAction action = OpenAction::None;
  switch (options.disposition) {
    case CreateDisposition::Supersede:
      action = exists ? OpenAction::Superseded : OpenAction::Created;
      break;
    case CreateDisposition::Open:
      action = OpenAction::Opened;
      break;
    case CreateDisposition::Create:
      action = OpenAction::Created;
      break;
    case CreateDisposition::OpenIf:
      action = exists ? OpenAction::Opened : OpenAction::Created;
      break;
    case CreateDisposition::Overwrite:
      action = OpenAction::Overwritten;
      break;
    case CreateDisposition::OverwriteIf:
      action = exists ? OpenAction::Overwritten : OpenAction::Created;
      break;
  }

  const auto key = open_key(path);
  std::uint64_t open_id = 0;
  const auto share_error = reserve_open(open_registry_, key, options, open_id);
  if (share_error != FsError::None) return share_error;
  struct OpenReservationGuard {
    std::shared_ptr<HostOpenRegistry> registry;
    std::string key;
    std::uint64_t id{};
    bool committed{};
    ~OpenReservationGuard() {
      if (!committed) release_open(registry, key, id);
    }
  } guard{open_registry_, key, open_id, false};

  const bool should_create =
      options.disposition == CreateDisposition::Supersede ||
      options.disposition == CreateDisposition::Create ||
      options.disposition == CreateDisposition::OpenIf ||
      options.disposition == CreateDisposition::OverwriteIf;
  const bool should_truncate =
      options.disposition == CreateDisposition::Supersede ||
      options.disposition == CreateDisposition::Overwrite ||
      options.disposition == CreateDisposition::OverwriteIf;

  if (should_create && !std::filesystem::exists(path, ec)) {
    std::ofstream create(path, std::ios::binary | std::ios::out);
    if (!create) return FsError::IoError;
  }
  if (ec) return map_error(ec);

  if (should_truncate) {
    if (!wants_write) return FsError::AccessDenied;
    std::ofstream truncate(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!truncate) return FsError::IoError;
  }

  std::ios::openmode mode = std::ios::binary;
  if (has_access(options.access, FileAccess::Read)) mode |= std::ios::in;
  if (has_access(options.access, FileAccess::Write)) {
    // Keep std::fstream from implicitly truncating an existing file.
    mode |= std::ios::in | std::ios::out;
  }
  std::fstream stream(path, mode);
  if (!stream) return FsError::IoError;
  out_file = std::make_unique<HostFileHandle>(path, options.access, std::move(stream),
                                              open_registry_, key, open_id);
  guard.committed = true;
  if (out_action) *out_action = action;
  return FsError::None;
}

FsError HostPathDevice::list(
    std::string_view relative_path,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  std::filesystem::path path;
  const auto error = resolve_existing(relative_path, path);
  if (error != FsError::None) return error;
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    return ec ? map_error(ec) : FsError::NotDirectory;
  }

  for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end;
       it.increment(ec)) {
    DirectoryEntry entry{};
    entry.name = it->path().filename().string();
    entry.info = make_info(it->path(), ec);
    if (ec) break;
    out_entries.push_back(std::move(entry));
  }
  if (ec) return map_error(ec);
  std::sort(out_entries.begin(), out_entries.end(), [](const auto& lhs, const auto& rhs) {
    return guest_path_key(lhs.name) < guest_path_key(rhs.name);
  });
  return FsError::None;
}

FsError HostPathDevice::create_directory(std::string_view relative_path,
                                         bool recursive) {
  if (read_only_) return FsError::ReadOnly;
  if (relative_path.empty()) return FsError::AlreadyExists;

  if (recursive) {
    std::string normalized;
    const auto normal_error = normalize_guest_path(relative_path, normalized, true);
    if (normal_error != FsError::None || guest_path_is_absolute(normalized)) {
      return FsError::InvalidPath;
    }
    std::string partial;
    for (const auto& component : split_relative(normalized)) {
      if (!partial.empty()) partial.push_back('\\');
      partial += component;
      std::filesystem::path existing;
      const auto existing_error = resolve_existing(partial, existing);
      if (existing_error == FsError::None) {
        std::error_code ec;
        if (!std::filesystem::is_directory(existing, ec)) {
          return ec ? map_error(ec) : FsError::NotDirectory;
        }
        continue;
      }
      if (existing_error != FsError::NotFound) return existing_error;
      std::filesystem::path path;
      const auto create_error = resolve_for_creation(partial, path, false);
      if (create_error != FsError::None) return create_error;
      std::error_code ec;
      if (!std::filesystem::create_directory(path, ec)) {
        return ec ? map_error(ec) : FsError::AlreadyExists;
      }
    }
    return FsError::None;
  }

  std::filesystem::path path;
  const auto error = resolve_for_creation(relative_path, path, false);
  if (error != FsError::None) return error;
  std::error_code ec;
  if (!std::filesystem::create_directory(path, ec)) {
    return ec ? map_error(ec) : FsError::AlreadyExists;
  }
  return FsError::None;
}

FsError HostPathDevice::remove(std::string_view relative_path) {
  if (read_only_) return FsError::ReadOnly;
  if (relative_path.empty()) return FsError::AccessDenied;
  std::filesystem::path path;
  const auto error = resolve_existing(relative_path, path);
  if (error != FsError::None) return error;
  if (!delete_is_shared(open_registry_, open_key(path))) {
    return FsError::SharingViolation;
  }
  std::error_code ec;
  if (!std::filesystem::remove(path, ec)) {
    return ec ? map_error(ec) : FsError::NotFound;
  }
  return FsError::None;
}

FsError HostPathDevice::rename(std::string_view old_relative_path,
                               std::string_view new_relative_path,
                               bool replace_existing) {
  if (read_only_) return FsError::ReadOnly;
  if (old_relative_path.empty() || new_relative_path.empty()) return FsError::AccessDenied;

  std::filesystem::path source;
  auto error = resolve_existing(old_relative_path, source);
  if (error != FsError::None) return error;

  std::filesystem::path destination;
  error = resolve_for_creation(new_relative_path, destination, replace_existing);
  if (error != FsError::None) return error;
  if (!delete_is_shared(open_registry_, open_key(source)) ||
      !delete_is_shared(open_registry_, open_key(destination))) {
    return FsError::SharingViolation;
  }

  std::error_code ec;
  if (std::filesystem::exists(destination, ec)) {
    if (ec) return map_error(ec);
    if (!replace_existing) return FsError::AlreadyExists;
    if (std::filesystem::is_directory(destination, ec)) return FsError::IsDirectory;
    if (!std::filesystem::remove(destination, ec)) return map_error(ec);
  }
  std::filesystem::rename(source, destination, ec);
  return map_error(ec);
}

FsError HostPathDevice::set_attributes(
    std::string_view relative_path, const FileAttributeUpdate& update) {
  if (read_only_) return FsError::ReadOnly;
  constexpr std::uint32_t kSupportedMask = FileAttributeReadOnly;
  if ((update.mask & ~kSupportedMask) != 0) return FsError::Unsupported;
  if (update.mask == FileAttributeNone) return FsError::None;

  std::filesystem::path path;
  const auto error = resolve_existing(relative_path, path);
  if (error != FsError::None) return error;

  std::error_code ec;
  auto permissions = std::filesystem::status(path, ec).permissions();
  if (ec) return map_error(ec);
  const auto write_bits = std::filesystem::perms::owner_write |
                          std::filesystem::perms::group_write |
                          std::filesystem::perms::others_write;
  if ((update.value & FileAttributeReadOnly) != 0) {
    permissions &= ~write_bits;
  } else {
    // Restoring owner write is the portable minimum. Do not broaden group or
    // other permissions that the host file did not previously grant.
    permissions |= std::filesystem::perms::owner_write;
  }
  std::filesystem::permissions(path, permissions,
                               std::filesystem::perm_options::replace, ec);
  return map_error(ec);
}

FsError HostPathDevice::set_last_write_time(
    std::string_view relative_path,
    std::filesystem::file_time_type last_write_time) {
  if (read_only_) return FsError::ReadOnly;
  std::filesystem::path path;
  const auto error = resolve_existing(relative_path, path);
  if (error != FsError::None) return error;
  std::error_code ec;
  std::filesystem::last_write_time(path, last_write_time, ec);
  return map_error(ec);
}

FsError HostPathDevice::disk_space(DiskSpace& out_space) const {
  if (!initialized_) return FsError::IoError;
  std::error_code ec;
  const auto info = std::filesystem::space(root_, ec);
  if (ec) return map_error(ec);
  out_space.capacity = info.capacity;
  out_space.free = info.free;
  out_space.available = info.available;
  return FsError::None;
}

}  // namespace xenon::filesystem
