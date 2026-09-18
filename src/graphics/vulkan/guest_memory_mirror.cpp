#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"

#include <span>

namespace xenon::gpu::vulkan {

GuestMemoryMirror::~GuestMemoryMirror() { reset(); }

bool GuestMemoryMirror::initialize(VkPhysicalDevice physical_device,
                                   VkDevice device, CommandQueue& queue,
                                   memory::AddressSpace& memory) {
  reset();
  error_.clear();
  if (!mirror_.initialize(physical_device, device, memory::kPhysicalMemorySize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
      !upload_.initialize(physical_device, device, kUploadSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = "failed to allocate Vulkan Xbox physical-memory mirror";
    reset();
    return false;
  }
  memory_ = &memory;
  queue_ = &queue;
  // Start at epoch zero so the first synchronization uploads every page
  // dirtied by AddressSpace initialization/reset without a write callback.
  synchronized_epoch_ = 0;
  dirty_ranges_.clear();
  return true;
}

void GuestMemoryMirror::reset() noexcept {
  memory_ = nullptr;
  queue_ = nullptr;
  upload_.reset();
  mirror_.reset();
  dirty_ranges_.clear();
  synchronized_epoch_ = 0;
  shader_read_state_ = false;
}

bool GuestMemoryMirror::synchronize() {
  std::span<std::byte> upload_bytes;
  if (!memory_ || !queue_) {
    error_ = "Vulkan guest-memory mirror is not initialized";
    return false;
  }
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map Vulkan upload buffer";
    return false;
  }
  const auto through_epoch = memory_->coherency().current_epoch();
  memory_->coherency().collect_dirty_ranges(
      synchronized_epoch_, through_epoch, kUploadSize, dirty_ranges_);
  for (const auto& range : dirty_ranges_) {
    const auto address = range.address;
    const auto size = range.size;
    if (!memory_->copy_physical_range(address, upload_bytes.first(size))) {
      error_ = "failed to snapshot Xbox physical memory";
      return false;
    }
    if (!queue_->execute([&](VkCommandBuffer command) {
          VkBufferMemoryBarrier2 before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
          before.srcStageMask = shader_read_state_ ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                                   : VK_PIPELINE_STAGE_2_NONE;
          before.srcAccessMask = shader_read_state_ ? VK_ACCESS_2_MEMORY_READ_BIT : 0;
          before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
          before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
          before.buffer = mirror_.buffer();
          before.offset = address;
          before.size = size;
          VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          dependency.bufferMemoryBarrierCount = 1;
          dependency.pBufferMemoryBarriers = &before;
          vkCmdPipelineBarrier2(command, &dependency);
          VkBufferCopy copy{0, address, size};
          vkCmdCopyBuffer(command, upload_.buffer(), mirror_.buffer(), 1, &copy);
          VkBufferMemoryBarrier2 after{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
          after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
          after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
          after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
          after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
          after.buffer = mirror_.buffer();
          after.offset = address;
          after.size = size;
          dependency.pBufferMemoryBarriers = &after;
          vkCmdPipelineBarrier2(command, &dependency);
        })) {
      error_ = queue_->error();
      return false;
    }
    shader_read_state_ = true;
  }
  // Publish consumption only after every queued upload has completed. If any
  // range failed, the old epoch is retained and the next call retries it.
  synchronized_epoch_ = through_epoch;
  return true;
}

}  // namespace xenon::gpu::vulkan
