#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/memory.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu::vulkan {

class GuestMemoryMirror {
 public:
  ~GuestMemoryMirror();
  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device,
                                VkDevice device, CommandQueue& queue,
                                memory::AddressSpace& memory);
  void reset() noexcept;
  [[nodiscard]] bool synchronize();
  [[nodiscard]] VkBuffer buffer() const noexcept { return mirror_.buffer(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  static constexpr std::uint32_t kPageSize = 4096;
  static constexpr std::uint32_t kUploadSize = 16u * 1024u * 1024u;
  void mark_dirty(std::uint32_t address, std::uint32_t width);
  memory::AddressSpace* memory_{};
  CommandQueue* queue_{};
  Buffer mirror_{};
  Buffer upload_{};
  std::uint64_t callback_id_{};
  std::vector<std::uint8_t> dirty_pages_{};
  std::mutex dirty_mutex_{};
  bool shader_read_state_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
