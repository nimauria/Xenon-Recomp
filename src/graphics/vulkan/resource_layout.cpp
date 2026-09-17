#include "xenon/gpu/vulkan/resource_layout.hpp"

#include <array>

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
  const std::array pool_sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 32}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    error_ = "vkCreateDescriptorPool failed for the Xenon resource ABI";
    reset();
    return false;
  }
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = descriptor_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &descriptor_set_layout_;
  if (vkAllocateDescriptorSets(device_, &allocate, &descriptor_set_) != VK_SUCCESS) {
    error_ = "vkAllocateDescriptorSets failed for the Xenon resource ABI";
    reset();
    return false;
  }
  if (!constants_.initialize(physical_device, device_, kConstantBufferBytes,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = constants_.error();
    reset();
    return false;
  }
  VkDescriptorBufferInfo constants_info{constants_.buffer(), 0, kConstantBufferBytes};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = descriptor_set_;
  write.dstBinding = kConstantBinding;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.pBufferInfo = &constants_info;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  return true;
}

bool ResourceLayout::bind_guest_memory(VkBuffer buffer, VkDeviceSize size) {
  if (!device_ || !descriptor_set_ || !buffer || !size) {
    error_ = "Vulkan guest-memory descriptor requires an initialized layout and buffer";
    return false;
  }
  VkDescriptorBufferInfo buffer_info{buffer, 0, size};
  std::array writes{
      VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
      VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  for (auto& write : writes) {
    write.dstSet = descriptor_set_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
  }
  writes[0].dstBinding = kGuestMemoryBinding;
  writes[1].dstBinding = kMemoryExportBinding;
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
  return true;
}

bool ResourceLayout::bind_texture(std::uint32_t slot, TextureDimension dimension,
                                  VkImageView view, VkSampler sampler) {
  if (!device_ || !descriptor_set_ || slot >= 32 || !view || !sampler) {
    error_ = "Vulkan texture binding requires a valid slot, image view and sampler";
    return false;
  }
  std::uint32_t binding{};
  switch (dimension) {
    case TextureDimension::OneD: binding = kTexture1DBinding; break;
    case TextureDimension::TwoDOrStacked: binding = kTexture2DBinding; break;
    case TextureDimension::ThreeD: binding = kTexture3DBinding; break;
    case TextureDimension::Cube: binding = kTextureCubeBinding; break;
  }
  VkDescriptorImageInfo image_info{};
  image_info.imageView = view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo sampler_info{};
  sampler_info.sampler = sampler;
  std::array writes{VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                    VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  writes[0].dstSet = descriptor_set_;
  writes[0].dstBinding = binding;
  writes[0].dstArrayElement = slot;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &image_info;
  writes[1].dstSet = descriptor_set_;
  writes[1].dstBinding = kSamplerBinding;
  writes[1].dstArrayElement = slot;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[1].pImageInfo = &sampler_info;
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
  return true;
}

void ResourceLayout::reset() noexcept {
  if (device_ && descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  constants_.reset();
  if (device_ && pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (device_ && descriptor_set_layout_)
    vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
  descriptor_set_ = VK_NULL_HANDLE;
  descriptor_pool_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  descriptor_set_layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

}  // namespace xenon::gpu::vulkan
