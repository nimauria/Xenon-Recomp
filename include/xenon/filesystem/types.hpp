#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xenon::filesystem {

enum class FsError : std::uint8_t {
  None = 0,
  InvalidPath,
  NotFound,
  AlreadyExists,
  AccessDenied,
  ReadOnly,
  NotDirectory,
  IsDirectory,
  DirectoryNotEmpty,
  InvalidArgument,
  IoError,
  Unsupported,
  CrossDevice,
  TooManyLinks,
};

[[nodiscard]] constexpr bool succeeded(FsError error) noexcept {
  return error == FsError::None;
}

[[nodiscard]] std::string_view to_string(FsError error) noexcept;

enum class FileAccess : std::uint8_t {
  None = 0,
  Read = 1u << 0u,
  Write = 1u << 1u,
};

[[nodiscard]] constexpr FileAccess operator|(FileAccess lhs,
                                             FileAccess rhs) noexcept {
  return static_cast<FileAccess>(static_cast<std::uint8_t>(lhs) |
                                 static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr FileAccess operator&(FileAccess lhs,
                                             FileAccess rhs) noexcept {
  return static_cast<FileAccess>(static_cast<std::uint8_t>(lhs) &
                                 static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_access(FileAccess value,
                                        FileAccess flag) noexcept {
  return (value & flag) != FileAccess::None;
}

// Mirrors the Xbox/NT create-disposition model so the later kernel bridge can
// forward semantics without translating through host-specific Win32 modes.
enum class CreateDisposition : std::uint8_t {
  Supersede,    // Replace if present, create otherwise.
  Open,         // Open only if present.
  Create,       // Create only if absent.
  OpenIf,       // Open if present, create otherwise.
  Overwrite,    // Truncate only if present.
  OverwriteIf,  // Truncate if present, create otherwise.
};

enum class SeekOrigin : std::uint8_t {
  Begin,
  Current,
  End,
};

struct OpenOptions {
  FileAccess access{FileAccess::Read};
  CreateDisposition disposition{CreateDisposition::Open};
};

struct FileInfo {
  std::uint64_t size{};
  bool is_directory{};
  bool read_only{};
  std::filesystem::file_time_type last_write_time{};
};

struct DirectoryEntry {
  std::string name{};
  FileInfo info{};
};

struct DiskSpace {
  std::uint64_t capacity{};
  std::uint64_t free{};
  std::uint64_t available{};
};

}  // namespace xenon::filesystem
