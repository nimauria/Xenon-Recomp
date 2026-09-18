#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/filesystem/file.hpp"
#include "xenon/filesystem/types.hpp"

namespace xenon::filesystem {

class Device {
 public:
  Device(std::string mount_point, bool read_only);
  virtual ~Device() = default;

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  [[nodiscard]] const std::string& mount_point() const noexcept {
    return mount_point_;
  }
  [[nodiscard]] bool read_only() const noexcept { return read_only_; }

  [[nodiscard]] virtual FsError initialize() = 0;
  [[nodiscard]] virtual FsError stat(std::string_view relative_path,
                                     FileInfo& out_info) const = 0;
  [[nodiscard]] virtual FsError open(std::string_view relative_path,
                                     const OpenOptions& options,
                                     std::unique_ptr<FileHandle>& out_file) = 0;
  [[nodiscard]] virtual FsError list(
      std::string_view relative_path,
      std::vector<DirectoryEntry>& out_entries) const = 0;
  [[nodiscard]] virtual FsError create_directory(std::string_view relative_path,
                                                 bool recursive) = 0;
  [[nodiscard]] virtual FsError remove(std::string_view relative_path) = 0;
  [[nodiscard]] virtual FsError rename(std::string_view old_relative_path,
                                       std::string_view new_relative_path,
                                       bool replace_existing) = 0;
  [[nodiscard]] virtual FsError disk_space(DiskSpace& out_space) const = 0;

 protected:
  std::string mount_point_{};
  bool read_only_{};
};

}  // namespace xenon::filesystem
