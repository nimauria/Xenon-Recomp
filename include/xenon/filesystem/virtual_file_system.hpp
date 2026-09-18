#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/filesystem/device.hpp"

namespace xenon::filesystem {

struct ResolvedPath {
  std::shared_ptr<Device> device{};
  std::string canonical_path{};
  std::string relative_path{};
};

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  VirtualFileSystem(const VirtualFileSystem&) = delete;
  VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

  [[nodiscard]] FsError register_device(std::shared_ptr<Device> device);
  [[nodiscard]] FsError unregister_device(std::string_view mount_point);
  void clear_devices();

  [[nodiscard]] FsError register_symbolic_link(std::string_view alias,
                                               std::string_view target);
  [[nodiscard]] FsError unregister_symbolic_link(std::string_view alias);
  void clear_symbolic_links();

  // Relative guest paths are resolved beneath this guest path. "game:" is a
  // useful default for recompiled titles, but callers may change or clear it.
  [[nodiscard]] FsError set_working_directory(std::string_view guest_path);
  [[nodiscard]] std::string working_directory() const;

  [[nodiscard]] FsError resolve(std::string_view guest_path,
                                ResolvedPath& out_path) const;
  [[nodiscard]] FsError stat(std::string_view guest_path,
                             FileInfo& out_info) const;
  [[nodiscard]] FsError open(std::string_view guest_path,
                             const OpenOptions& options,
                             std::unique_ptr<FileHandle>& out_file) const;
  [[nodiscard]] FsError list(
      std::string_view guest_path,
      std::vector<DirectoryEntry>& out_entries) const;
  [[nodiscard]] FsError create_directory(std::string_view guest_path,
                                         bool recursive = false) const;
  [[nodiscard]] FsError remove(std::string_view guest_path) const;
  [[nodiscard]] FsError rename(std::string_view old_guest_path,
                               std::string_view new_guest_path,
                               bool replace_existing = false) const;

 private:
  [[nodiscard]] FsError make_absolute(std::string_view guest_path,
                                      std::string& out_path) const;
  [[nodiscard]] FsError expand_symbolic_links(std::string& path) const;

  mutable std::shared_mutex mutex_{};
  std::vector<std::shared_ptr<Device>> devices_{};
  std::unordered_map<std::string, std::string> symbolic_links_{};
  std::string working_directory_{"game:"};
};

}  // namespace xenon::filesystem
