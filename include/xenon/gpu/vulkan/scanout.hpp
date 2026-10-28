#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/scanout.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"

namespace xenon::gpu::vulkan {

// Draws a front buffer into a host color attachment on the GPU: one
// fullscreen pass whose fragment shader (make_scanout_shader) reads the front
// buffer from the guest-memory mirror, untiles, converts and filters it. No
// CPU readback, decode or scaling is involved. Pipelines are built once per
// attachment format.
class ScanoutRenderer {
 public:
  ScanoutRenderer() = default;
  ~ScanoutRenderer();
  ScanoutRenderer(const ScanoutRenderer&) = delete;
  ScanoutRenderer& operator=(const ScanoutRenderer&) = delete;

  // Builds the pipeline for `format` and the mirror's descriptor set if
  // needed; call before record().
  [[nodiscard]] bool prepare(VkDevice device, VkFormat format, GuestMemoryMirror& mirror);
  // Records the pass into `target`, which must be in COLOR_ATTACHMENT_OPTIMAL
  // layout and of a format passed to prepare().
  void record(VkCommandBuffer command, GuestMemoryMirror& mirror, VkImageView target,
              VkFormat format, const ScanoutConstants& constants);
  // Draws into an offscreen RGBA8 image and reads it back: a screenshot of
  // scan-out at the constants' target size.
  [[nodiscard]] bool capture(VkPhysicalDevice physical_device, VkDevice device,
                             CommandQueue& queue, GuestMemoryMirror& mirror,
                             const ScanoutConstants& constants,
                             PreparedPresentationFrame& output);
  void reset() noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] bool create_layouts(VkDevice device);

  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkDescriptorPool pool_{VK_NULL_HANDLE};
  VkDescriptorSet set_{VK_NULL_HANDLE};
  VkBuffer set_buffer_{VK_NULL_HANDLE};
  std::unordered_map<std::uint32_t, VkPipeline> pipelines_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
