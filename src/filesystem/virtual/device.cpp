#include "xenon/filesystem/device.hpp"

#include <stdexcept>

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

}  // namespace xenon::filesystem
