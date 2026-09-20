#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "xenon/filesystem/types.hpp"

namespace xenon::kernel {

using Handle = std::uint32_t;
constexpr Handle kInvalidHandle = 0;

enum class ObjectType : std::uint8_t {
  Unknown = 0,
  File,
  Event,
  IoCompletionPort,
  Thread,
  Semaphore,
  Mutant,
  Timer,
  Process,
  Module,
};

enum class HandleFlags : std::uint8_t {
  None = 0,
  Inherit = 1u << 0u,
  ProtectFromClose = 1u << 1u,
};

[[nodiscard]] constexpr HandleFlags operator|(HandleFlags lhs,
                                               HandleFlags rhs) noexcept {
  return static_cast<HandleFlags>(static_cast<std::uint8_t>(lhs) |
                                  static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr HandleFlags operator&(HandleFlags lhs,
                                               HandleFlags rhs) noexcept {
  return static_cast<HandleFlags>(static_cast<std::uint8_t>(lhs) &
                                  static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_flag(HandleFlags value,
                                      HandleFlags flag) noexcept {
  return (value & flag) != HandleFlags::None;
}

enum class KernelIoCode : std::uint8_t {
  Success = 0,
  Pending,
  Cancelled,
  Timeout,
  InvalidHandle,
  InvalidObjectType,
  InvalidParameter,
  AccessDenied,
  ProtectedHandle,
  NotFound,
  AlreadyExists,
  NotDirectory,
  IsDirectory,
  DirectoryNotEmpty,
  SharingViolation,
  Unsupported,
  CrossDevice,
  NoMoreFiles,
  DeletePending,
  FilesystemError,
};

[[nodiscard]] std::string_view to_string(KernelIoCode code) noexcept;

struct IoStatus {
  KernelIoCode code{KernelIoCode::Success};
  filesystem::FsError filesystem_error{filesystem::FsError::None};
  std::size_t information{};
  std::uint64_t request_id{};

  [[nodiscard]] constexpr bool succeeded() const noexcept {
    return code == KernelIoCode::Success;
  }
};

[[nodiscard]] IoStatus from_filesystem_error(filesystem::FsError error,
                                             std::size_t information = 0) noexcept;

enum class IoOperation : std::uint8_t {
  Read = 0,
  Write,
  Flush,
  Resize,
  QueryDirectory,
  QueryInformation,
  SetInformation,
  Other,
};

enum class IoRequestState : std::uint8_t {
  Pending = 0,
  Completed,
  Cancelled,
};

enum class OpenKind : std::uint8_t {
  Any = 0,
  File,
  Directory,
};

struct KernelOpenOptions {
  filesystem::OpenOptions file{};
  OpenKind kind{OpenKind::Any};
  bool synchronous{true};
  bool delete_on_close{false};
  HandleFlags handle_flags{HandleFlags::None};
};

struct DuplicateHandleOptions {
  bool same_access{true};
  std::uint32_t desired_access{};
  HandleFlags flags{HandleFlags::None};
};

}  // namespace xenon::kernel
