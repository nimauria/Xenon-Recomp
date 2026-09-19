#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "xenon/kernel/io_manager.hpp"

namespace xenon::kernel::xbox {

using Status = std::uint32_t;

namespace status {
constexpr Status Success = 0x00000000u;
constexpr Status Timeout = 0x00000102u;
constexpr Status Pending = 0x00000103u;
constexpr Status BufferOverflow = 0x80000005u;
constexpr Status NoMoreFiles = 0x80000006u;
constexpr Status Unsuccessful = 0xC0000001u;
constexpr Status NotImplemented = 0xC0000002u;
constexpr Status InvalidInfoClass = 0xC0000003u;
constexpr Status InfoLengthMismatch = 0xC0000004u;
constexpr Status AccessViolation = 0xC0000005u;
constexpr Status InvalidHandle = 0xC0000008u;
constexpr Status InvalidParameter = 0xC000000Du;
constexpr Status NoSuchFile = 0xC000000Fu;
constexpr Status EndOfFile = 0xC0000011u;
constexpr Status AccessDenied = 0xC0000022u;
constexpr Status BufferTooSmall = 0xC0000023u;
constexpr Status ObjectTypeMismatch = 0xC0000024u;
constexpr Status ObjectNameInvalid = 0xC0000033u;
constexpr Status ObjectNameNotFound = 0xC0000034u;
constexpr Status ObjectNameCollision = 0xC0000035u;
constexpr Status SharingViolation = 0xC0000043u;
constexpr Status DeletePending = 0xC0000056u;
constexpr Status InsufficientResources = 0xC000009Au;
constexpr Status FileIsDirectory = 0xC00000BAu;
constexpr Status NotSupported = 0xC00000BBu;
constexpr Status NotSameDevice = 0xC00000D4u;
constexpr Status DirectoryNotEmpty = 0xC0000101u;
constexpr Status NotADirectory = 0xC0000103u;
constexpr Status Cancelled = 0xC0000120u;
}  // namespace status

namespace access {
constexpr std::uint32_t FileReadData = 0x00000001u;
constexpr std::uint32_t FileWriteData = 0x00000002u;
constexpr std::uint32_t FileAppendData = 0x00000004u;
constexpr std::uint32_t FileExecute = 0x00000020u;
constexpr std::uint32_t FileDeleteChild = 0x00000040u;
constexpr std::uint32_t FileReadAttributes = 0x00000080u;
constexpr std::uint32_t FileWriteAttributes = 0x00000100u;
constexpr std::uint32_t Delete = 0x00010000u;
constexpr std::uint32_t Synchronize = 0x00100000u;
constexpr std::uint32_t GenericAll = 0x10000000u;
constexpr std::uint32_t GenericExecute = 0x20000000u;
constexpr std::uint32_t GenericWrite = 0x40000000u;
constexpr std::uint32_t GenericRead = 0x80000000u;
}  // namespace access

namespace share {
constexpr std::uint32_t Read = 0x1u;
constexpr std::uint32_t Write = 0x2u;
constexpr std::uint32_t Delete = 0x4u;
constexpr std::uint32_t All = Read | Write | Delete;
}  // namespace share

namespace create_option {
constexpr std::uint32_t DirectoryFile = 0x00000001u;
constexpr std::uint32_t WriteThrough = 0x00000002u;
constexpr std::uint32_t SequentialOnly = 0x00000004u;
constexpr std::uint32_t NoIntermediateBuffering = 0x00000008u;
constexpr std::uint32_t SynchronousIoAlert = 0x00000010u;
constexpr std::uint32_t SynchronousIoNonAlert = 0x00000020u;
constexpr std::uint32_t NonDirectoryFile = 0x00000040u;
constexpr std::uint32_t RandomAccess = 0x00000800u;
constexpr std::uint32_t DeleteOnClose = 0x00001000u;
}  // namespace create_option

constexpr Handle ObDosDevices = 0xFFFFFFFDu;

// Values written to IO_STATUS_BLOCK.Information by NtCreateFile/NtOpenFile.
enum class FileAction : std::uint32_t {
  Superseded = 0,
  Opened = 1,
  Created = 2,
  Overwritten = 3,
  Exists = 4,
  DoesNotExist = 5,
};

enum class FileInformationClass : std::uint32_t {
  Directory = 1,
  FullDirectory = 2,
  BothDirectory = 3,
  Basic = 4,
  Standard = 5,
  Internal = 6,
  Ea = 7,
  Access = 8,
  Name = 9,
  Rename = 10,
  Link = 11,
  Names = 12,
  Disposition = 13,
  Position = 14,
  FullEa = 15,
  Mode = 16,
  Alignment = 17,
  All = 18,
  Allocation = 19,
  EndOfFile = 20,
  MountPartition = 23,
  Sector = 28,
  XctdCompression = 29,
  Completion = 31,
  IoPriority = 33,
  NetworkOpen = 34,
};

enum class FsInformationClass : std::uint32_t {
  Volume = 1,
  Size = 3,
  Device = 4,
  Attribute = 5,
};

struct IoStatusBlock {
  Status status{status::Success};
  std::uint32_t information{};
};

struct CreateFileRequest {
  Handle root_directory{kInvalidHandle};
  std::string path{};
  std::uint32_t desired_access{access::GenericRead};
  std::uint32_t file_attributes{};
  std::uint32_t share_access{share::All};
  std::uint32_t creation_disposition{1};
  std::uint32_t create_options{};
  HandleFlags handle_flags{HandleFlags::None};
};

struct FsVolumeInformation {
  std::uint64_t creation_time{};
  std::uint32_t serial_number{};
  bool supports_objects{};
  std::string label{};
};

struct FsSizeInformation {
  std::uint64_t total_allocation_units{};
  std::uint64_t available_allocation_units{};
  std::uint32_t sectors_per_allocation_unit{1};
  std::uint32_t bytes_per_sector{512};
};

struct FsDeviceInformation {
  std::uint32_t device_type{0x22};  // FILE_DEVICE_UNKNOWN
  std::uint32_t characteristics{};
};

struct FsAttributeInformation {
  std::uint32_t attributes{};
  std::int32_t component_name_max_length{255};
  std::string name{};
};

using VolumeInformation = std::variant<FsVolumeInformation, FsSizeInformation,
                                       FsDeviceInformation, FsAttributeInformation>;

struct DirectoryInformation {
  std::uint32_t file_index{};
  filesystem::DirectoryEntry entry{};
};

[[nodiscard]] Status map_status(const IoStatus& value) noexcept;
[[nodiscard]] Status map_status(KernelIoCode value) noexcept;
[[nodiscard]] bool valid_path(std::string_view path, bool pattern) noexcept;

class IoFacade {
 public:
  explicit IoFacade(KernelIoManager& io) : io_(io) {}

