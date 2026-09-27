#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

struct HandleView {
  std::shared_ptr<KernelObject> object{};
  std::uint32_t granted_access{};
  HandleFlags flags{HandleFlags::None};

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(object);
  }
};

class HandleTable {
 public:
  HandleTable() = default;
  ~HandleTable();

  HandleTable(const HandleTable&) = delete;
  HandleTable& operator=(const HandleTable&) = delete;

  [[nodiscard]] KernelIoCode insert(std::shared_ptr<KernelObject> object,
                                    std::uint32_t granted_access,
                                    HandleFlags flags, Handle& out_handle);
  [[nodiscard]] KernelIoCode lookup(Handle handle, HandleView& out_view) const;
  [[nodiscard]] KernelIoCode duplicate(
      Handle source, const DuplicateHandleOptions& options,
      Handle& out_handle);
  [[nodiscard]] KernelIoCode close(Handle handle, bool force = false);
  [[nodiscard]] KernelIoCode set_flags(Handle handle, HandleFlags flags);

  [[nodiscard]] std::size_t size() const;
  void clear();

 private:
  struct Entry {
    std::shared_ptr<KernelObject> object{};
    std::uint32_t granted_access{};
    HandleFlags flags{HandleFlags::None};
  };

  [[nodiscard]] Handle allocate_handle_locked();

  mutable std::mutex mutex_{};
  std::unordered_map<Handle, Entry> entries_{};
  Handle next_handle_{4};
};

}  // namespace xenon::kernel
