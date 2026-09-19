#pragma once

#include <cstdint>
#include <string>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/gpu_coherency.hpp"

namespace xenon::gpu::d3d12 {

class GuestMemoryMirror {
 public:
  GuestMemoryMirror() = default;
  ~GuestMemoryMirror();
  GuestMemoryMirror(const GuestMemoryMirror&) = delete;
  GuestMemoryMirror& operator=(const GuestMemoryMirror&) = delete;

  [[nodiscard]] bool initialize(ID3D12Device* device, CommandQueue& queue,
                                memory::AddressSpace& memory);
  void reset() noexcept;
  // Uploads only bytes that are newer on the CPU. GPU-owned memexport ranges
  // are never overwritten by a wider page upload.
  [[nodiscard]] bool synchronize();
  [[nodiscard]] bool synchronize_range(
      std::uint32_t address, std::uint32_t width,
      memory::GpuRangeUsage usage = memory::GpuRangeUsage::Generic);
  // Records a range that a shader may have modified through memexport.
  void mark_gpu_write(std::uint32_t address, std::uint32_t width);
  // Performs an on-demand GPU -> CPU download for the GPU-owned portion of the
  // requested range. This is intentionally explicit so normal memexport stays
  // GPU-resident; Memory v2 can call it when a guest CPU access needs visibility.
  [[nodiscard]] bool make_cpu_visible(
      std::uint32_t address, std::uint32_t width,
      memory::GpuRangeUsage usage = memory::GpuRangeUsage::RenderReadback);
  [[nodiscard]] bool has_gpu_dirty(std::uint32_t address,
                                   std::uint32_t width) const;
  [[nodiscard]] bool device_range_valid(std::uint32_t address,
                                        std::uint32_t width) const {
    return coherency_.device_range_valid(address, width);
  }
  void prepare_shader_access(ID3D12GraphicsCommandList* list, bool writable);
  [[nodiscard]] ID3D12Resource* resource() const noexcept {
    return mirror_.resource();
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  static constexpr std::uint32_t kTransferSize = 16u * 1024u * 1024u;

  memory::AddressSpace* memory_{};
  CommandQueue* queue_{};
  Buffer mirror_{};
  Buffer upload_{};
  Buffer readback_{};
  memory::GuestMemoryGpuCoherency coherency_{};
  D3D12_RESOURCE_STATES state_{D3D12_RESOURCE_STATE_COPY_DEST};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
