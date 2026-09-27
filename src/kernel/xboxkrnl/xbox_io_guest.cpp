#include "xenon/kernel/xbox_io_guest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "xenon/memory/fault.hpp"

namespace xenon::kernel::xbox {
namespace {

constexpr std::uint64_t kWindowsEpochToUnixSeconds = 11644473600ull;
constexpr std::uint64_t kTicksPerSecond = 10000000ull;
constexpr std::uint32_t kDirectoryInfoBaseSize = 0x40u;
constexpr std::uint32_t kNetworkOpenInfoSize = 56u;
constexpr std::uint32_t kStandardInfoSize = 24u;
constexpr std::uint32_t kMaxTransferBytes = 256u * 1024u * 1024u;

[[nodiscard]] constexpr std::uint32_t align_up(std::uint32_t value,
                                                std::uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

[[nodiscard]] std::uint32_t minimum_query_file_information_length(
    std::uint32_t info_class) noexcept {
  switch (static_cast<FileInformationClass>(info_class)) {
    case FileInformationClass::Basic: return 40;
    case FileInformationClass::Standard: return kStandardInfoSize;
    case FileInformationClass::Internal: return 8;
    case FileInformationClass::Name: return 8;
    case FileInformationClass::Position: return 8;
    case FileInformationClass::Mode: return 4;
    case FileInformationClass::Alignment: return 4;
    case FileInformationClass::NetworkOpen: return kNetworkOpenInfoSize;
    default: return 0;
  }
}

[[nodiscard]] std::uint32_t minimum_set_file_information_length(
    std::uint32_t info_class) noexcept {
  switch (static_cast<FileInformationClass>(info_class)) {
    case FileInformationClass::Basic: return 40;
    case FileInformationClass::Rename: return 16;
    case FileInformationClass::Disposition: return 1;
    case FileInformationClass::Position: return 8;
    case FileInformationClass::Allocation: return 8;
    case FileInformationClass::EndOfFile: return 8;
    case FileInformationClass::Completion: return 8;
    default: return 0;
  }
}

[[nodiscard]] std::uint32_t minimum_volume_information_length(
    std::uint32_t info_class) noexcept {
  switch (static_cast<FsInformationClass>(info_class)) {
    case FsInformationClass::Volume: return 24;
    case FsInformationClass::Size: return 24;
    case FsInformationClass::Device: return 8;
    case FsInformationClass::Attribute: return 16;
    default: return 0;
  }
}

}  // namespace

bool GuestIoBridge::range_has_access(GuestAddress address, std::uint32_t size,
                                     memory::Protect access) const noexcept {
  if (size == 0) return true;
  const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
  if (end > 0x100000000ull) return false;

  std::uint64_t current = address;
  while (current < end) {
    const auto info = memory_.query(static_cast<GuestAddress>(current));
    if (!info || info->state != memory::PageState::Committed ||
        !memory::has(info->current_protect, access)) {
      return false;
    }
    const auto next_page = (current & ~std::uint64_t{0xFFF}) + 0x1000ull;
    current = std::min<std::uint64_t>(next_page, end);
  }
  return true;
}

bool GuestIoBridge::readable_range(GuestAddress address,
                                   std::uint32_t size) const noexcept {
  return range_has_access(address, size, memory::Protect::Read);
}

bool GuestIoBridge::writable_range(GuestAddress address,
                                   std::uint32_t size) const noexcept {
  return range_has_access(address, size, memory::Protect::Write);
}

bool GuestIoBridge::read_ansi_string(GuestAddress descriptor,
                                     std::string& out) const {
  out.clear();
  if (!descriptor || !readable_range(descriptor, 8)) return false;
  try {
    const auto length = memory_.read16_be(descriptor + 0);
    const auto maximum_length = memory_.read16_be(descriptor + 2);
    const auto pointer = memory_.read32_be(descriptor + 4);
    if (length > maximum_length) return false;
    if (length == 0) return true;
    if (!pointer || !readable_range(pointer, length)) return false;
    std::vector<std::byte> bytes(length);
    memory_.read_bytes(pointer, bytes);
    out.resize(length);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      out[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[i]));
    }
    return true;
  } catch (const memory::MemoryFault&) {
    return false;
  }
}

