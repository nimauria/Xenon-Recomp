#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"

#include <algorithm>
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
  dirty_pages_.assign(memory::kPhysicalMemorySize / kPageSize, 1);
  callback_id_ = memory.add_physical_write_callback(
      [this](std::uint32_t address, std::uint32_t width) {
        mark_dirty(address, width);
      });
  return true;
}

void GuestMemoryMirror::reset() noexcept {
  if (memory_ && callback_id_) memory_->remove_physical_write_callback(callback_id_);
  callback_id_ = 0;
  memory_ = nullptr;
  queue_ = nullptr;
  upload_.reset();
  mirror_.reset();
  dirty_pages_.clear();
  shader_read_state_ = false;
}

void GuestMemoryMirror::mark_dirty(std::uint32_t address,
                                   std::uint32_t width) {
  if (!width || address >= memory::kPhysicalMemorySize) return;
  const auto first = address / kPageSize;
  const auto end = std::min<std::uint64_t>(
      std::uint64_t{address} + width, memory::kPhysicalMemorySize);
  const auto last = static_cast<std::uint32_t>((end - 1u) / kPageSize);
  std::lock_guard lock(dirty_mutex_);
  std::fill(dirty_pages_.begin() + first, dirty_pages_.begin() + last + 1u,
            std::uint8_t{1});
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
  for (std::uint32_t page = 0; page < dirty_pages_.size();) {
    std::uint32_t first = page;
    std::uint32_t count = 0;
    {
      std::lock_guard lock(dirty_mutex_);
      while (first < dirty_pages_.size() && !dirty_pages_[first]) ++first;
      if (first == dirty_pages_.size()) break;
      const auto max_pages = kUploadSize / kPageSize;
      while (first + count < dirty_pages_.size() && count < max_pages &&
             dirty_pages_[first + count]) {
        dirty_pages_[first + count] = 0;
        ++count;
      }
    }
    const auto address = first * kPageSize;
    const auto size = count * kPageSize;
    if (!memory_->copy_physical_range(address, upload_bytes.first(size))) {
      std::lock_guard lock(dirty_mutex_);
      std::fill(dirty_pages_.begin() + first,
                dirty_pages_.begin() + first + count, std::uint8_t{1});
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
      std::lock_guard lock(dirty_mutex_);
      std::fill(dirty_pages_.begin() + first,
                dirty_pages_.begin() + first + count, std::uint8_t{1});
      error_ = queue_->error();
      return false;
    }
    shader_read_state_ = true;
    page = first + count;
  }
  return true;
}

}  // namespace xenon::gpu::vulkan
