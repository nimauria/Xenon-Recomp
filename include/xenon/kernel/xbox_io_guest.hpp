#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "xenon/kernel/xbox_io.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::kernel::xbox {

// Guest-memory marshalling boundary for Xbox 360 xboxkrnl file I/O.
//
// IoFacade owns Xbox/NT filesystem semantics; AddressSpace owns guest memory,
// protection and coherency. This bridge deliberately owns neither. It only
// decodes big-endian guest structures, validates guest buffers, calls IoFacade,
// and writes results back in Xbox layout.
class GuestIoBridge {
 public:
  using GuestAddress = memory::GuestAddress;
  using ApcQueue = std::function<void(GuestAddress routine,
                                      GuestAddress context,
                                      GuestAddress io_status_block)>;

  GuestIoBridge(memory::AddressSpace& memory, KernelIoManager& io)
      : memory_(memory), io_(io), facade_(io) {}

  void set_apc_queue(ApcQueue queue) { apc_queue_ = std::move(queue); }

  [[nodiscard]] Status nt_create_file(
      GuestAddress handle_out, std::uint32_t desired_access,
      GuestAddress object_attributes, GuestAddress io_status_block,
      GuestAddress allocation_size, std::uint32_t file_attributes,
      std::uint32_t share_access, std::uint32_t creation_disposition,
      std::uint32_t create_options);

  [[nodiscard]] Status nt_open_file(
      GuestAddress handle_out, std::uint32_t desired_access,
      GuestAddress object_attributes, GuestAddress io_status_block,
      std::uint32_t open_options);

  [[nodiscard]] Status nt_read_file(
      Handle file_handle, Handle event_handle, GuestAddress apc_routine,
      GuestAddress apc_context, GuestAddress io_status_block,
      GuestAddress buffer, std::uint32_t buffer_length,
      GuestAddress byte_offset);

  [[nodiscard]] Status nt_read_file_scatter(
      Handle file_handle, Handle event_handle, GuestAddress apc_routine,
      GuestAddress apc_context, GuestAddress io_status_block,
      GuestAddress segment_array, std::uint32_t length,
      GuestAddress byte_offset);

  [[nodiscard]] Status nt_write_file(
      Handle file_handle, Handle event_handle, GuestAddress apc_routine,
      GuestAddress apc_context, GuestAddress io_status_block,
      GuestAddress buffer, std::uint32_t buffer_length,
      GuestAddress byte_offset);

  [[nodiscard]] Status nt_flush_buffers_file(Handle file_handle,
                                              GuestAddress io_status_block);

  [[nodiscard]] Status nt_query_directory_file(
      Handle file_handle, Handle event_handle, GuestAddress apc_routine,
      GuestAddress apc_context, GuestAddress io_status_block,
      GuestAddress file_information, std::uint32_t length,
      GuestAddress file_name, bool restart_scan);

  [[nodiscard]] Status nt_query_information_file(
      Handle file_handle, GuestAddress io_status_block,
      GuestAddress information, std::uint32_t information_length,
      std::uint32_t information_class);

  [[nodiscard]] Status nt_set_information_file(
      Handle file_handle, GuestAddress io_status_block,
      GuestAddress information, std::uint32_t information_length,
      std::uint32_t information_class);

  [[nodiscard]] Status nt_query_volume_information_file(
      Handle file_handle, GuestAddress io_status_block,
      GuestAddress information, std::uint32_t information_length,
      std::uint32_t information_class);

  [[nodiscard]] Status nt_query_full_attributes_file(
      GuestAddress object_attributes, GuestAddress file_information);

 private:
  struct ObjectAttributes {
    Handle root_directory{kInvalidHandle};
    GuestAddress name{};
    std::uint32_t attributes{};
  };

  [[nodiscard]] bool readable_range(GuestAddress address,
                                    std::uint32_t size) const noexcept;
  [[nodiscard]] bool writable_range(GuestAddress address,
                                    std::uint32_t size) const noexcept;
  [[nodiscard]] bool range_has_access(GuestAddress address,
                                      std::uint32_t size,
                                      memory::Protect access) const noexcept;
  [[nodiscard]] bool read_ansi_string(GuestAddress descriptor,
                                      std::string& out) const;
  [[nodiscard]] bool read_object_attributes(GuestAddress address,
                                            ObjectAttributes& out,
                                            std::string& out_name) const;
  [[nodiscard]] bool read_optional_offset(
      GuestAddress address, std::optional<std::uint64_t>& out) const;

  [[nodiscard]] bool write_iosb(GuestAddress address,
                                const IoStatusBlock& iosb) noexcept;
  [[nodiscard]] bool write_status_result(GuestAddress iosb_address,
                                         Status call_status,
                                         const IoStatusBlock& iosb) noexcept;
  void queue_apc(GuestAddress routine, GuestAddress context,
                 GuestAddress iosb_address,
                 const IoStatusBlock& iosb) const;

  [[nodiscard]] Status serialize_file_information(
      std::uint32_t information_class, const FileInformation& value,
      GuestAddress destination, std::uint32_t length,
      std::uint32_t& out_bytes);
  [[nodiscard]] Status deserialize_file_information(
      std::uint32_t information_class, GuestAddress source,
      std::uint32_t length, FileInformation& out);
  [[nodiscard]] Status serialize_volume_information(
      std::uint32_t information_class, const VolumeInformation& value,
      GuestAddress destination, std::uint32_t length,
      std::uint32_t& out_bytes);
  [[nodiscard]] Status serialize_network_open_information(
      const FileNetworkOpenInformation& value, GuestAddress destination,
      std::uint32_t length, std::uint32_t& out_bytes);

  [[nodiscard]] static std::uint64_t xbox_time(
      std::filesystem::file_time_type value) noexcept;
  [[nodiscard]] static std::filesystem::file_time_type host_time(
      std::uint64_t value) noexcept;

  memory::AddressSpace& memory_;
  KernelIoManager& io_;
  IoFacade facade_;
  ApcQueue apc_queue_{};
};

}  // namespace xenon::kernel::xbox
