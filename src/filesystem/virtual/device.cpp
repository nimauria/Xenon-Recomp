#include "xenon/filesystem/device.hpp"

#include <memory>
#include <stdexcept>

#include "xenon/filesystem/directory_cursor.hpp"
#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {

Device::Device(std::string mount_point, bool read_only) : read_only_(read_only) {
  if (normalize_guest_path(mount_point, mount_point_, false) != FsError::None ||
      mount_point_.empty() || mount_point_.front() != '\\') {
    throw std::invalid_argument(
        "filesystem device mount point must be an absolute guest path");
  }
}

FsError Device::query_directory(
    std::string_view relative_path, const DirectoryQuery& query,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  if (!query.include_files && !query.include_directories) {
    return FsError::None;
  }
  if (query.pattern.find_first_of("\\/") != std::string::npos) {
    return FsError::InvalidArgument;
  }

  std::vector<DirectoryEntry> all_entries;
  const auto error = list(relative_path, all_entries);
  if (error != FsError::None) return error;

  const std::string_view pattern = query.pattern.empty()
                                       ? std::string_view{"*"}
                                       : std::string_view{query.pattern};
  for (auto& entry : all_entries) {
    if (entry.info.is_directory && !query.include_directories) continue;
    if (!entry.info.is_directory && !query.include_files) continue;
    if (!guest_wildcard_match(pattern, entry.name)) continue;
    out_entries.push_back(std::move(entry));
    if (query.max_entries != 0 && out_entries.size() >= query.max_entries) {
      break;
    }
  }
  return FsError::None;
}

FsError Device::open_directory_cursor(
    std::string_view relative_path, const DirectoryQuery& query,
    std::unique_ptr<DirectoryCursor>& out_cursor) const {
  out_cursor.reset();
  auto cursor_query = query;
  // Pagination belongs to DirectoryCursor::read. Capture the complete filtered
  // snapshot here so a later Xbox file object can restart or resume reliably.
  cursor_query.max_entries = 0;
  std::vector<DirectoryEntry> entries;
  const auto error = query_directory(relative_path, cursor_query, entries);
  if (error != FsError::None) return error;
  out_cursor = std::make_unique<DirectoryCursor>(std::move(entries));
  return FsError::None;
}

FsError Device::set_attributes(std::string_view,
                               const FileAttributeUpdate&) {
  return read_only_ ? FsError::ReadOnly : FsError::Unsupported;
}

FsError Device::set_last_write_time(
    std::string_view, std::filesystem::file_time_type) {
  return read_only_ ? FsError::ReadOnly : FsError::Unsupported;
}

}  // namespace xenon::filesystem
