#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/filesystem/directory_cursor.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/kernel/io_request.hpp"
#include "xenon/kernel/file_information.hpp"
#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

class IoCompletionPort;

class FileObjectLease {
 public:
  virtual ~FileObjectLease() = default;

  [[nodiscard]] virtual KernelIoCode set_delete_pending(bool value) = 0;
  [[nodiscard]] virtual bool delete_pending() const = 0;
  [[nodiscard]] virtual KernelIoCode rename_to(
      const filesystem::ResolvedPath& target, bool replace_existing) = 0;
  virtual void release() noexcept = 0;
};

class KernelFileObject final : public KernelObject {
 public:
  KernelFileObject(filesystem::ResolvedPath resolved,
                   filesystem::FileAccess open_access,
                   filesystem::ShareAccess share_access, bool synchronous,
                   bool directory, std::unique_ptr<filesystem::FileHandle> file,
                   std::shared_ptr<FileObjectLease> lease);
  ~KernelFileObject() override;

  [[nodiscard]] std::string path() const;
  [[nodiscard]] bool is_directory() const noexcept { return directory_; }
  [[nodiscard]] bool synchronous() const noexcept { return synchronous_; }
  [[nodiscard]] bool closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }
  [[nodiscard]] filesystem::FileAccess open_access() const noexcept {
    return open_access_;
  }
  [[nodiscard]] filesystem::ShareAccess share_access() const noexcept {
    return share_access_;
  }

  [[nodiscard]] std::uint64_t position() const;
  [[nodiscard]] IoStatus set_position(std::uint64_t value);
  [[nodiscard]] filesystem::ResolvedPath resolved_path() const;
  [[nodiscard]] IoStatus seek(std::int64_t offset,
                              filesystem::SeekOrigin origin);
  [[nodiscard]] IoStatus read(std::span<std::byte> destination,
                              std::optional<std::uint64_t> byte_offset,
                              bool update_position,
                              std::uint64_t context = 0);
  [[nodiscard]] IoStatus write(std::span<const std::byte> source,
                               std::optional<std::uint64_t> byte_offset,
                               bool update_position,
                               std::uint64_t context = 0);
  [[nodiscard]] IoStatus resize(std::uint64_t new_size,
                                std::uint64_t context = 0);
  [[nodiscard]] IoStatus flush(std::uint64_t context = 0);
  [[nodiscard]] IoStatus stat(filesystem::FileInfo& out_info) const;

  // Supplying a non-null query starts a new search. A null query continues the
  // existing search, while restart rewinds the active cursor. Duplicated
  // handles share this cursor because it belongs to the file object.
  [[nodiscard]] IoStatus query_directory(
      const filesystem::DirectoryQuery* query, bool restart,
      std::size_t max_entries, std::vector<filesystem::DirectoryEntry>& out,
      bool& out_end, std::uint64_t context = 0);

  [[nodiscard]] KernelIoCode set_delete_pending(bool value);
  [[nodiscard]] IoStatus set_basic_information(const FileBasicInformation& info);
  [[nodiscard]] IoStatus rename_to(const filesystem::ResolvedPath& target,
                                   bool replace_existing);
  void associate_completion_port(std::shared_ptr<IoCompletionPort> port,
                                 std::uint64_t key);
  void clear_completion_port();
  [[nodiscard]] bool delete_pending() const;

  [[nodiscard]] std::shared_ptr<IoRequest> begin_request(
      IoOperation operation, std::uint64_t context = 0);
  [[nodiscard]] KernelIoCode complete_request(std::uint64_t request_id,
                                              IoStatus result);
  [[nodiscard]] KernelIoCode cancel_request(std::uint64_t request_id);
  [[nodiscard]] std::size_t active_request_count() const;

 protected:
  void on_last_handle_closed() noexcept override;

 private:
  [[nodiscard]] IoStatus finish_synchronous(
      const std::shared_ptr<IoRequest>& request, IoStatus status);
  void cancel_all_requests() noexcept;

  filesystem::ResolvedPath resolved_{};
  filesystem::FileAccess open_access_{filesystem::FileAccess::None};
  filesystem::ShareAccess share_access_{filesystem::ShareAccess::All};
  bool synchronous_{true};
  bool directory_{};

  mutable std::mutex io_mutex_{};
  std::unique_ptr<filesystem::FileHandle> file_{};
  std::uint64_t position_{};
  filesystem::DirectoryQuery directory_query_{};
  std::unique_ptr<filesystem::DirectoryCursor> directory_cursor_{};
  std::atomic<bool> closed_{false};

  std::shared_ptr<FileObjectLease> lease_{};

  mutable std::mutex request_mutex_{};
  std::unordered_map<std::uint64_t, std::shared_ptr<IoRequest>> requests_{};
  std::atomic<std::uint64_t> next_request_id_{1};

  mutable std::mutex completion_mutex_{};
  std::weak_ptr<IoCompletionPort> completion_port_{};
  std::uint64_t completion_key_{};
};

}  // namespace xenon::kernel
