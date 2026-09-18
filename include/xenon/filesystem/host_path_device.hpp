#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "xenon/filesystem/device.hpp"

namespace xenon::filesystem {

struct HostOpenRegistry;

struct HostPathDeviceOptions {
  bool read_only{true};
  bool create_root{false};
  bool case_insensitive{true};
};

// Maps one Xbox/VFS device path to a host directory. Guest path traversal is
// rejected, and resolved host paths are kept beneath the canonical mount root.
class HostPathDevice final : public Device {
 public:
  HostPathDevice(std::string mount_point, std::filesystem::path host_root,
                 HostPathDeviceOptions options = {});
  ~HostPathDevice() override;

  [[nodiscard]] FsError initialize() override;
  [[nodiscard]] FsError stat(std::string_view relative_path,
                             FileInfo& out_info) const override;
  [[nodiscard]] FsError open(std::string_view relative_path,
                             const OpenOptions& options,
                             std::unique_ptr<FileHandle>& out_file,
                             OpenAction* out_action = nullptr) override;
  [[nodiscard]] FsError list(
      std::string_view relative_path,
      std::vector<DirectoryEntry>& out_entries) const override;
  [[nodiscard]] FsError create_directory(std::string_view relative_path,
                                         bool recursive) override;
  [[nodiscard]] FsError remove(std::string_view relative_path) override;
  [[nodiscard]] FsError rename(std::string_view old_relative_path,
                               std::string_view new_relative_path,
                               bool replace_existing) override;
  [[nodiscard]] FsError disk_space(DiskSpace& out_space) const override;

  [[nodiscard]] const std::filesystem::path& host_root() const noexcept {
    return root_;
  }

 private:
  [[nodiscard]] FsError resolve_existing(std::string_view relative_path,
                                         std::filesystem::path& out_path) const;
  [[nodiscard]] FsError resolve_for_creation(
      std::string_view relative_path, std::filesystem::path& out_path,
      bool allow_existing) const;
  [[nodiscard]] bool path_is_within_root(
      const std::filesystem::path& path) const;

  std::filesystem::path configured_root_{};
  std::filesystem::path root_{};
  HostPathDeviceOptions options_{};
  std::shared_ptr<HostOpenRegistry> open_registry_{};
  bool initialized_{};
};

}  // namespace xenon::filesystem