bool GuestIoBridge::read_object_attributes(GuestAddress address,
                                           ObjectAttributes& out,
                                           std::string& out_name) const {
  if (!address || !readable_range(address, 12)) return false;
  try {
    out.root_directory = memory_.read32_be(address + 0);
    out.name = memory_.read32_be(address + 4);
    out.attributes = memory_.read32_be(address + 8);
  } catch (const memory::MemoryFault&) {
    return false;
  }
  if (!out.name) return false;
  return read_ansi_string(out.name, out_name);
}

bool GuestIoBridge::read_optional_offset(
    GuestAddress address, std::optional<std::uint64_t>& out) const {
  out.reset();
  if (!address) return true;
  if (!readable_range(address, 8)) return false;
  try {
    const auto value = memory_.read64_be(address);
    if (value != std::numeric_limits<std::uint64_t>::max()) out = value;
    return true;
  } catch (const memory::MemoryFault&) {
    return false;
  }
}

bool GuestIoBridge::write_iosb(GuestAddress address,
                               const IoStatusBlock& iosb) noexcept {
  if (!address) return true;
  if (!writable_range(address, 8)) return false;
  try {
    memory_.write32_be(address + 0, iosb.status);
    memory_.write32_be(address + 4, iosb.information);
    return true;
  } catch (const memory::MemoryFault&) {
    return false;
  }
}

bool GuestIoBridge::write_status_result(GuestAddress iosb_address,
                                        Status,
                                        const IoStatusBlock& iosb) noexcept {
  return write_iosb(iosb_address, iosb);
}

void GuestIoBridge::queue_apc(GuestAddress routine, GuestAddress context,
                              GuestAddress iosb_address,
                              const IoStatusBlock& iosb) const {
  if (!apc_queue_ || (routine & ~1u) == 0 || context == 0 ||
      iosb.status != status::Success) {
    return;
  }
  apc_queue_(routine & ~1u, context, iosb_address);
}

Status GuestIoBridge::nt_create_file(
    GuestAddress handle_out, std::uint32_t desired_access,
    GuestAddress object_attributes, GuestAddress io_status_block,
    GuestAddress allocation_size, std::uint32_t file_attributes,
    std::uint32_t share_access, std::uint32_t creation_disposition,
    std::uint32_t create_options) {
  if (!handle_out || !writable_range(handle_out, 4) || !object_attributes) {
    return status::InvalidParameter;
  }
  if (allocation_size && !readable_range(allocation_size, 8)) {
    return status::AccessViolation;
  }

  ObjectAttributes attrs{};
  std::string path;
  if (!read_object_attributes(object_attributes, attrs, path)) {
    return status::AccessViolation;
  }

  CreateFileRequest request{};
  request.root_directory = attrs.root_directory;
  request.path = std::move(path);
  request.desired_access = desired_access;
  request.file_attributes = file_attributes;
  request.share_access = share_access;
  request.creation_disposition = creation_disposition;
  request.create_options = create_options;

  Handle handle = kInvalidHandle;
  IoStatusBlock iosb{};
  const auto result = facade_.create_file(request, handle, iosb);
  try {
    memory_.write32_be(handle_out, handle);
  } catch (const memory::MemoryFault&) {
    if (handle != kInvalidHandle) (void)io_.close(handle, true);
    return status::AccessViolation;
  }
  if (!write_iosb(io_status_block, iosb)) {
    if (handle != kInvalidHandle) (void)io_.close(handle, true);
    return status::AccessViolation;
  }
  return result;
}

