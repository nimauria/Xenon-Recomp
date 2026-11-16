#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/guest_memory_segments.hpp"
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
  // Segmented guest memory (XEN-001): read-only segment i >= 1 at
  // t(128 + i), past the texture arrays; read-write segment i at u(i).
  static constexpr std::uint32_t kGuestMemorySegmentBinding = 144;  // + segment
  [[nodiscard]] static constexpr std::uint32_t guest_memory_binding(std::uint32_t segment) noexcept {
    return segment ? kGuestMemorySegmentBinding + segment : kGuestMemoryBinding;
  }
  [[nodiscard]] static constexpr std::uint32_t memory_export_binding(std::uint32_t segment) noexcept {
    return kMemoryExportBinding + segment;
  }
  static constexpr VkDeviceSize kConstantBufferBytes = kShaderConstantBufferBytes;
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
  // Where a recorded draw finds its bindings: the descriptor set to bind and
  // the dynamic offset of its constants in the frame's constant buffer.
  struct DrawBinding {
    VkDescriptorSet set{VK_NULL_HANDLE};
    std::uint32_t constants_offset{};
  };
  // Snapshots the staged bindings and constants for the draw recorded in
  // `draw_slot` of the batch on `frame_index`. Slots increase within a batch.
  // The constants go to the slot's own range of the frame's constant buffer
  // (a dynamic uniform buffer); a draw whose textures, samplers and guest
  // memory binding have not changed since an earlier draw of the same batch
  // reuses that draw's descriptor set instead of writing its own.
  [[nodiscard]] DrawBinding prepare_draw(std::uint32_t frame_index, std::uint32_t draw_slot);
  [[nodiscard]] std::uint64_t sets_reused() const noexcept { return sets_reused_; }
  [[nodiscard]] std::uint64_t sets_written() const noexcept { return sets_written_; }

  [[nodiscard]] bool ready() const noexcept { return pipeline_layout_ != VK_NULL_HANDLE; }
  [[nodiscard]] VkDescriptorSetLayout descriptor_set_layout() const noexcept {
    return descriptor_set_layout_;
  }
  [[nodiscard]] VkPipelineLayout pipeline_layout() const noexcept {
    return pipeline_layout_;
  }
  [[nodiscard]] std::span<std::byte> constants() noexcept {
    return constants_staging_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // How guest memory is bound, from the device's storage-buffer range
  // (guest_memory_limits.hpp).
  [[nodiscard]] const GuestMemorySegmentation& segmentation() const noexcept {
    return segmentation_;
  }

 private:
  GuestMemorySegmentation segmentation_{};
  struct FrameBindings {
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, CommandQueue::kBatchCommandCount> descriptor_sets{};
    // Every draw slot's constants, kConstantStride apart.
    Buffer constants{};
    std::span<std::byte> constants_mapping{};
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
  // Bytes between draw slots' constants (minUniformBufferOffsetAlignment).
  VkDeviceSize constant_stride_{};
  // Bumped whenever a texture, sampler or the guest memory buffer the next
  // draw binds differs; a set written at the current generation is reused.
  std::uint64_t binding_generation_{1};
  struct LastSet {
    std::uint32_t frame{UINT32_MAX};
    std::uint32_t slot{};
    VkDescriptorSet set{VK_NULL_HANDLE};
    std::uint64_t generation{};
  };
  LastSet last_set_{};
  std::uint64_t sets_reused_{};
  std::uint64_t sets_written_{};
  void write_set(VkDescriptorSet set, VkBuffer constants);
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
