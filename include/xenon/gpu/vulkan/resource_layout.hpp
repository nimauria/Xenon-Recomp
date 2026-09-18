#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/memory.hpp"

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
  static constexpr std::uint32_t kFrameCount = CommandQueue::kFrameCount;

  ResourceLayout() = default;
  ~ResourceLayout();
  ResourceLayout(const ResourceLayout&) = delete;
  ResourceLayout& operator=(const ResourceLayout&) = delete;

  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device, VkDevice device);
  void reset() noexcept;
  [[nodiscard]] bool bind_guest_memory(VkBuffer buffer, VkDeviceSize size);
  [[nodiscard]] bool bind_texture(std::uint32_t slot, TextureDimension dimension,
                                  VkImageView view, VkSampler sampler);
  void prepare_draw(std::uint32_t frame_index, std::uint32_t draw_slot);

  [[nodiscard]] bool ready() const noexcept { return pipeline_layout_ != VK_NULL_HANDLE; }
  [[nodiscard]] VkDescriptorSetLayout descriptor_set_layout() const noexcept {
    return descriptor_set_layout_;
  }
  [[nodiscard]] VkPipelineLayout pipeline_layout() const noexcept {
    return pipeline_layout_;
  }
  [[nodiscard]] VkDescriptorSet descriptor_set(
      std::uint32_t frame_index, std::uint32_t draw_slot) const noexcept {
    return frame_index < frames_.size() && draw_slot < CommandQueue::kBatchCommandCount
               ? frames_[frame_index].descriptor_sets[draw_slot]
               : VK_NULL_HANDLE;
  }
  [[nodiscard]] std::span<std::byte> constants() noexcept {
    return constants_staging_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct FrameBindings {
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, CommandQueue::kBatchCommandCount> descriptor_sets{};
    std::array<Buffer, CommandQueue::kBatchCommandCount> constants{};
    std::array<std::span<std::byte>, CommandQueue::kBatchCommandCount>
        constants_mappings{};
  };
  struct TextureBinding {
    bool valid{};
    TextureDimension dimension{TextureDimension::TwoDOrStacked};
    VkImageView view{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
  };

  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptor_set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  std::array<FrameBindings, kFrameCount> frames_{};
  std::array<TextureBinding, 32> texture_bindings_{};
  std::array<std::byte, static_cast<std::size_t>(kConstantBufferBytes)>
      constants_staging_{};
  VkBuffer guest_memory_buffer_{VK_NULL_HANDLE};
  VkDeviceSize guest_memory_size_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
