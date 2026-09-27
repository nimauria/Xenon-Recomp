#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "xenon/filesystem/types.hpp"

namespace xenon::filesystem {

class FileHandle {
 public:
  virtual ~FileHandle() = default;

  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  [[nodiscard]] virtual FsError read(std::span<std::byte> destination,
                                     std::size_t& bytes_read) = 0;
  [[nodiscard]] virtual FsError write(std::span<const std::byte> source,
                                      std::size_t& bytes_written) = 0;
  [[nodiscard]] virtual FsError read_at(std::uint64_t offset,
                                        std::span<std::byte> destination,
                                        std::size_t& bytes_read) = 0;
  [[nodiscard]] virtual FsError write_at(std::uint64_t offset,
                                         std::span<const std::byte> source,
                                         std::size_t& bytes_written) = 0;
  [[nodiscard]] virtual FsError seek(std::int64_t offset, SeekOrigin origin,
                                     std::uint64_t& new_position) = 0;
  [[nodiscard]] virtual std::uint64_t tell() const noexcept = 0;
  [[nodiscard]] virtual FsError size(std::uint64_t& out_size) const = 0;
  [[nodiscard]] virtual FsError resize(std::uint64_t new_size) = 0;
  [[nodiscard]] virtual FsError flush() = 0;

 protected:
  FileHandle() = default;
};

}  // namespace xenon::filesystem
