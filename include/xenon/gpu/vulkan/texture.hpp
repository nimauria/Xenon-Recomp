#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "xenon/gpu/staging_ring.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/memory.hpp"

namespace xenon::gpu::vulkan {

[[nodiscard]] VkFormat host_texture_format(TextureHostFormat format) noexcept;

class TextureImage {
 public:
  ~TextureImage();
  TextureImage() = default;
  TextureImage(const TextureImage&) = delete;
  TextureImage& operator=(const TextureImage&) = delete;

  // Creates the host image, view and sampler for `layout`, without contents
  // (layout UNDEFINED).
  [[nodiscard]] bool create(VkPhysicalDevice physical_device, VkDevice device,
                            const TextureLayout& layout);
  // Whether this image can hold `layout` (same format, extent, dimension and
  // mip chain), so reloading a texture reuses it instead of a new image.
  [[nodiscard]] bool matches(const TextureLayout& layout) const noexcept;
  // Creates the image and uploads CPU-decoded texels (decode_texture()),
  // waiting for the copy. Tools and tests use it; the runtime loads textures
  // with GpuTextureLoader.
  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device, VkDevice device,
                                CommandQueue& queue,
                                const DecodedTexture& texture);
  void reset() noexcept;
  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView view() const noexcept { return view_; }
  [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] VkImageAspectFlags aspect() const noexcept { return aspect_; }
  [[nodiscard]] std::uint32_t mip_levels() const noexcept { return mip_levels_; }
  [[nodiscard]] std::uint32_t layers() const noexcept { return layers_; }
  // The layout the recorded work leaves the image in.
  [[nodiscard]] VkImageLayout layout() const noexcept { return layout_; }
  void set_layout(VkImageLayout layout) noexcept { layout_ = layout; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Part 7 of the AC6 Runtime Readiness pass: structured signals alongside
  // error(), set exactly when create() failed/degraded for that specific,
  // distinguishable reason - without the caller having to string-match
  // error().
  [[nodiscard]] bool unsupported_format() const noexcept { return unsupported_format_; }
  [[nodiscard]] std::uint64_t unsupported_sampler_behaviors() const noexcept {
    return unsupported_sampler_behaviors_;
  }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkSampler sampler_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  VkImageAspectFlags aspect_{VK_IMAGE_ASPECT_COLOR_BIT};
  VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  TextureDescriptor descriptor_{};
  std::uint32_t mip_levels_{};
  std::uint32_t layers_{};
  std::string error_{};
  bool unsupported_format_{};
  std::uint64_t unsupported_sampler_behaviors_{};
};

// Textures loaded on the GPU (see GpuTextureLoadConstants): a compute pass
// untiles and converts the guest texels into a staging buffer, and a copy
// moves them into the image. Everything is recorded into the queue's open
// batch, ahead of the draw that samples the image; nothing waits.
class GpuTextureLoader {
 public:
  GpuTextureLoader() = default;
  ~GpuTextureLoader();
  GpuTextureLoader(const GpuTextureLoader&) = delete;
  GpuTextureLoader& operator=(const GpuTextureLoader&) = delete;

  // From the guest-memory mirror, which must hold the texture's bytes
  // (GuestMemoryMirror::synchronize_range()).
  [[nodiscard]] bool load_from_mirror(VkPhysicalDevice physical_device, VkDevice device,
                                      CommandQueue& queue, GuestMemoryMirror& mirror,
                                      TextureImage& image, const TextureLayout& layout);
  // From `bytes`, a copy of guest memory [base, base + bytes.size()) taken
  // at a sync point.
  [[nodiscard]] bool load_from_bytes(VkPhysicalDevice physical_device, VkDevice device,
                                     CommandQueue& queue, TextureImage& image,
                                     const TextureLayout& layout,
                                     std::span<const std::byte> bytes, std::uint32_t base);
  void reset() noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  static constexpr VkDeviceSize kStagingSize = 64ull * 1024ull * 1024ull;
  static constexpr VkDeviceSize kUploadSize = 32ull * 1024ull * 1024ull;
  static constexpr std::uint32_t kTransientSets = 64u;

  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device, VkDevice device);
  [[nodiscard]] VkDescriptorSet source_set(VkDescriptorSet& set, VkBuffer& bound,
                                           VkBuffer source);
  [[nodiscard]] bool run(CommandQueue& queue, TextureImage& image, const TextureLayout& layout,
                         VkDescriptorSet set, std::uint32_t source_bias,
                         std::uint32_t source_base, std::uint32_t source_limit,
                         GuestMemoryMirror* mirror);
  void retire(std::uint64_t completed);

  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline pipeline_{VK_NULL_HANDLE};
  VkDescriptorPool pool_{VK_NULL_HANDLE};
  VkDescriptorSet mirror_set_{VK_NULL_HANDLE};
  VkBuffer mirror_set_buffer_{VK_NULL_HANDLE};
  VkDescriptorSet upload_set_{VK_NULL_HANDLE};
  VkBuffer upload_set_buffer_{VK_NULL_HANDLE};
  Buffer staging_{};
  StagingRing staging_ring_{};
  // The staging buffer's last use: a copy reads it before the next dispatch
  // writes it.
  bool staging_copied_{};
  Buffer upload_{};
  StagingRing upload_ring_{};
  // Uploads too large for the ring, with their descriptor sets, kept until
  // the GPU has read them.
  struct Transient {
    std::unique_ptr<Buffer> buffer{};
    VkDescriptorSet set{VK_NULL_HANDLE};
    std::uint64_t value{};
  };
  std::vector<Transient> transients_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
