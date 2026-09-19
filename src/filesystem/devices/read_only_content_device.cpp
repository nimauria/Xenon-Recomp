#include "xenon/filesystem/read_only_content_device.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>

#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {
namespace {

class ContentFileHandle final : public FileHandle {
 public:
  ContentFileHandle(std::shared_ptr<ReadOnlyContentSource> source,
                    std::string path, std::uint64_t size)
      : source_(std::move(source)), path_(std::move(path)), size_(size) {}

  [[nodiscard]] FsError read(std::span<std::byte> destination,
                             std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    const auto error = read_at_locked(position_, destination, bytes_read);
    if (error == FsError::None) position_ += bytes_read;
    return error;
  }

  [[nodiscard]] FsError write(std::span<const std::byte>,
                              std::size_t& bytes_written) override {
    bytes_written = 0;
    return FsError::ReadOnly;
  }

  [[nodiscard]] FsError read_at(std::uint64_t offset,
                                std::span<std::byte> destination,
                                std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    return read_at_locked(offset, destination, bytes_read);
  }

  [[nodiscard]] FsError write_at(std::uint64_t,
                                 std::span<const std::byte>,
                                 std::size_t& bytes_written) override {
    bytes_written = 0;
    return FsError::ReadOnly;
  }

  [[nodiscard]] FsError seek(std::int64_t offset, SeekOrigin origin,
                             std::uint64_t& new_position) override {
    std::scoped_lock lock(mutex_);
    std::uint64_t base = 0;
    switch (origin) {
      case SeekOrigin::Begin:
        break;
      case SeekOrigin::Current:
        base = position_;
        break;
      case SeekOrigin::End:
        base = size_;
        break;
    }

    if (offset < 0) {
      const auto magnitude =
          static_cast<std::uint64_t>(-(offset + 1)) + std::uint64_t{1};
      if (magnitude > base) return FsError::InvalidArgument;
      position_ = base - magnitude;
    } else {
      const auto positive = static_cast<std::uint64_t>(offset);
      if (base > std::numeric_limits<std::uint64_t>::max() - positive) {
        return FsError::InvalidArgument;
      }
      position_ = base + positive;
    }
    new_position = position_;
    return FsError::None;
  }

  [[nodiscard]] std::uint64_t tell() const noexcept override {
    std::scoped_lock lock(mutex_);
    return position_;
  }

  [[nodiscard]] FsError size(std::uint64_t& out_size) const override {
    out_size = size_;
    return FsError::None;
  }

  [[nodiscard]] FsError resize(std::uint64_t) override {
    return FsError::ReadOnly;
  }

  [[nodiscard]] FsError flush() override { return FsError::None; }

 private:
  [[nodiscard]] FsError read_at_locked(std::uint64_t offset,
                                       std::span<std::byte> destination,
                                       std::size_t& bytes_read) const {
    bytes_read = 0;
    if (destination.empty() || offset >= size_) return FsError::None;

    const auto remaining = size_ - offset;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
        remaining, static_cast<std::uint64_t>(destination.size())));
    return source_->read_at(path_, offset, destination.first(count), bytes_read);
  }

  std::shared_ptr<ReadOnlyContentSource> source_{};
  std::string path_{};
  std::uint64_t size_{};
  mutable std::mutex mutex_{};
  std::uint64_t position_{};
};

}  // namespace

ReadOnlyContentDevice::ReadOnlyContentDevice(
    std::string mount_point, std::shared_ptr<ReadOnlyContentSource> source)
    : Device(std::move(mount_point), true), source_(std::move(source)) {
  if (!source_) {
    throw std::invalid_argument("read-only content device requires a source");
  }
}

ReadOnlyContentDevice::~ReadOnlyContentDevice() = default;

FsError ReadOnlyContentDevice::initialize() {
  if (initialized_) return FsError::None;
  const auto error = source_->initialize();
  if (error == FsError::None) initialized_ = true;
  return error;
}

