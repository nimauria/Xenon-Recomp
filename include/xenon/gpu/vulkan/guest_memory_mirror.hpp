#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/guest_memory_limits.hpp"
#include "xenon/gpu/readback_publisher.hpp"
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
  [[nodiscard]] bool cpu_visibility_pending() const { return publisher_.pending(); }
  // The readbacks written into guest memory since the last call.
  [[nodiscard]] std::vector<memory::GpuPublishedWrite> take_published_writes() {
    return publisher_.take_published_writes();
  }
  [[nodiscard]] bool has_gpu_dirty(std::uint32_t address,
                                   std::uint32_t width) const;
  [[nodiscard]] bool device_range_valid(std::uint32_t address,
                                        std::uint32_t width) const {
    return coherency_.device_range_valid(address, width);
  }
  // Uploads staged by synchronize_range() since the last flush are recorded
  // together: one barrier pair around one copy per range, rather than one
  // ordered command (which ends the open draw run) per range. The queue's
  // pre-ordered-work hook (and BackendCore before each draw) calls this first,
  // so copies still precede the work that reads them and follow the draws that
  // read the old bytes.
  [[nodiscard]] bool flush_pending_uploads();
  [[nodiscard]] bool uploads_pending() const noexcept { return !pending_.empty(); }
  [[nodiscard]] std::uint64_t upload_batches() const noexcept { return upload_batches_; }
  [[nodiscard]] std::uint64_t upload_ranges() const noexcept { return upload_ranges_; }
  void prepare_shader_access(VkCommandBuffer command, bool writable);
  // Whether prepare_shader_access() would record a barrier now: earlier
  // shader writes are pending, or this access writes after reads.
  [[nodiscard]] bool needs_shader_access_barrier(bool writable) const noexcept {
    return shader_write_state_ || writable;
  }
  // A shader access that needed no barrier (needs_shader_access_barrier() was
  // false): later writes must still wait for it.
  void note_shader_access(bool writable) noexcept {
    shader_read_state_ = true;
    shader_write_state_ = writable;
  }
  [[nodiscard]] VkBuffer buffer() const noexcept { return mirror_.buffer(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // The storage-buffer limits passes binding a window of the mirror use.
  [[nodiscard]] const GuestMemoryLimits& storage_limits() const noexcept { return limits_; }

 private:
  GuestMemoryLimits limits_{};
  static constexpr std::uint32_t kTransferSize = 16u * 1024u * 1024u;
  // Room for a frame's resolves in flight (AC6 resolves ~20 MiB a frame).
  static constexpr std::uint32_t kReadbackSize = 64u * 1024u * 1024u;

  // Submits the open batch and waits until every readback whose GPU value is
  // <= `value` is in guest memory.
  [[nodiscard]] bool wait_readbacks(std::uint64_t value);

  memory::AddressSpace* memory_{};
  CommandQueue* queue_{};
  Buffer mirror_{};
  Buffer upload_{};
  // Regions of upload_ the GPU may still be copying from.
  StagingRing upload_ring_{};
  Buffer readback_{};
  // Regions of readback_ the GPU may still be writing or the publisher has
  // not written into guest memory yet.
  StagingRing readback_ring_{};
  memory::GuestMemoryGpuCoherency coherency_{};
  struct PendingUpload {
    std::uint32_t address{};
    std::uint32_t size{};
    std::uint64_t offset{};
  };
  std::vector<PendingUpload> pending_{};
  std::uint64_t upload_batches_{};
  std::uint64_t upload_ranges_{};
  bool shader_read_state_{};
  bool shader_write_state_{};
  std::string error_{};
  // Writes finished readbacks into guest memory on a thread of its own.
  ReadbackPublisher publisher_{};
};

}  // namespace xenon::gpu::vulkan
