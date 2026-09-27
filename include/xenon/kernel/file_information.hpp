#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

#include "xenon/filesystem/types.hpp"
#include "xenon/kernel/io_types.hpp"

namespace xenon::kernel {

enum class FileInformationClass : std::uint8_t {
  Basic = 0,
  Standard,
  Internal,
  Position,
  Name,
  NetworkOpen,
  Mode,
  Alignment,
  Disposition,
  Allocation,
  EndOfFile,
  Rename,
  Completion,
};

struct FileBasicInformation {
  std::filesystem::file_time_type last_write_time{};
  std::uint32_t attributes{filesystem::FileAttributeNone};
  bool set_last_write_time{};
  bool set_attributes{};
};

struct FileStandardInformation {
  std::uint64_t allocation_size{};
  std::uint64_t end_of_file{};
  std::uint32_t number_of_links{1};
  bool delete_pending{};
  bool directory{};
};

struct FileInternalInformation {
  std::uint64_t index_number{};
};

struct FilePositionInformation {
  std::uint64_t current_byte_offset{};
};

struct FileNameInformation {
  std::string name{};
};

struct FileNetworkOpenInformation {
  filesystem::FileInfo file{};
};

struct FileModeInformation {
  bool synchronous{};
};

struct FileAlignmentInformation {
  std::uint32_t alignment_requirement{};
};

struct FileDispositionInformation {
  bool delete_file{};
};

struct FileAllocationInformation {
  std::uint64_t allocation_size{};
};

struct FileEndOfFileInformation {
  std::uint64_t end_of_file{};
};

struct FileRenameInformation {
  bool replace_if_exists{};
  Handle root_directory{kInvalidHandle};
  std::string file_name{};
};

struct FileCompletionInformation {
  Handle completion_port{kInvalidHandle};
  std::uint64_t key{};
};

using FileInformation = std::variant<
    FileBasicInformation, FileStandardInformation, FileInternalInformation,
    FilePositionInformation, FileNameInformation, FileNetworkOpenInformation,
    FileModeInformation, FileAlignmentInformation, FileDispositionInformation,
    FileAllocationInformation, FileEndOfFileInformation, FileRenameInformation,
    FileCompletionInformation>;

struct VolumeInformation {
  std::string name{};
  bool read_only{};
  std::uint32_t filesystem_attributes{};
  std::uint32_t component_name_max_length{255};
  std::uint32_t sectors_per_allocation_unit{1};
  std::uint32_t bytes_per_sector{0x200};
  filesystem::DiskSpace space{};
};

}  // namespace xenon::kernel