Status GuestIoBridge::nt_open_file(
    GuestAddress handle_out, std::uint32_t desired_access,
    GuestAddress object_attributes, GuestAddress io_status_block,
    std::uint32_t open_options) {
  return nt_create_file(handle_out, desired_access, object_attributes,
                        io_status_block, 0, 0, 0, 1, open_options);
}

Status GuestIoBridge::nt_read_file(
    Handle file_handle, Handle event_handle, GuestAddress apc_routine,
    GuestAddress apc_context, GuestAddress io_status_block,
    GuestAddress buffer, std::uint32_t buffer_length,
    GuestAddress byte_offset) {
  if (buffer_length > kMaxTransferBytes) return status::InsufficientResources;
  if (buffer_length && (!buffer || !writable_range(buffer, buffer_length))) {
    return status::AccessViolation;
  }
  std::optional<std::uint64_t> offset;
  if (!read_optional_offset(byte_offset, offset)) return status::AccessViolation;

  std::vector<std::byte> bytes;
  try {
    bytes.resize(buffer_length);
  } catch (const std::bad_alloc&) {
    return status::InsufficientResources;
  }

  IoStatusBlock iosb{};
  const auto result = facade_.read_file(file_handle, bytes, offset, iosb,
                                        event_handle, apc_context);
  if ((iosb.status == status::Success || result == status::Pending) &&
      iosb.information != 0) {
    try {
      memory_.write_bytes(buffer,
                          std::span<const std::byte>(bytes.data(), iosb.information));
    } catch (const memory::MemoryFault&) {
      iosb = {status::AccessViolation, 0};
      (void)write_iosb(io_status_block, iosb);
      return status::AccessViolation;
    }
  }
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  queue_apc(apc_routine, apc_context, io_status_block, iosb);
  return result;
}

Status GuestIoBridge::nt_read_file_scatter(
    Handle file_handle, Handle event_handle, GuestAddress apc_routine,
    GuestAddress apc_context, GuestAddress io_status_block,
    GuestAddress segment_array, std::uint32_t length,
    GuestAddress byte_offset) {
  constexpr std::uint32_t kSegmentSize = 4096;
  if (length > kMaxTransferBytes) return status::InsufficientResources;
  const std::uint32_t segment_count =
      length == 0 ? 0 : (length + kSegmentSize - 1u) / kSegmentSize;
  if (segment_count &&
      (!segment_array || !readable_range(segment_array, segment_count * 4u))) {
    return status::AccessViolation;
  }

  std::vector<GuestAddress> segments(segment_count);
  try {
    for (std::uint32_t i = 0; i < segment_count; ++i) {
      segments[i] = memory_.read32_be(segment_array + i * 4u);
      const auto chunk = std::min<std::uint32_t>(
          kSegmentSize, length - i * kSegmentSize);
      if (!segments[i] || !writable_range(segments[i], chunk)) {
        return status::AccessViolation;
      }
    }
  } catch (const memory::MemoryFault&) {
    return status::AccessViolation;
  }

  std::optional<std::uint64_t> offset;
  if (!read_optional_offset(byte_offset, offset)) return status::AccessViolation;
  std::vector<std::byte> bytes;
  try {
    bytes.resize(length);
  } catch (const std::bad_alloc&) {
    return status::InsufficientResources;
  }

  IoStatusBlock iosb{};
  const auto result = facade_.read_file(file_handle, bytes, offset, iosb,
                                        event_handle, apc_context);
  std::uint32_t remaining = iosb.information;
  std::uint32_t source_offset = 0;
  try {
    for (std::uint32_t i = 0; i < segment_count && remaining; ++i) {
      const auto chunk = std::min<std::uint32_t>(kSegmentSize, remaining);
      memory_.write_bytes(
          segments[i], std::span<const std::byte>(bytes.data() + source_offset, chunk));
      source_offset += chunk;
      remaining -= chunk;
    }
  } catch (const memory::MemoryFault&) {
    iosb = {status::AccessViolation, 0};
    (void)write_iosb(io_status_block, iosb);
    return status::AccessViolation;
  }
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  queue_apc(apc_routine, apc_context, io_status_block, iosb);
  return result;
}