FsError ReadOnlyContentDevice::normalize_relative(
    std::string_view relative_path, std::string& out_path) const {
  if (relative_path.empty()) {
    out_path.clear();
    return FsError::None;
  }
  const auto error = normalize_guest_path(relative_path, out_path, true);
  if (error != FsError::None || guest_path_is_absolute(out_path)) {
    return FsError::InvalidPath;
  }
  return FsError::None;
}

FsError ReadOnlyContentDevice::stat(std::string_view relative_path,
                                    FileInfo& out_info) const {
  if (!initialized_) return FsError::IoError;
  std::string normalized;
  const auto error = normalize_relative(relative_path, normalized);
  if (error != FsError::None) return error;
  const auto result = source_->stat(normalized, out_info);
  if (result == FsError::None) {
    out_info.read_only = true;
    out_info.attributes |= FileAttributeReadOnly;
  }
  return result;
}

FsError ReadOnlyContentDevice::open(std::string_view relative_path,
                                    const OpenOptions& options,
                                    std::unique_ptr<FileHandle>& out_file,
                                    OpenAction* out_action) {
  out_file.reset();
  if (out_action) *out_action = OpenAction::None;
  if (!initialized_) return FsError::IoError;
  if (options.access == FileAccess::None) return FsError::InvalidArgument;
  if (has_access(options.access, FileAccess::Write) ||
      has_access(options.access, FileAccess::Delete)) {
    return FsError::ReadOnly;
  }

  std::string normalized;
  const auto normalize_error = normalize_relative(relative_path, normalized);
  if (normalize_error != FsError::None) return normalize_error;

  FileInfo info{};
  const auto stat_error = source_->stat(normalized, info);
  const bool exists = stat_error == FsError::None;
  if (!exists && stat_error != FsError::NotFound) return stat_error;

  switch (options.disposition) {
    case CreateDisposition::Open:
      if (!exists) return FsError::NotFound;
      break;
    case CreateDisposition::OpenIf:
      if (!exists) return FsError::ReadOnly;
      break;
    case CreateDisposition::Supersede:
    case CreateDisposition::Create:
    case CreateDisposition::Overwrite:
    case CreateDisposition::OverwriteIf:
      return FsError::ReadOnly;
  }

  if (info.is_directory) return FsError::IsDirectory;
  out_file = std::make_unique<ContentFileHandle>(source_, normalized, info.size);
  if (out_action) *out_action = OpenAction::Opened;
  return FsError::None;
}

FsError ReadOnlyContentDevice::list(
    std::string_view relative_path,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  if (!initialized_) return FsError::IoError;
  std::string normalized;
  const auto error = normalize_relative(relative_path, normalized);
  if (error != FsError::None) return error;
  const auto result = source_->list(normalized, out_entries);
  if (result != FsError::None) return result;
  for (auto& entry : out_entries) {
    entry.info.read_only = true;
    entry.info.attributes |= FileAttributeReadOnly;
  }
  std::sort(out_entries.begin(), out_entries.end(), [](const auto& lhs, const auto& rhs) {
    return guest_path_key(lhs.name) < guest_path_key(rhs.name);
  });
  return FsError::None;
}

FsError ReadOnlyContentDevice::create_directory(std::string_view, bool) {
  return FsError::ReadOnly;
}

FsError ReadOnlyContentDevice::remove(std::string_view) {
  return FsError::ReadOnly;
}

FsError ReadOnlyContentDevice::rename(std::string_view, std::string_view, bool) {
  return FsError::ReadOnly;
}

FsError ReadOnlyContentDevice::disk_space(DiskSpace& out_space) const {
  if (!initialized_) return FsError::IoError;
  const auto error = source_->disk_space(out_space);
  if (error == FsError::None) {
    out_space.free = 0;
    out_space.available = 0;
  }
  return error;
}

}  // namespace xenon::filesystem
