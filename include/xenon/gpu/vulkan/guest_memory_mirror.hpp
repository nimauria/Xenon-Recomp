#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/staging_ring.hpp"
#include "xenon/gpu/vulkan/memory.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

namespace xenon::gpu::vulkan {

class GuestMemoryMirror {
 public:
  ~GuestMemoryMirror();
  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device,
                                VkDevice device, CommandQueue& queue,
                                memory::AddressSpace& memory);
  void reset() noexcept;
  [[nodiscard]] bool synchronize();
  // read_epoch: see memory::GuestMemoryGpuCoherency::plan_upload(); 0 is now.
  [[nodiscard]] bool synchronize_range(
      std::uint32_t address, std::uint32_t width,
      memory::GpuRangeUsage usage = memory::GpuRangeUsage::Generic,
      std::uint64_t read_epoch = 0);
  void mark_gpu_write(std::uint32_t address, std::uint32_t width);
  [[nodiscard]] bool make_cpu_visible(
      std::uint32_t address, std::uint32_t width,
      memory::GpuRangeUsage usage = memory::GpuRangeUsage::RenderReadback);
  // The deferred form: records the GPU -> CPU copy of the GPU-owned portion
  // of the range into the open batch and returns without waiting. The bytes
  // reach guest memory in complete_cpu_visibility(), once the GPU has copied
  // them, and only where neither the CPU nor a newer GPU write has replaced
  // them since (GuestMemoryGpuCoherency generations).
  [[nodiscard]] bool queue_cpu_visibility(
      std::uint32_t address, std::uint32_t width,
      memory::GpuRangeUsage usage = memory::GpuRangeUsage::RenderReadback);
  // Writes every queued readback the GPU has finished into guest memory;
  // with `wait`, first submits and waits for all of them.
  [[nodiscard]] bool complete_cpu_visibility(bool wait);
  [[nodiscard]] bool cpu_visibility_pending() const noexcept {
    return !pending_readbacks_.empty();
  }
  // The readbacks written into guest memory since the last call.
  [[nodiscard]] std::vector<memory::GpuPublishedWrite> take_published_writes() {
    return std::exchange(published_writes_, {});
  }
  [[nodiscard]] bool has_gpu_dirty(std::uint32_t address,
                                   std::uint32_t width) const;
  [[nodiscard]] bool device_range_valid(std::uint32_t address,
                                        std::uint32_t width) const {
    return coherency_.device_range_valid(address, width);
  }
  void prepare_shader_access(VkCommandBuffer command, bool writable);
  [[nodiscard]] VkBuffer buffer() const noexcept { return mirror_.buffer(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  static constexpr std::uint32_t kTransferSize = 16u * 1024u * 1024u;
  // Room for a frame's resolves in flight (AC6 resolves ~20 MiB a frame).
  static constexpr std::uint32_t kReadbackSize = 64u * 1024u * 1024u;

  struct PendingReadback {
    memory::GpuReadbackRange range{};
    std::uint64_t offset{};
    std::uint64_t value{};
  };
  [[nodiscard]] bool complete_through(std::uint64_t completed);

  memory::AddressSpace* memory_{};
  CommandQueue* queue_{};
  Buffer mirror_{};
  Buffer upload_{};
  // Regions of upload_ the GPU may still be copying from.
  StagingRing upload_ring_{};
  Buffer readback_{};
  // Regions of readback_ the GPU may still be writing or the CPU has not
  // read yet; pending_readbacks_ holds them in queue order.
  StagingRing readback_ring_{};
  std::deque<PendingReadback> pending_readbacks_{};
  std::vector<memory::GpuPublishedWrite> published_writes_{};
  memory::GuestMemoryGpuCoherency coherency_{};
  bool shader_read_state_{};
  bool shader_write_state_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