Status GuestIoBridge::nt_write_file(
    Handle file_handle, Handle event_handle, GuestAddress apc_routine,
    GuestAddress apc_context, GuestAddress io_status_block,
    GuestAddress buffer, std::uint32_t buffer_length,
    GuestAddress byte_offset) {
  if (buffer_length > kMaxTransferBytes) return status::InsufficientResources;
  if (buffer_length && (!buffer || !readable_range(buffer, buffer_length))) {
    return status::AccessViolation;
  }
  std::optional<std::uint64_t> offset;
  if (!read_optional_offset(byte_offset, offset)) return status::AccessViolation;

  std::vector<std::byte> bytes;
  try {
    bytes.resize(buffer_length);
    if (buffer_length) memory_.read_bytes(buffer, bytes);
  } catch (const std::bad_alloc&) {
    return status::InsufficientResources;
  } catch (const memory::MemoryFault&) {
    return status::AccessViolation;
  }

  IoStatusBlock iosb{};
  const auto result = facade_.write_file(file_handle, bytes, offset, iosb,
                                         event_handle, apc_context);
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  queue_apc(apc_routine, apc_context, io_status_block, iosb);
  return result;
}

Status GuestIoBridge::nt_flush_buffers_file(Handle file_handle,
                                             GuestAddress io_status_block) {
  IoStatusBlock iosb{};
  const auto result = facade_.flush_buffers_file(file_handle, iosb);
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  return result;
}

std::uint64_t GuestIoBridge::xbox_time(
    std::filesystem::file_time_type value) noexcept {
  if (value == std::filesystem::file_time_type{}) return 0;
  const auto file_now = std::filesystem::file_time_type::clock::now();
  const auto sys_now = std::chrono::system_clock::now();
  const auto sys_delta =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          value - file_now);
  const auto sys_value = sys_now + sys_delta;
  const auto since_unix = std::chrono::duration_cast<std::chrono::nanoseconds>(
      sys_value.time_since_epoch());
  const auto unix_100ns = since_unix.count() / 100;
  const auto epoch_100ns =
      static_cast<std::int64_t>(kWindowsEpochToUnixSeconds * kTicksPerSecond);
  if (unix_100ns < -epoch_100ns) return 0;
  return static_cast<std::uint64_t>(unix_100ns + epoch_100ns);
}

std::filesystem::file_time_type GuestIoBridge::host_time(
    std::uint64_t value) noexcept {
  if (value == 0) return {};
  const auto epoch_100ns =
      static_cast<std::int64_t>(kWindowsEpochToUnixSeconds * kTicksPerSecond);
  const auto ticks = static_cast<std::int64_t>(value);
  const auto unix_100ns = ticks - epoch_100ns;
  const auto sys_duration =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds(unix_100ns * 100));
  const auto sys_value =
      std::chrono::system_clock::time_point(sys_duration);
  const auto file_now = std::filesystem::file_time_type::clock::now();
  const auto sys_now = std::chrono::system_clock::now();
  const auto file_delta =
      std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
          sys_value - sys_now);
  return file_now + file_delta;
}

