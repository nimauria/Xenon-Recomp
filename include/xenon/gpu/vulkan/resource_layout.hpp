#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/vulkan/memory.hpp"
#include "xenon/gpu/resource_ir.hpp"

namespace xenon::gpu::vulkan {

class ResourceLayout {
 public:
  static constexpr std::uint32_t kConstantBinding = 0;
  static constexpr std::uint32_t kGuestMemoryBinding = 16;
  static constexpr std::uint32_t kTexture1DBinding = 17;
  static constexpr std::uint32_t kTexture2DBinding = 49;
  static constexpr std::uint32_t kTexture3DBinding = 81;
  static constexpr std::uint32_t kTextureCubeBinding = 113;
  static constexpr std::uint32_t kSamplerBinding = 160;
  static constexpr std::uint32_t kMemoryExportBinding = 192;
  static constexpr VkDeviceSize kConstantBufferBytes = 9472;

  ResourceLayout() = default;
  ~ResourceLayout();
  ResourceLayout(const ResourceLayout&) = delete;
  ResourceLayout& operator=(const ResourceLayout&) = delete;

  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device, VkDevice device);
  void reset() noexcept;
  [[nodiscard]] bool bind_guest_memory(VkBuffer buffer, VkDeviceSize size);
  [[nodiscard]] bool bind_texture(std::uint32_t slot, TextureDimension dimension,
                                  VkImageView view, VkSampler sampler);
  [[nodiscard]] bool ready() const noexcept { return pipeline_layout_ != VK_NULL_HANDLE; }
  [[nodiscard]] VkDescriptorSetLayout descriptor_set_layout() const noexcept {
    return descriptor_set_layout_;
  }
  [[nodiscard]] VkPipelineLayout pipeline_layout() const noexcept {
    return pipeline_layout_;
  }
  [[nodiscard]] VkDescriptorSet descriptor_set() const noexcept { return descriptor_set_; }
  [[nodiscard]] Buffer& constants() noexcept { return constants_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptor_set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
  VkDescriptorSet descriptor_set_{VK_NULL_HANDLE};
  Buffer constants_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
