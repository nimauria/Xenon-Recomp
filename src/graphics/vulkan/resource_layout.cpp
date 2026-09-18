#include "xenon/gpu/vulkan/resource_layout.hpp"

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
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
                                    VK_SHADER_STAGE_FRAGMENT_BIT;
  const std::array bindings{
      VkDescriptorSetLayoutBinding{kConstantBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
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

  for (auto& frame : frames_) {
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             CommandQueue::kBatchCommandCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             2u * CommandQueue::kBatchCommandCount},
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
    for (std::uint32_t draw = 0; draw < CommandQueue::kBatchCommandCount; ++draw) {
      if (!frame.constants[draw].initialize(
              physical_device, device_, kConstantBufferBytes,
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
          !frame.constants[draw].map(frame.constants_mappings[draw])) {
        error_ = frame.constants[draw].error();
        reset();
        return false;
      }
    }
  }
  constants_staging_.fill(std::byte{});
  return true;
}

bool ResourceLayout::bind_guest_memory(VkBuffer buffer, VkDeviceSize size) {
  if (!device_ || !buffer || !size) {
    error_ = "Vulkan guest-memory descriptor requires an initialized layout and buffer";
    return false;
  }
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
  texture_bindings_[slot] = {true, dimension, view, sampler};
  return true;
}

void ResourceLayout::prepare_draw(std::uint32_t frame_index,
                                  std::uint32_t draw_slot) {
  if (!device_ || frame_index >= frames_.size() ||
      draw_slot >= CommandQueue::kBatchCommandCount) return;
  auto& frame = frames_[frame_index];
  const auto descriptor_set = frame.descriptor_sets[draw_slot];
  std::copy(constants_staging_.begin(), constants_staging_.end(),
            frame.constants_mappings[draw_slot].begin());

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(3 + texture_bindings_.size() * 2);
  std::vector<VkDescriptorBufferInfo> buffers;
  buffers.reserve(3);
  buffers.push_back({frame.constants[draw_slot].buffer(), 0, kConstantBufferBytes});
  VkWriteDescriptorSet constants_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  constants_write.dstSet = descriptor_set;
  constants_write.dstBinding = kConstantBinding;
  constants_write.descriptorCount = 1;
  constants_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  constants_write.pBufferInfo = &buffers.back();
  writes.push_back(constants_write);

  if (guest_memory_buffer_ && guest_memory_size_) {
    buffers.push_back({guest_memory_buffer_, 0, guest_memory_size_});
    VkWriteDescriptorSet read_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    read_write.dstSet = descriptor_set;
    read_write.dstBinding = kGuestMemoryBinding;
    read_write.descriptorCount = 1;
    read_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    read_write.pBufferInfo = &buffers.back();
    writes.push_back(read_write);
    // The vector may reallocate after push_back, so repair pointers below after
    // all buffer infos are stable.
    buffers.push_back({guest_memory_buffer_, 0, guest_memory_size_});
    VkWriteDescriptorSet export_write = read_write;
    export_write.dstBinding = kMemoryExportBinding;
    export_write.pBufferInfo = &buffers.back();
    writes.push_back(export_write);
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
    if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
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
  constants_staging_.fill(std::byte{});
  for (auto& frame : frames_) {
    for (std::uint32_t draw = 0; draw < CommandQueue::kBatchCommandCount; ++draw) {
      frame.constants_mappings[draw] = {};
      frame.constants[draw].reset();
      frame.descriptor_sets[draw] = VK_NULL_HANDLE;
    }
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
