#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/kernel/completion_port.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/file_information.hpp"
#include "xenon/kernel/file_object.hpp"
#include "xenon/kernel/handle_table.hpp"

namespace xenon::kernel {

class KernelIoManager {
 public:
  explicit KernelIoManager(std::shared_ptr<filesystem::VirtualFileSystem> vfs);
  ~KernelIoManager();

  KernelIoManager(const KernelIoManager&) = delete;
  KernelIoManager& operator=(const KernelIoManager&) = delete;

  [[nodiscard]] IoStatus open(std::string_view guest_path,
                              const KernelOpenOptions& options,
                              Handle& out_handle,
                              filesystem::OpenAction* out_action = nullptr);
  [[nodiscard]] IoStatus open_at(Handle root_directory,
                                 std::string_view guest_path,
                                 const KernelOpenOptions& options,
                                 Handle& out_handle,
                                 filesystem::OpenAction* out_action = nullptr);
  [[nodiscard]] KernelIoCode duplicate(
      Handle source, const DuplicateHandleOptions& options,
      Handle& out_handle);
  [[nodiscard]] KernelIoCode close(Handle handle, bool force = false);
  [[nodiscard]] KernelIoCode set_handle_flags(Handle handle,
                                              HandleFlags flags);

  [[nodiscard]] IoStatus read(Handle handle,
                              std::span<std::byte> destination,
                              std::optional<std::uint64_t> byte_offset = {},
                              bool update_position = true,
                              std::uint64_t context = 0);
  [[nodiscard]] IoStatus write(
      Handle handle, std::span<const std::byte> source,
      std::optional<std::uint64_t> byte_offset = {},
      bool update_position = true, std::uint64_t context = 0);
  [[nodiscard]] IoStatus seek(Handle handle, std::int64_t offset,
                              filesystem::SeekOrigin origin);
  [[nodiscard]] IoStatus resize(Handle handle, std::uint64_t new_size,
                                std::uint64_t context = 0);
  [[nodiscard]] IoStatus flush(Handle handle, std::uint64_t context = 0);
  [[nodiscard]] IoStatus stat(Handle handle,
                              filesystem::FileInfo& out_info) const;
  [[nodiscard]] IoStatus stat_path(std::string_view guest_path,
                                   filesystem::FileInfo& out_info) const;
  [[nodiscard]] IoStatus stat_path_at(Handle root_directory,
                                      std::string_view guest_path,
                                      filesystem::FileInfo& out_info) const;
  [[nodiscard]] IoStatus query_directory(
      Handle handle, const filesystem::DirectoryQuery* query, bool restart,
      std::size_t max_entries, std::vector<filesystem::DirectoryEntry>& out,
      bool& out_end, std::uint64_t context = 0);

  [[nodiscard]] IoStatus query_information(Handle handle,
                                           FileInformationClass info_class,
                                           FileInformation& out_info) const;
  [[nodiscard]] IoStatus set_information(Handle handle,
                                         FileInformationClass info_class,
                                         const FileInformation& info,
                                         std::uint64_t context = 0);
  [[nodiscard]] IoStatus query_volume_information(Handle handle,
                                                  VolumeInformation& out_info) const;

  [[nodiscard]] KernelIoCode set_delete_pending(Handle handle, bool value);
  [[nodiscard]] bool delete_pending(Handle handle) const;

  [[nodiscard]] IoStatus begin_request(Handle handle, IoOperation operation,
                                       std::uint64_t context,
                                       std::shared_ptr<IoRequest>& out_request);
  [[nodiscard]] KernelIoCode complete_request(Handle handle,
                                              std::uint64_t request_id,
                                              IoStatus result);
  [[nodiscard]] KernelIoCode cancel_request(Handle handle,
                                            std::uint64_t request_id);

  [[nodiscard]] KernelIoCode create_completion_port(Handle& out_handle);
  [[nodiscard]] KernelIoCode associate_completion_port(Handle file_handle,
                                                        Handle port_handle,
                                                        std::uint64_t key);
  [[nodiscard]] KernelIoCode remove_completion(
      Handle port_handle, CompletionPacket& out_packet,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

  [[nodiscard]] KernelIoCode create_event(bool manual_reset, bool initial_state,
                                          Handle& out_handle);
  [[nodiscard]] KernelIoCode set_event(Handle event_handle);
  [[nodiscard]] KernelIoCode reset_event(Handle event_handle);
  [[nodiscard]] KernelIoCode wait_event(Handle event_handle,
                                        std::chrono::milliseconds timeout,
                                        bool& out_signaled);

  [[nodiscard]] std::size_t handle_count() const { return handles_.size(); }
  [[nodiscard]] std::shared_ptr<filesystem::VirtualFileSystem> vfs() const {
    return vfs_;
  }

 private:
  struct OpenShareState;

  [[nodiscard]] IoStatus lookup_file(Handle handle, HandleView& out_view,
                                     std::shared_ptr<KernelFileObject>& out_file) const;
  [[nodiscard]] KernelIoCode resolve_rooted_path(Handle root_directory,
                                                 std::string_view guest_path,
                                                 std::string& out_path) const;
  [[nodiscard]] static bool access_allowed(std::uint32_t granted,
                                           filesystem::FileAccess required);

  std::shared_ptr<filesystem::VirtualFileSystem> vfs_{};
  HandleTable handles_{};
  std::shared_ptr<OpenShareState> share_state_{};
  mutable std::mutex open_mutex_{};
};

}  // namespace xenon::kernel
