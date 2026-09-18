#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
  SharingViolation,
};

[[nodiscard]] constexpr bool succeeded(FsError error) noexcept {
  return error == FsError::None;
}

[[nodiscard]] std::string_view to_string(FsError error) noexcept;

enum class FileAccess : std::uint8_t {
  None = 0,
  Read = 1u << 0u,
  Write = 1u << 1u,
  Delete = 1u << 2u,
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

enum class ShareAccess : std::uint8_t {
  None = 0,
  Read = 1u << 0u,
  Write = 1u << 1u,
  Delete = 1u << 2u,
  All = (1u << 0u) | (1u << 1u) | (1u << 2u),
};

[[nodiscard]] constexpr ShareAccess operator|(ShareAccess lhs,
                                              ShareAccess rhs) noexcept {
  return static_cast<ShareAccess>(static_cast<std::uint8_t>(lhs) |
                                  static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr ShareAccess operator&(ShareAccess lhs,
                                              ShareAccess rhs) noexcept {
  return static_cast<ShareAccess>(static_cast<std::uint8_t>(lhs) &
                                  static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_share(ShareAccess value,
                                       ShareAccess flag) noexcept {
  return (value & flag) != ShareAccess::None;
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

enum class OpenAction : std::uint8_t {
  None = 0,
  Superseded,
  Opened,
  Created,
  Overwritten,
};

enum class SeekOrigin : std::uint8_t {
  Begin,
  Current,
  End,
};

enum FileAttribute : std::uint32_t {
  FileAttributeNone = 0x0000,
  FileAttributeReadOnly = 0x0001,
  FileAttributeHidden = 0x0002,
  FileAttributeSystem = 0x0004,
  FileAttributeDirectory = 0x0010,
  FileAttributeArchive = 0x0020,
  FileAttributeDevice = 0x0040,
  FileAttributeNormal = 0x0080,
  FileAttributeTemporary = 0x0100,
  FileAttributeCompressed = 0x0800,
  FileAttributeEncrypted = 0x4000,
};

struct OpenOptions {
  FileAccess access{FileAccess::Read};
  ShareAccess share{ShareAccess::All};
  CreateDisposition disposition{CreateDisposition::Open};
};

struct FileInfo {
  std::uint64_t size{};
  std::uint64_t allocation_size{};
  std::uint32_t attributes{FileAttributeNone};
  bool is_directory{};
  bool read_only{};
  std::filesystem::file_time_type last_write_time{};
};

struct DirectoryEntry {
  std::string name{};
  FileInfo info{};
};

struct DirectoryQuery {
  std::string pattern{"*"};
  bool include_files{true};
  bool include_directories{true};
  std::size_t max_entries{};  // 0 means no limit.
};

struct DiskSpace {
  std::uint64_t capacity{};
  std::uint64_t free{};
  std::uint64_t available{};
};

struct MountInfo {
  std::string mount_point{};
  bool read_only{};
};

struct SymbolicLinkInfo {
  std::string alias{};
  std::string target{};
};

}  // namespace xenon::filesystem
