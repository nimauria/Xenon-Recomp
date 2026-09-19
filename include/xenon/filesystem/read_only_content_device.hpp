#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "xenon/filesystem/device.hpp"

namespace xenon::filesystem {

// A format-neutral random-access source for immutable Xbox content. Future
// GDFX/ISO/container implementations live behind this interface; the VFS and
// launcher never need to know the on-disc/container format.
class ReadOnlyContentSource {
 public:
  virtual ~ReadOnlyContentSource() = default;

  [[nodiscard]] virtual FsError initialize() = 0;
  [[nodiscard]] virtual FsError stat(std::string_view relative_path,
                                     FileInfo& out_info) const = 0;
  [[nodiscard]] virtual FsError list(
      std::string_view relative_path,
      std::vector<DirectoryEntry>& out_entries) const = 0;
  [[nodiscard]] virtual FsError read_at(std::string_view relative_path,
                                        std::uint64_t offset,
                                        std::span<std::byte> destination,
                                        std::size_t& bytes_read) const = 0;
  [[nodiscard]] virtual FsError disk_space(DiskSpace& out_space) const = 0;
};

class ReadOnlyContentDevice final : public Device {
 public:
  ReadOnlyContentDevice(std::string mount_point,
                        std::shared_ptr<ReadOnlyContentSource> source);
  ~ReadOnlyContentDevice() override;

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

  [[nodiscard]] const std::shared_ptr<ReadOnlyContentSource>& source() const noexcept {
    return source_;
  }

 private:
  [[nodiscard]] FsError normalize_relative(std::string_view relative_path,
                                           std::string& out_path) const;

  std::shared_ptr<ReadOnlyContentSource> source_{};
  bool initialized_{};
};

}  // namespace xenon::filesystem