  [[nodiscard]] Status create_file(const CreateFileRequest& request,
                                   Handle& out_handle, IoStatusBlock& iosb);
  [[nodiscard]] Status open_file(const CreateFileRequest& request,
                                 Handle& out_handle, IoStatusBlock& iosb);
  [[nodiscard]] Status read_file(Handle handle, std::span<std::byte> destination,
                                 std::optional<std::uint64_t> byte_offset,
                                 IoStatusBlock& iosb, Handle event = kInvalidHandle,
                                 std::uint64_t context = 0);
  [[nodiscard]] Status write_file(Handle handle,
                                  std::span<const std::byte> source,
                                  std::optional<std::uint64_t> byte_offset,
                                  IoStatusBlock& iosb, Handle event = kInvalidHandle,
                                  std::uint64_t context = 0);
  [[nodiscard]] Status flush_buffers_file(Handle handle, IoStatusBlock& iosb,
                                          std::uint64_t context = 0);
  [[nodiscard]] Status query_directory_file(
      Handle handle, std::string_view pattern, bool restart_scan,
      std::size_t max_entries, std::vector<DirectoryInformation>& out,
      IoStatusBlock& iosb, Handle event = kInvalidHandle,
      std::uint64_t context = 0);
  [[nodiscard]] Status query_information_file(Handle handle,
                                              std::uint32_t info_class,
                                              FileInformation& out,
                                              IoStatusBlock& iosb);
  [[nodiscard]] Status set_information_file(Handle handle,
                                            std::uint32_t info_class,
                                            const FileInformation& value,
                                            IoStatusBlock& iosb,
                                            std::uint64_t context = 0);
  [[nodiscard]] Status query_volume_information_file(
      Handle handle, std::uint32_t info_class, VolumeInformation& out,
      IoStatusBlock& iosb);
  [[nodiscard]] Status query_full_attributes_file(
      Handle root_directory, std::string_view path,
      FileNetworkOpenInformation& out, IoStatusBlock& iosb);

 private:
  [[nodiscard]] Status signal_event_if_present(Handle event, Status current);
  [[nodiscard]] Status finish_io_status(const IoStatus& result,
                                        IoStatusBlock& iosb,
                                        bool asynchronous_handle,
                                        Handle event);
  [[nodiscard]] static bool decode_open_options(
      const CreateFileRequest& request, KernelOpenOptions& out_options,
      Status& out_error);
  [[nodiscard]] static bool decode_file_information_class(
      std::uint32_t raw, ::xenon::kernel::FileInformationClass& out_class,
      bool for_set) noexcept;

  KernelIoManager& io_;
  std::uint32_t directory_index_{};
};

}  // namespace xenon::kernel::xbox
