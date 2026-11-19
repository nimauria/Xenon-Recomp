#include "xenon/gpu/vulkan/resource_layout.hpp"

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/vulkan/guest_memory_limits.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace xenon::gpu::vulkan {

ResourceLayout::~ResourceLayout() { reset(); }

bool ResourceLayout::initialize(VkPhysicalDevice physical_device, VkDevice device) {
  reset();
  error_.clear();
  if (!physical_device || !device) {
    error_ = "Vulkan resource layout requires physical and logical devices";
    return false;
  }
  device_ = device;
  {
    const auto limits = query_guest_memory_limits(physical_device);
    const auto plan =
        plan_guest_memory_segments(limits.max_storage_range, memory::kPhysicalMemorySize);
    if (!plan) {
      error_ = "Vulkan device storage buffers span " + std::to_string(limits.max_storage_range) +
               " bytes, too few to bind guest memory in " +
               std::to_string(GuestMemorySegmentation::kMaxSegments) + " segments";
      reset();
      return false;
    }
    if (plan->segmented() && (plan->segment_bytes % limits.storage_offset_alignment) != 0u) {
      error_ = "Vulkan guest-memory segments do not meet minStorageBufferOffsetAlignment";
      reset();
      return false;
    }
    segmentation_ = *plan;
  }
  // The geometry stage reads the draw state (point sprite sizes).
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                                    VK_SHADER_STAGE_FRAGMENT_BIT;
  std::vector<VkDescriptorSetLayoutBinding> bindings{
      VkDescriptorSetLayoutBinding{kConstantBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                   1, stages, nullptr},
      VkDescriptorSetLayoutBinding{kGuestMemoryBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   1, stages, nullptr},
      VkDescriptorSetLayoutBinding{kTexture1DBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   32, stages, nullptr},
      VkDescriptorSetLayoutBinding{kTexture2DBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   32, stages, nullptr},
      VkDescriptorSetLayoutBinding{kTexture3DBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   32, stages, nullptr},
      VkDescriptorSetLayoutBinding{kTextureCubeBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   32, stages, nullptr},
      VkDescriptorSetLayoutBinding{kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER,
                                   32, stages, nullptr},
      VkDescriptorSetLayoutBinding{kMemoryExportBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   1, stages, nullptr}};
  for (std::uint32_t segment = 1; segment < segmentation_.count; ++segment) {
    bindings.push_back({guest_memory_binding(segment), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                        stages, nullptr});
    bindings.push_back({memory_export_binding(segment), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                        stages, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                  &descriptor_set_layout_) != VK_SUCCESS) {
    error_ = "vkCreateDescriptorSetLayout failed for the Xenon resource ABI";
    reset();
    return false;
  }
  VkPipelineLayoutCreateInfo pipeline_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_info.setLayoutCount = 1;
  pipeline_info.pSetLayouts = &descriptor_set_layout_;
  if (vkCreatePipelineLayout(device_, &pipeline_info, nullptr,
                             &pipeline_layout_) != VK_SUCCESS) {
    error_ = "vkCreatePipelineLayout failed for the Xenon resource ABI";
    reset();
    return false;
  }

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physical_device, &properties);
  const auto alignment =
      (std::max)(VkDeviceSize(16), properties.limits.minUniformBufferOffsetAlignment);
  constant_stride_ = (kConstantBufferBytes + alignment - 1u) / alignment * alignment;
  for (auto& frame : frames_) {
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                             CommandQueue::kBatchCommandCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             2u * segmentation_.count * CommandQueue::kBatchCommandCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             128u * CommandQueue::kBatchCommandCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER,
                             32u * CommandQueue::kBatchCommandCount}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = CommandQueue::kBatchCommandCount;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr,
                               &frame.descriptor_pool) != VK_SUCCESS) {
      error_ = "vkCreateDescriptorPool failed for a Xenon submission frame";
      reset();
      return false;
    }
    std::array<VkDescriptorSetLayout, CommandQueue::kBatchCommandCount> layouts{};
    layouts.fill(descriptor_set_layout_);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = frame.descriptor_pool;
    allocate.descriptorSetCount = CommandQueue::kBatchCommandCount;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocate,
                                 frame.descriptor_sets.data()) != VK_SUCCESS) {
      error_ = "vkAllocateDescriptorSets failed for Xenon draw snapshots";
      reset();
      return false;
    }
    {
      if (!frame.constants.initialize(
              physical_device, device_, constant_stride_ * CommandQueue::kBatchCommandCount,
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
          !frame.constants.map(frame.constants_mapping)) {
        error_ = frame.constants.error();
        reset();
        return false;
      }
    }
  }
  constants_staging_.fill(std::byte{});
  last_set_ = {};
  ++binding_generation_;
  return true;
}

