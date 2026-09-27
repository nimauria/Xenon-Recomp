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

// Reads the entire file at `relative_path` out of `source` into `out_bytes`,
// without ever writing anything to a temporary host file. This is the
// primitive that lets a caller (title-update application, the recompilation
// pipeline) obtain a full in-memory `default.xex`/patch file straight out of
// a mounted disc image or package - `XEX Loader V2` only ever consumes
// `std::span<const std::byte>`, so this is the sole piece needed to bridge
// "file lives inside an ISO/STFS container" to "bytes XEX Loader V2 accepts".
[[nodiscard]] FsError read_all(const ReadOnlyContentSource& source,
                               std::string_view relative_path,
                               std::vector<std::byte>& out_bytes);

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
