#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"

#include <algorithm>
#include <cstring>
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
      !upload_.initialize(physical_device, device, kTransferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
      !readback_.initialize(physical_device, device, kTransferSize,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = "failed to allocate Vulkan Xbox physical-memory mirror transfers";
    reset();
    return false;
  }
  memory_ = &memory;
  queue_ = &queue;
  coherency_.reset(memory::kPhysicalMemorySize, true);
  return true;
}

void GuestMemoryMirror::reset() noexcept {
  memory_ = nullptr;
  queue_ = nullptr;
  readback_.reset();
  upload_.reset();
  mirror_.reset();
  coherency_.clear();
  shader_read_state_ = false;
  shader_write_state_ = false;
}

bool GuestMemoryMirror::synchronize() {
  return synchronize_range(0u, memory::kPhysicalMemorySize);
}

bool GuestMemoryMirror::synchronize_range(std::uint32_t address,
                                          std::uint32_t width) {
  if (!memory_ || !queue_) {
    error_ = "Vulkan guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_upload(memory_->coherency(), address, width,
                                            kTransferSize);
  if (plan.ranges.empty()) return true;

  std::span<std::byte> upload_bytes;
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map Vulkan upload buffer";
    return false;
  }

  for (const auto& range : plan.ranges) {
      const auto chunk_address = range.address;
      const auto chunk_size = range.size;
      coherency_.commit_cpu_upload(chunk_address, chunk_size);
      if (!memory_->copy_physical_range(
              chunk_address, upload_bytes.first(chunk_size))) {
        coherency_.rollback_cpu_upload(chunk_address, chunk_size);
        error_ = "failed to snapshot Xbox physical memory";
        return false;
      }
      if (!queue_->execute([&](VkCommandBuffer command) {
            VkBufferMemoryBarrier2 before{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            before.srcStageMask = (shader_read_state_ || shader_write_state_)
                                      ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                      : VK_PIPELINE_STAGE_2_NONE;
            before.srcAccessMask =
                (shader_read_state_ ? VK_ACCESS_2_MEMORY_READ_BIT : 0) |
                (shader_write_state_ ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0);
            before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            before.buffer = mirror_.buffer();
            before.offset = chunk_address;
            before.size = chunk_size;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &before;
            vkCmdPipelineBarrier2(command, &dependency);
            VkBufferCopy copy{0, chunk_address, chunk_size};
            vkCmdCopyBuffer(command, upload_.buffer(), mirror_.buffer(), 1,
                            &copy);
            VkBufferMemoryBarrier2 after{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            after.buffer = mirror_.buffer();
            after.offset = chunk_address;
            after.size = chunk_size;
            dependency.pBufferMemoryBarriers = &after;
            vkCmdPipelineBarrier2(command, &dependency);
          })) {
        coherency_.rollback_cpu_upload(chunk_address, chunk_size);
        error_ = queue_->error();
        return false;
      }
      shader_read_state_ = true;
      shader_write_state_ = false;
  }
  return true;
}

void GuestMemoryMirror::mark_gpu_write(std::uint32_t address,
                                       std::uint32_t width) {
  coherency_.mark_gpu_write(address, width);
}

bool GuestMemoryMirror::has_gpu_dirty(std::uint32_t address,
                                      std::uint32_t width) const {
  return coherency_.has_gpu_dirty(address, width);
}

bool GuestMemoryMirror::make_cpu_visible(std::uint32_t address,
                                         std::uint32_t width) {
  if (!memory_ || !queue_) {
    error_ = "Vulkan guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_readback(address, width, kTransferSize);
  if (plan.ranges.empty()) return true;

  std::span<std::byte> readback_bytes;
  if (!readback_.map(readback_bytes)) {
    error_ = "failed to map Vulkan guest-memory readback buffer";
    return false;
  }

  for (const auto& range : plan.ranges) {
      const auto chunk_address = range.address;
      const auto chunk_size = range.size;
      if (!queue_->execute([&](VkCommandBuffer command) {
            VkBufferMemoryBarrier2 before{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            before.srcStageMask = (shader_read_state_ || shader_write_state_)
                                      ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                      : VK_PIPELINE_STAGE_2_NONE;
            before.srcAccessMask =
                (shader_read_state_ ? VK_ACCESS_2_MEMORY_READ_BIT : 0) |
                (shader_write_state_ ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0);
            before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            before.buffer = mirror_.buffer();
            before.offset = chunk_address;
            before.size = chunk_size;

            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &before;
            vkCmdPipelineBarrier2(command, &dependency);

            VkBufferCopy copy{chunk_address, 0, chunk_size};
            vkCmdCopyBuffer(command, mirror_.buffer(), readback_.buffer(), 1,
                            &copy);

            VkBufferMemoryBarrier2 host{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            host.buffer = readback_.buffer();
            host.offset = 0;
            host.size = chunk_size;
            dependency.pBufferMemoryBarriers = &host;
            vkCmdPipelineBarrier2(command, &dependency);
          })) {
        error_ = queue_->error();
        return false;
      }

      // The queue helper is synchronous, so no shader access remains in flight.
      // The next prepare_shader_access call establishes the next GPU dependency.
      shader_read_state_ = false;
      shader_write_state_ = false;

      std::uint64_t publication_epoch = 0u;
      if (!memory_->write_physical(
              chunk_address, readback_bytes.first(chunk_size),
              &publication_epoch)) {
        error_ = "Vulkan guest-memory readback destination is outside physical RAM";
        return false;
      }
      (void)coherency_.commit_gpu_download(
          memory_->coherency(), plan, chunk_address, chunk_size,
          publication_epoch);
  }
  return true;
}

void GuestMemoryMirror::prepare_shader_access(VkCommandBuffer command,
                                               bool writable) {
  if (!command || !mirror_.buffer()) return;

  VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  barrier.srcStageMask = (shader_read_state_ || shader_write_state_)
                             ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                             : VK_PIPELINE_STAGE_2_NONE;
  barrier.srcAccessMask =
      (shader_read_state_ ? VK_ACCESS_2_MEMORY_READ_BIT : 0) |
      (shader_write_state_ ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0);
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                          (writable ? VK_ACCESS_2_SHADER_WRITE_BIT : 0);
  barrier.buffer = mirror_.buffer();
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;

  // Keep an explicit shader-to-shader dependency even when the access class
  // doesn't change. Consecutive memexport draws otherwise have no transition
  // that makes the previous shader writes visible to a later vertex fetch.
  if (barrier.srcStageMask != VK_PIPELINE_STAGE_2_NONE || writable) {
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
  }
  shader_read_state_ = true;
  shader_write_state_ = writable;
}

}  // namespace xenon::gpu::vulkan
