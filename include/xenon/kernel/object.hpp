#pragma once

#include <atomic>
#include <cstdint>

#include "xenon/kernel/io_types.hpp"

namespace xenon::kernel {

class HandleTable;

class KernelObject {
 public:
  virtual ~KernelObject() = default;

  KernelObject(const KernelObject&) = delete;
  KernelObject& operator=(const KernelObject&) = delete;

  [[nodiscard]] ObjectType type() const noexcept { return type_; }
  [[nodiscard]] std::uint64_t object_id() const noexcept { return object_id_; }
  [[nodiscard]] std::uint32_t handle_count() const noexcept {
    return handle_count_.load(std::memory_order_acquire);
  }

 protected:
  explicit KernelObject(ObjectType type);

  virtual void on_last_handle_closed() noexcept {}

 private:
  friend class HandleTable;

  void retain_handle() noexcept;
  void release_handle() noexcept;

  ObjectType type_{ObjectType::Unknown};
  std::uint64_t object_id_{};
  std::atomic<std::uint32_t> handle_count_{0};
};

}  // namespace xenon::kernel
