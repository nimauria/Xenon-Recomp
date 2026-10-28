#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "xenon/gpu/gpu_resolve.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"

namespace xenon::gpu::vulkan {

// Color resolves and their clears on the GPU, out of the native image that
// owns the resolved EDRAM tiles (see GpuResolveConstants). Both are recorded
// into the queue's open batch; nothing is read back.
class GpuResolver {
 public:
  GpuResolver() = default;
  ~GpuResolver();
  GpuResolver(const GpuResolver&) = delete;
  GpuResolver& operator=(const GpuResolver&) = delete;

  [[nodiscard]] bool resolve(VkDevice device, CommandQueue& queue, GuestMemoryMirror& mirror,
                             RenderTargetImage& owner, const GpuResolveConstants& constants);
  [[nodiscard]] bool clear(VkDevice device, CommandQueue& queue, RenderTargetImage& owner,
                           const GpuClearConstants& constants);
  void reset() noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] bool create_layouts(VkDevice device);
  [[nodiscard]] VkPipeline resolve_pipeline(VkDevice device, MsaaSamples samples);
  [[nodiscard]] VkPipeline clear_pipeline(VkDevice device, VkFormat format, MsaaSamples samples);

  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout source_layout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout guest_layout_{VK_NULL_HANDLE};
  VkPipelineLayout resolve_layout_{VK_NULL_HANDLE};
  VkPipelineLayout clear_layout_{VK_NULL_HANDLE};
  VkDescriptorPool pool_{VK_NULL_HANDLE};
  VkDescriptorSet guest_set_{VK_NULL_HANDLE};
  VkBuffer guest_set_buffer_{VK_NULL_HANDLE};
  std::unordered_map<std::uint32_t, VkPipeline> resolve_pipelines_{};
  std::unordered_map<std::uint64_t, VkPipeline> clear_pipelines_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