bool ResourceLayout::bind_guest_memory(VkBuffer buffer, VkDeviceSize size) {
  if (!device_ || !buffer || !size) {
    error_ = "Vulkan guest-memory descriptor requires an initialized layout and buffer";
    return false;
  }
  if (guest_memory_buffer_ != buffer || guest_memory_size_ != size) ++binding_generation_;
  guest_memory_buffer_ = buffer;
  guest_memory_size_ = size;
  return true;
}

bool ResourceLayout::bind_texture(std::uint32_t slot, TextureDimension dimension,
                                  VkImageView view, VkSampler sampler) {
  if (!device_ || slot >= texture_bindings_.size() || !view || !sampler) {
    error_ = "Vulkan texture binding requires a valid slot, image view and sampler";
    return false;
  }
  auto& binding = texture_bindings_[slot];
  if (!binding.valid || binding.dimension != dimension || binding.view != view ||
      binding.sampler != sampler)
    ++binding_generation_;
  binding = {true, dimension, view, sampler};
  return true;
}

ResourceLayout::DrawBinding ResourceLayout::prepare_draw(std::uint32_t frame_index,
                                                         std::uint32_t draw_slot) {
  DrawBinding result{};
  if (!device_ || frame_index >= frames_.size() ||
      draw_slot >= CommandQueue::kBatchCommandCount) return result;
  auto& frame = frames_[frame_index];
  const auto offset = static_cast<std::size_t>(constant_stride_ * draw_slot);
  std::copy(constants_staging_.begin(), constants_staging_.end(),
            frame.constants_mapping.begin() + static_cast<std::ptrdiff_t>(offset));
  result.constants_offset = static_cast<std::uint32_t>(offset);
  // A set written for an earlier slot of this batch (same frame, lower slot:
  // a new batch on this frame starts again at slot 0) at the current binding
  // generation is reused; slots only increase within a batch, so nothing
  // rewrites it while the GPU may still read it.
  if (last_set_.frame == frame_index && last_set_.slot < draw_slot &&
      last_set_.generation == binding_generation_) {
    last_set_.slot = draw_slot;
    result.set = last_set_.set;
    ++sets_reused_;
    perf::add(perf::Counter::BindingsReused, 1u);
    return result;
  }
  const auto descriptor_set = frame.descriptor_sets[draw_slot];
  write_set(descriptor_set, frame.constants.buffer());
  last_set_ = {frame_index, draw_slot, descriptor_set, binding_generation_};
  result.set = descriptor_set;
  ++sets_written_;
  perf::add(perf::Counter::BindingsWritten, 1u);
  return result;
}

