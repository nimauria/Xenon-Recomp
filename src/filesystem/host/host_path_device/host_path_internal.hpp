#pragma once

// Private to HostPathDevice: the per-device open registry and the helpers
// shared by the device and its file handles.

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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

[[nodiscard]] inline FsError map_error(const std::error_code& ec) noexcept {
  if (!ec) return FsError::None;
  if (ec == std::errc::no_such_file_or_directory) return FsError::NotFound;
  if (ec == std::errc::file_exists) return FsError::AlreadyExists;
  if (ec == std::errc::permission_denied) return FsError::AccessDenied;
  if (ec == std::errc::directory_not_empty) return FsError::DirectoryNotEmpty;
  if (ec == std::errc::not_a_directory) return FsError::NotDirectory;
  if (ec == std::errc::is_a_directory) return FsError::IsDirectory;
  return FsError::IoError;
}

inline void release_open(const std::shared_ptr<HostOpenRegistry>& registry,
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

}  // namespace xenon::filesystem
