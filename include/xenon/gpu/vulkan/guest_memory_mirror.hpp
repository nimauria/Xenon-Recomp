#pragma once

#include <cstdint>
#include <string>

#include "xenon/gpu/vulkan/command_queue.hpp"
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
  [[nodiscard]] bool synchronize_range(std::uint32_t address,
                                       std::uint32_t width);
  void mark_gpu_write(std::uint32_t address, std::uint32_t width);
  [[nodiscard]] bool make_cpu_visible(std::uint32_t address,
                                      std::uint32_t width);
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

  memory::AddressSpace* memory_{};
  CommandQueue* queue_{};
  Buffer mirror_{};
  Buffer upload_{};
  Buffer readback_{};
  memory::GuestMemoryGpuCoherency coherency_{};
  bool shader_read_state_{};
  bool shader_write_state_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