void ResourceLayout::write_set(VkDescriptorSet descriptor_set, VkBuffer constants) {
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(3 + texture_bindings_.size() * 2);
  std::vector<VkDescriptorBufferInfo> buffers;
  buffers.reserve(1u + 2u * segmentation_.count);
  buffers.push_back({constants, 0, kConstantBufferBytes});
  VkWriteDescriptorSet constants_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  constants_write.dstSet = descriptor_set;
  constants_write.dstBinding = kConstantBinding;
  constants_write.descriptorCount = 1;
  constants_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  constants_write.pBufferInfo = &buffers.back();
  writes.push_back(constants_write);

  if (guest_memory_buffer_ && guest_memory_size_) {
    // One read and one read-write descriptor per segment (a single pair over
    // the whole buffer unless the device's range requires segments).
    for (std::uint32_t segment = 0; segment < segmentation_.count; ++segment) {
      const VkDeviceSize offset = segmentation_.segmented() ? segmentation_.offset(segment) : 0u;
      const VkDeviceSize range =
          segmentation_.segmented() ? segmentation_.range(segment) : guest_memory_size_;
      buffers.push_back({guest_memory_buffer_, offset, range});
      VkWriteDescriptorSet read_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      read_write.dstSet = descriptor_set;
      read_write.dstBinding = guest_memory_binding(segment);
      read_write.descriptorCount = 1;
      read_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      read_write.pBufferInfo = &buffers.back();
      writes.push_back(read_write);
      // The vector may reallocate after push_back, so repair pointers below
      // after all buffer infos are stable.
      buffers.push_back({guest_memory_buffer_, offset, range});
      VkWriteDescriptorSet export_write = read_write;
      export_write.dstBinding = memory_export_binding(segment);
      export_write.pBufferInfo = &buffers.back();
      writes.push_back(export_write);
    }
  }

  std::vector<VkDescriptorImageInfo> images;
  std::vector<VkDescriptorImageInfo> samplers;
  images.reserve(texture_bindings_.size());
  samplers.reserve(texture_bindings_.size());
  for (std::uint32_t slot = 0; slot < texture_bindings_.size(); ++slot) {
    const auto& binding = texture_bindings_[slot];
    if (!binding.valid) continue;
    std::uint32_t image_binding{};
    switch (binding.dimension) {
      case TextureDimension::OneD: image_binding = kTexture1DBinding; break;
      case TextureDimension::TwoDOrStacked: image_binding = kTexture2DBinding; break;
      case TextureDimension::ThreeD: image_binding = kTexture3DBinding; break;
      case TextureDimension::Cube: image_binding = kTextureCubeBinding; break;
    }
    images.push_back({VK_NULL_HANDLE, binding.view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet image_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    image_write.dstSet = descriptor_set;
    image_write.dstBinding = image_binding;
    image_write.dstArrayElement = slot;
    image_write.descriptorCount = 1;
    image_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    image_write.pImageInfo = &images.back();
    writes.push_back(image_write);

    samplers.push_back({binding.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    VkWriteDescriptorSet sampler_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    sampler_write.dstSet = descriptor_set;
    sampler_write.dstBinding = kSamplerBinding;
    sampler_write.dstArrayElement = slot;
    sampler_write.descriptorCount = 1;
    sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_write.pImageInfo = &samplers.back();
    writes.push_back(sampler_write);
  }

  // Repair pBufferInfo pointers after vector growth.
  std::size_t buffer_index = 0;
  for (auto& write : writes) {
    if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
        write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
      write.pBufferInfo = &buffers[buffer_index++];
    }
  }
  // Image vectors were reserved to their maximum size, so their element
  // addresses stay stable while writes are assembled.
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
}

void ResourceLayout::reset() noexcept {
  texture_bindings_.fill({});
  guest_memory_buffer_ = VK_NULL_HANDLE;
  guest_memory_size_ = 0;
  last_set_ = {};
  constants_staging_.fill(std::byte{});
  for (auto& frame : frames_) {
    frame.constants_mapping = {};
    frame.constants.reset();
    for (std::uint32_t draw = 0; draw < CommandQueue::kBatchCommandCount; ++draw)
      frame.descriptor_sets[draw] = VK_NULL_HANDLE;
    if (device_ && frame.descriptor_pool)
      vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
    frame.descriptor_pool = VK_NULL_HANDLE;
  }
  if (device_ && pipeline_layout_)
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (device_ && descriptor_set_layout_)
    vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
  pipeline_layout_ = VK_NULL_HANDLE;
  descriptor_set_layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

}  // namespace xenon::gpu::vulkan