Status GuestIoBridge::nt_query_directory_file(
    Handle file_handle, Handle event_handle, GuestAddress apc_routine,
    GuestAddress apc_context, GuestAddress io_status_block,
    GuestAddress file_information, std::uint32_t length,
    GuestAddress file_name, bool restart_scan) {
  if (length < kDirectoryInfoBaseSize) {
    IoStatusBlock iosb{status::InfoLengthMismatch, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }
  if (!file_information || !writable_range(file_information, length)) {
    return status::AccessViolation;
  }

  std::string pattern;
  if (file_name && !read_ansi_string(file_name, pattern)) {
    return status::AccessViolation;
  }

  std::vector<DirectoryInformation> entries;
  IoStatusBlock iosb{};
  const auto result = facade_.query_directory_file(
      file_handle, pattern, restart_scan, 1, entries, iosb, event_handle,
      apc_context);
  if (result != status::Success || entries.empty()) {
    if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
    queue_apc(apc_routine, apc_context, io_status_block, iosb);
    return result;
  }

  const auto& item = entries.front();
  const auto& entry = item.entry;
  const auto name_length = static_cast<std::uint32_t>(entry.name.size());
  const auto record_size = align_up(kDirectoryInfoBaseSize + name_length, 8);
  if (record_size > length) {
    iosb = {status::BufferOverflow, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }

  try {
    const auto time = xbox_time(entry.info.last_write_time);
    memory_.fill_bytes(file_information, record_size, 0);
    memory_.write32_be(file_information + 0x00, 0);
    memory_.write32_be(file_information + 0x04, item.file_index);
    memory_.write64_be(file_information + 0x08, time);
    memory_.write64_be(file_information + 0x10, time);
    memory_.write64_be(file_information + 0x18, time);
    memory_.write64_be(file_information + 0x20, time);
    memory_.write64_be(file_information + 0x28, entry.info.size);
    memory_.write64_be(file_information + 0x30, entry.info.allocation_size);
    memory_.write32_be(file_information + 0x38, entry.info.attributes);
    memory_.write32_be(file_information + 0x3C, name_length);
    if (name_length) {
      memory_.write_bytes(
          file_information + kDirectoryInfoBaseSize,
          std::as_bytes(std::span(entry.name.data(), entry.name.size())));
    }
  } catch (const memory::MemoryFault&) {
    iosb = {status::AccessViolation, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }

  iosb = {status::Success, record_size};
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  queue_apc(apc_routine, apc_context, io_status_block, iosb);
  return status::Success;
}

Status GuestIoBridge::serialize_network_open_information(
    const FileNetworkOpenInformation& value, GuestAddress destination,
    std::uint32_t length, std::uint32_t& out_bytes) {
  out_bytes = 0;
  if (length < kNetworkOpenInfoSize) return status::InfoLengthMismatch;
  if (!writable_range(destination, kNetworkOpenInfoSize)) {
    return status::AccessViolation;
  }
  try {
    const auto time = xbox_time(value.file.last_write_time);
    memory_.write64_be(destination + 0x00, time);
    memory_.write64_be(destination + 0x08, time);
    memory_.write64_be(destination + 0x10, time);
    memory_.write64_be(destination + 0x18, time);
    memory_.write64_be(destination + 0x20, value.file.allocation_size);
    memory_.write64_be(destination + 0x28, value.file.size);
    memory_.write32_be(destination + 0x30, value.file.attributes);
    memory_.write32_be(destination + 0x34, 0);
    out_bytes = kNetworkOpenInfoSize;
    return status::Success;
  } catch (const memory::MemoryFault&) {
    return status::AccessViolation;
  }
}

Status GuestIoBridge::serialize_file_information(
    std::uint32_t information_class, const FileInformation& value,
    GuestAddress destination, std::uint32_t length,
    std::uint32_t& out_bytes) {
  out_bytes = 0;
  const auto minimum = minimum_query_file_information_length(information_class);
  if (!minimum) return status::InvalidInfoClass;
  if (length < minimum) return status::InfoLengthMismatch;
  if (!writable_range(destination, length)) return status::AccessViolation;

  try {
    memory_.fill_bytes(destination, length, 0);
    switch (static_cast<FileInformationClass>(information_class)) {
      case FileInformationClass::Basic: {
        const auto* info = std::get_if<FileBasicInformation>(&value);
        if (!info) return status::InvalidParameter;
        const auto time = xbox_time(info->last_write_time);
        memory_.write64_be(destination + 0x00, time);
        memory_.write64_be(destination + 0x08, time);
        memory_.write64_be(destination + 0x10, time);
        memory_.write64_be(destination + 0x18, time);
        memory_.write64_be(destination + 0x20, info->attributes);
        out_bytes = 40;
        return status::Success;
      }
      case FileInformationClass::Standard: {
        const auto* info = std::get_if<FileStandardInformation>(&value);
        if (!info) return status::InvalidParameter;
        memory_.write64_be(destination + 0x00, info->allocation_size);
        memory_.write64_be(destination + 0x08, info->end_of_file);
        memory_.write32_be(destination + 0x10, info->number_of_links);
        memory_.write8(destination + 0x14, info->delete_pending ? 1 : 0);
        memory_.write8(destination + 0x15, info->directory ? 1 : 0);
        out_bytes = kStandardInfoSize;
        return status::Success;
      }
      case FileInformationClass::Internal: {
        const auto* info = std::get_if<FileInternalInformation>(&value);
        if (!info) return status::InvalidParameter;
        memory_.write64_be(destination, info->index_number);
        out_bytes = 8;
        return status::Success;
      }
      case FileInformationClass::Position: {
        const auto* info = std::get_if<FilePositionInformation>(&value);
        if (!info) return status::InvalidParameter;
        memory_.write64_be(destination, info->current_byte_offset);
        out_bytes = 8;
        return status::Success;
      }
      case FileInformationClass::Name: {
        const auto* info = std::get_if<FileNameInformation>(&value);
        if (!info) return status::InvalidParameter;
        const auto name_size = static_cast<std::uint32_t>(info->name.size());
        if (name_size > length - 4) return status::BufferOverflow;
        memory_.write32_be(destination, name_size);
        if (name_size) {
          memory_.write_bytes(destination + 4,
                              std::as_bytes(std::span(info->name.data(),
                                                      info->name.size())));
        }
        out_bytes = 4 + name_size;
        return status::Success;
      }
      case FileInformationClass::Mode: {
        const auto* info = std::get_if<FileModeInformation>(&value);
        if (!info) return status::InvalidParameter;
        const std::uint32_t mode =
            info->synchronous ? create_option::SynchronousIoNonAlert : 0u;
        memory_.write32_be(destination, mode);
        out_bytes = 4;
        return status::Success;
      }
      case FileInformationClass::Alignment: {
        const auto* info = std::get_if<FileAlignmentInformation>(&value);
        if (!info) return status::InvalidParameter;
        memory_.write32_be(destination, info->alignment_requirement);
        out_bytes = 4;
        return status::Success;
      }
      case FileInformationClass::NetworkOpen: {
        const auto* info = std::get_if<FileNetworkOpenInformation>(&value);
        if (!info) return status::InvalidParameter;
        return serialize_network_open_information(*info, destination, length,
                                                  out_bytes);
      }
      default:
        return status::InvalidInfoClass;
    }
  } catch (const memory::MemoryFault&) {
    return status::AccessViolation;
  }
}

Status GuestIoBridge::deserialize_file_information(
    std::uint32_t information_class, GuestAddress source,
    std::uint32_t length, FileInformation& out) {
  const auto minimum = minimum_set_file_information_length(information_class);
  if (!minimum) return status::InvalidInfoClass;
  if (length < minimum) return status::InfoLengthMismatch;
  if (!readable_range(source, length)) return status::AccessViolation;

  try {
    switch (static_cast<FileInformationClass>(information_class)) {
      case FileInformationClass::Basic: {
        const auto last_write = memory_.read64_be(source + 0x10);
        const auto attributes64 = memory_.read64_be(source + 0x20);
        FileBasicInformation info{};
        if (last_write) {
          info.last_write_time = host_time(last_write);
          info.set_last_write_time = true;
        }
        if (attributes64) {
          info.attributes = static_cast<std::uint32_t>(attributes64);
          info.set_attributes = true;
        }
        out = info;
        return status::Success;
      }
      case FileInformationClass::Disposition:
        out = FileDispositionInformation{memory_.read8(source) != 0};
        return status::Success;
      case FileInformationClass::Position:
        out = FilePositionInformation{memory_.read64_be(source)};
        return status::Success;
      case FileInformationClass::Allocation:
        out = FileAllocationInformation{memory_.read64_be(source)};
        return status::Success;
      case FileInformationClass::EndOfFile:
        out = FileEndOfFileInformation{memory_.read64_be(source)};
        return status::Success;
      case FileInformationClass::Completion:
        out = FileCompletionInformation{memory_.read32_be(source + 0),
                                        memory_.read32_be(source + 4)};
        return status::Success;
      case FileInformationClass::Rename: {
        FileRenameInformation info{};
        info.replace_if_exists = memory_.read32_be(source + 0) != 0;
        info.root_directory = memory_.read32_be(source + 4);
        if (!read_ansi_string(source + 8, info.file_name)) {
          return status::AccessViolation;
        }
        out = std::move(info);
        return status::Success;
      }
      default:
        return status::InvalidInfoClass;
    }
  } catch (const memory::MemoryFault&) {
    return status::AccessViolation;
  }
}

Status GuestIoBridge::nt_query_information_file(
    Handle file_handle, GuestAddress io_status_block,
    GuestAddress information, std::uint32_t information_length,
    std::uint32_t information_class) {
  const auto minimum = minimum_query_file_information_length(information_class);
  if (!minimum) {
    IoStatusBlock iosb{status::InvalidInfoClass, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }
  if (information_length < minimum) {
    IoStatusBlock iosb{status::InfoLengthMismatch, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }

  FileInformation value = FileBasicInformation{};
  IoStatusBlock iosb{};
  auto result = facade_.query_information_file(file_handle, information_class,
                                               value, iosb);
  if (result == status::Success) {
    std::uint32_t bytes = 0;
    result = serialize_file_information(information_class, value, information,
                                        information_length, bytes);
    iosb = {result, result == status::Success ? bytes : 0};
  }
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  return result;
}

Status GuestIoBridge::nt_set_information_file(
    Handle file_handle, GuestAddress io_status_block,
    GuestAddress information, std::uint32_t information_length,
    std::uint32_t information_class) {
  FileInformation value = FileBasicInformation{};
  auto decode = deserialize_file_information(information_class, information,
                                             information_length, value);
  if (decode != status::Success) {
    IoStatusBlock iosb{decode, 0};
    (void)write_iosb(io_status_block, iosb);
    return decode;
  }
  IoStatusBlock iosb{};
  const auto result = facade_.set_information_file(
      file_handle, information_class, value, iosb);
  if (result == status::Success) {
    iosb.information = minimum_set_file_information_length(information_class);
  }
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  return result;
}

Status GuestIoBridge::serialize_volume_information(
    std::uint32_t information_class, const VolumeInformation& value,
    GuestAddress destination, std::uint32_t length,
    std::uint32_t& out_bytes) {
  out_bytes = 0;
  const auto minimum = minimum_volume_information_length(information_class);
  if (!minimum) return status::InvalidInfoClass;
  if (length < minimum) return status::InfoLengthMismatch;
  if (!writable_range(destination, length)) return status::AccessViolation;

  try {
    memory_.fill_bytes(destination, length, 0);
    switch (static_cast<FsInformationClass>(information_class)) {
      case FsInformationClass::Volume: {
        const auto* info = std::get_if<FsVolumeInformation>(&value);
        if (!info) return status::InvalidParameter;
        const auto label_length = static_cast<std::uint32_t>(info->label.size());
        if (label_length > length - 17u) return status::BufferOverflow;
        memory_.write64_be(destination + 0x00, info->creation_time);
        memory_.write32_be(destination + 0x08, info->serial_number);
        memory_.write32_be(destination + 0x0C, label_length);
        memory_.write8(destination + 0x10, info->supports_objects ? 1 : 0);
        if (label_length) {
          memory_.write_bytes(destination + 0x11,
                              std::as_bytes(std::span(info->label.data(),
                                                      info->label.size())));
        }
        out_bytes = 17u + label_length;
        return status::Success;
      }
      case FsInformationClass::Size: {
        const auto* info = std::get_if<FsSizeInformation>(&value);
        if (!info) return status::InvalidParameter;
        memory_.write64_be(destination + 0x00, info->total_allocation_units);
        memory_.write64_be(destination + 0x08, info->available_allocation_units);
        memory_.write32_be(destination + 0x10, info->sectors_per_allocation_unit);
        memory_.write32_be(destination + 0x14, info->bytes_per_sector);
        out_bytes = 24;
        return status::Success;
      }
      case FsInformationClass::Device: {
        const auto* info = std::get_if<FsDeviceInformation>(&value);
        if (!info) return status::InvalidParameter;
        memory_.write32_be(destination + 0x00, info->device_type);
        memory_.write32_be(destination + 0x04, info->characteristics);
        out_bytes = 8;
        return status::Success;
      }
      case FsInformationClass::Attribute: {
        const auto* info = std::get_if<FsAttributeInformation>(&value);
        if (!info) return status::InvalidParameter;
        const auto name_length = static_cast<std::uint32_t>(info->name.size());
        if (name_length > length - 12u) return status::BufferOverflow;
        memory_.write32_be(destination + 0x00, info->attributes);
        memory_.write32_be(destination + 0x04,
                           static_cast<std::uint32_t>(info->component_name_max_length));
        memory_.write32_be(destination + 0x08, name_length);
        if (name_length) {
          memory_.write_bytes(destination + 0x0C,
                              std::as_bytes(std::span(info->name.data(),
                                                      info->name.size())));
        }
        out_bytes = 12u + name_length;
        return status::Success;
      }
    }
  } catch (const memory::MemoryFault&) {
    return status::AccessViolation;
  }
  return status::InvalidInfoClass;
}

Status GuestIoBridge::nt_query_volume_information_file(
    Handle file_handle, GuestAddress io_status_block,
    GuestAddress information, std::uint32_t information_length,
    std::uint32_t information_class) {
  const auto minimum = minimum_volume_information_length(information_class);
  if (!minimum) {
    IoStatusBlock iosb{status::InvalidInfoClass, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }
  if (information_length < minimum) {
    IoStatusBlock iosb{status::InfoLengthMismatch, 0};
    (void)write_iosb(io_status_block, iosb);
    return iosb.status;
  }

  VolumeInformation value = FsVolumeInformation{};
  IoStatusBlock iosb{};
  auto result = facade_.query_volume_information_file(file_handle,
                                                      information_class,
                                                      value, iosb);
  if (result == status::Success) {
    std::uint32_t bytes = 0;
    result = serialize_volume_information(information_class, value, information,
                                          information_length, bytes);
    iosb = {result, result == status::Success ? bytes : 0};
  }
  if (!write_iosb(io_status_block, iosb)) return status::AccessViolation;
  return result;
}

Status GuestIoBridge::nt_query_full_attributes_file(
    GuestAddress object_attributes, GuestAddress file_information) {
  ObjectAttributes attrs{};
  std::string path;
  if (!read_object_attributes(object_attributes, attrs, path)) {
    return status::AccessViolation;
  }
  if (!file_information ||
      !writable_range(file_information, kNetworkOpenInfoSize)) {
    return status::AccessViolation;
  }

  FileNetworkOpenInformation value{};
  IoStatusBlock iosb{};
  auto result = facade_.query_full_attributes_file(attrs.root_directory, path,
                                                   value, iosb);
  if (result != status::Success) return result;
  std::uint32_t bytes = 0;
  return serialize_network_open_information(value, file_information,
                                            kNetworkOpenInfoSize, bytes);
}

}  // namespace xenon::kernel::xbox
