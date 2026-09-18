#pragma once

#include "xenon/filesystem/device.hpp"

namespace xenon::filesystem {

// A deliberately empty read-only device. Useful for Xbox paths that a title
// probes opportunistically but that Xenon does not want to map onto host data.
class NullDevice final : public Device {
 public:
  explicit NullDevice(std::string mount_point);

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
};

}  // namespace xenon::filesystem
