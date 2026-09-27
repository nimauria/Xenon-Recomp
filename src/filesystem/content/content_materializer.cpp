#include "xenon/filesystem/content_materializer.hpp"

#include <algorithm>

#include <array>
#include <fstream>
#include <system_error>
#include <vector>

namespace xenon::filesystem {
namespace {
constexpr std::size_t kCopyBufferBytes = 256u * 1024u;
constexpr std::size_t kMaxDepth = 128;
constexpr std::size_t kMaxEntries = 1'000'000;

bool safe_segment(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const unsigned char c : name) {
    if (c == 0 || c == '/' || c == '\\' || c < 0x20u) return false;
  }
  return true;
}

FsError extract_directory(const ReadOnlyContentSource& source,
                          std::string_view guest_path,
                          const std::filesystem::path& host_path,
                          std::size_t depth,
                          std::size_t& entry_count) {
  if (depth > kMaxDepth) return FsError::InvalidArgument;
  std::vector<DirectoryEntry> entries;
  const auto list_error = source.list(guest_path, entries);
  if (list_error != FsError::None) return list_error;

  std::vector<std::byte> buffer(kCopyBufferBytes);
  for (const auto& entry : entries) {
    if (++entry_count > kMaxEntries || !safe_segment(entry.name)) {
      return FsError::InvalidArgument;
    }
    const auto child_guest = guest_path.empty()
                                 ? entry.name
                                 : std::string(guest_path) + "\\" + entry.name;
    const auto child_host = host_path / std::filesystem::path(entry.name);

    std::error_code ec;
    if (entry.info.is_directory) {
      std::filesystem::create_directory(child_host, ec);
      if (ec && !std::filesystem::is_directory(child_host, ec)) {
        return FsError::IoError;
      }
      const auto nested = extract_directory(source, child_guest, child_host,
                                            depth + 1, entry_count);
      if (nested != FsError::None) return nested;
      continue;
    }

    const auto status = std::filesystem::symlink_status(child_host, ec);
    if (!ec && std::filesystem::is_symlink(status)) return FsError::AccessDenied;
    ec.clear();
    std::ofstream output(child_host, std::ios::binary | std::ios::trunc);
    if (!output) return FsError::IoError;

    std::uint64_t offset = 0;
    while (offset < entry.info.size) {
      const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
          buffer.size(), entry.info.size - offset));
      std::size_t got = 0;
      const auto read_error = source.read_at(
          child_guest, offset, std::span<std::byte>(buffer).first(requested), got);
      if (read_error != FsError::None || got == 0 || got > requested) {
        return read_error == FsError::None ? FsError::IoError : read_error;
      }
      output.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(got));
      if (!output) return FsError::IoError;
      offset += got;
    }
  }
  return FsError::None;
}
}  // namespace

FsError materialize_content_source(const ReadOnlyContentSource& source,
                                   const std::filesystem::path& destination) {
  if (destination.empty()) return FsError::InvalidArgument;
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(destination, ec);
  if (!ec && std::filesystem::is_symlink(status)) return FsError::AccessDenied;
  ec.clear();
  std::filesystem::create_directories(destination, ec);
  if (ec || !std::filesystem::is_directory(destination, ec)) {
    return FsError::IoError;
  }
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(destination, ec))) {
    return FsError::AccessDenied;
  }

  std::size_t entry_count = 0;
  return extract_directory(source, {}, destination, 0, entry_count);
}

}  // namespace xenon::filesystem
