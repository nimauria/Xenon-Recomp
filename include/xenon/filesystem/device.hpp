#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/filesystem/file.hpp"
#include "xenon/filesystem/types.hpp"

namespace xenon::filesystem {

class DirectoryCursor;

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
  // Xbox FILE_FS_* queries expect filesystem/device geometry even for virtual
  // devices. Backends may override these when a format has more specific
  // semantics; defaults match the values used by Xenia/ReXGlue for generic
  // Xbox storage.
  [[nodiscard]] virtual std::string_view filesystem_name() const noexcept {
    return "XENON";
  }
  [[nodiscard]] virtual std::uint32_t filesystem_attributes() const noexcept {
    return 0;
  }
  [[nodiscard]] virtual std::uint32_t component_name_max_length() const noexcept {
    return 255;
  }
  [[nodiscard]] virtual std::uint32_t sectors_per_allocation_unit() const noexcept {
    return 1;
  }
  [[nodiscard]] virtual std::uint32_t bytes_per_sector() const noexcept {
    return 0x200;
  }

  [[nodiscard]] virtual FsError initialize() = 0;
  [[nodiscard]] virtual FsError stat(std::string_view relative_path,
                                     FileInfo& out_info) const = 0;
  [[nodiscard]] virtual FsError open(std::string_view relative_path,
                                     const OpenOptions& options,
                                     std::unique_ptr<FileHandle>& out_file,
                                     OpenAction* out_action = nullptr) = 0;
  [[nodiscard]] virtual FsError list(
      std::string_view relative_path,
      std::vector<DirectoryEntry>& out_entries) const = 0;
  [[nodiscard]] virtual FsError query_directory(
      std::string_view relative_path, const DirectoryQuery& query,
      std::vector<DirectoryEntry>& out_entries) const;
  [[nodiscard]] virtual FsError open_directory_cursor(
      std::string_view relative_path, const DirectoryQuery& query,
      std::unique_ptr<DirectoryCursor>& out_cursor) const;
  [[nodiscard]] virtual FsError create_directory(std::string_view relative_path,
                                                 bool recursive) = 0;
  [[nodiscard]] virtual FsError remove(std::string_view relative_path) = 0;
  [[nodiscard]] virtual FsError rename(std::string_view old_relative_path,
                                       std::string_view new_relative_path,
                                       bool replace_existing) = 0;
  [[nodiscard]] virtual FsError set_attributes(
      std::string_view relative_path, const FileAttributeUpdate& update);
  [[nodiscard]] virtual FsError set_last_write_time(
      std::string_view relative_path,
      std::filesystem::file_time_type last_write_time);
  [[nodiscard]] virtual FsError disk_space(DiskSpace& out_space) const = 0;

 protected:
  std::string mount_point_{};
  bool read_only_{};
};

}  // namespace xenon::filesystem
