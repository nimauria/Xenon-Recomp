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
      !readback_.initialize(physical_device, device, kReadbackSize,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = "failed to allocate Vulkan Xbox physical-memory mirror transfers";
    reset();
    return false;
  }
  memory_ = &memory;
  queue_ = &queue;
  coherency_.reset(memory::kPhysicalMemorySize, false);
  upload_ring_.reset(kTransferSize);
  readback_ring_.reset(kReadbackSize);
  pending_readbacks_.clear();
  published_writes_.clear();
  return true;
}

void GuestMemoryMirror::reset() noexcept {
  memory_ = nullptr;
  queue_ = nullptr;
  readback_.reset();
  upload_.reset();
  mirror_.reset();
  coherency_.clear();
  upload_ring_.reset(0);
  readback_ring_.reset(0);
  pending_readbacks_.clear();
  shader_read_state_ = false;
  shader_write_state_ = false;
}

bool GuestMemoryMirror::synchronize() {
  return synchronize_range(0u, memory::kPhysicalMemorySize,
                           memory::GpuRangeUsage::Unrestricted);
}

bool GuestMemoryMirror::synchronize_range(
    std::uint32_t address, std::uint32_t width,
    memory::GpuRangeUsage usage, std::uint64_t read_epoch) {
  if (!memory_ || !queue_) {
    error_ = "Vulkan guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_upload(memory_->coherency(), address, width,
                                            kTransferSize, usage, read_epoch);
  if (plan.ranges.empty()) return true;

  std::span<std::byte> upload_bytes;
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map Vulkan upload buffer";
    return false;
  }

  // Each range is staged in its own region of the upload ring and copied in
  // queue order, ahead of the work that reads it; the CPU waits only when the
  // ring wraps onto a region the GPU has not copied yet.
  const auto wait = [&](std::uint64_t value) {
    return queue_->flush() && queue_->wait_for_completion(value);
  };
  for (const auto& range : plan.ranges) {
      const auto chunk_address = range.address;
      const auto chunk_size = range.size;
      const auto offset = upload_ring_.reserve(chunk_size, queue_->completed_value(), wait);
      if (!offset) {
        error_ = queue_->error().empty() ? "Vulkan upload ring reservation failed"
                                         : queue_->error();
        return false;
      }
      coherency_.commit_cpu_upload(chunk_address, chunk_size);
      if (!memory_->copy_physical_range(
              chunk_address, upload_bytes.subspan(*offset, chunk_size))) {
        coherency_.rollback_cpu_upload(chunk_address, chunk_size);
        error_ = "failed to snapshot Xbox physical memory";
        return false;
      }
      if (!queue_->execute_async([&](VkCommandBuffer command, std::uint32_t,
                                     std::uint32_t) {
            // Earlier work in this batch or a previous one may still read or
            // write the range.
            VkBufferMemoryBarrier2 before{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            before.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            before.buffer = mirror_.buffer();
            before.offset = chunk_address;
            before.size = chunk_size;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &before;
            vkCmdPipelineBarrier2(command, &dependency);
            VkBufferCopy copy{*offset, chunk_address, chunk_size};
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
      upload_ring_.commit(*offset, chunk_size, queue_->retirement_value());
      shader_read_state_ = true;
      shader_write_state_ = false;
  }
  return true;
}

void GuestMemoryMirror::mark_gpu_write(std::uint32_t address,
                                       std::uint32_t width) {
  if (memory_) {
    coherency_.mark_gpu_write(memory_->coherency(), address, width);
  } else {
    coherency_.mark_gpu_write(address, width);
  }
}

bool GuestMemoryMirror::has_gpu_dirty(std::uint32_t address,
                                      std::uint32_t width) const {
  return coherency_.has_gpu_dirty(address, width);
}

bool GuestMemoryMirror::make_cpu_visible(
    std::uint32_t address, std::uint32_t width,
    memory::GpuRangeUsage usage) {
  // Bytes the GPU never wrote are already current in guest RAM: no readback,
  // and no wait for readbacks of other ranges (a range with one in flight
  // stays GPU-dirty until it commits). Every indexed draw asks for its index
  // buffer, and waiting here cost a GPU round trip after every resolve.
  if (!coherency_.has_gpu_dirty(address, width)) return true;
  // Queued readbacks first, so this one is not planned and copied twice.
  return complete_cpu_visibility(true) && queue_cpu_visibility(address, width, usage) &&
         complete_cpu_visibility(true);
}

bool GuestMemoryMirror::queue_cpu_visibility(
    std::uint32_t address, std::uint32_t width,
    memory::GpuRangeUsage usage) {
  if (!memory_ || !queue_) {
    error_ = "Vulkan guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_readback(memory_->coherency(), address, width,
                                             kTransferSize, usage);
  for (const auto& range : plan.ranges) {
    // Readbacks the GPU has finished are written out before the ring can hand
    // their regions out again; the same completed value bounds both.
    const auto completed = queue_->completed_value();
    if (!complete_through(completed)) return false;
    const auto offset = readback_ring_.reserve(range.size, completed, [&](std::uint64_t value) {
      return queue_->flush() && queue_->wait_for_completion(value) && complete_through(value);
    });
    if (!offset) {
      if (error_.empty())
        error_ = queue_->error().empty() ? "Vulkan readback ring reservation failed"
                                         : queue_->error();
      return false;
    }
    const bool read_state = shader_read_state_;
    const bool write_state = shader_write_state_;
    if (!queue_->execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
          // Earlier shader or copy work in this batch or a previous one
          // wrote the range (a resolve, a memory export).
          VkBufferMemoryBarrier2 before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
          before.srcStageMask = (read_state || write_state) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                                            : VK_PIPELINE_STAGE_2_NONE;
          before.srcAccessMask = (read_state ? VK_ACCESS_2_MEMORY_READ_BIT : 0) |
                                 (write_state ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0);
          before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
          before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
          before.buffer = mirror_.buffer();
          before.offset = range.address;
          before.size = range.size;
          VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          dependency.bufferMemoryBarrierCount = 1;
          dependency.pBufferMemoryBarriers = &before;
          vkCmdPipelineBarrier2(command, &dependency);
          VkBufferCopy copy{range.address, *offset, range.size};
          vkCmdCopyBuffer(command, mirror_.buffer(), readback_.buffer(), 1, &copy);
          VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
          host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
          host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
          host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
          host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
          host.buffer = readback_.buffer();
          host.offset = *offset;
          host.size = range.size;
          dependency.pBufferMemoryBarriers = &host;
          vkCmdPipelineBarrier2(command, &dependency);
        })) {
      error_ = queue_->error();
      return false;
    }
    // The copy reads the mirror: later writes must wait for it.
    shader_read_state_ = true;
    const auto value = queue_->retirement_value();
    readback_ring_.commit(*offset, range.size, value);
    pending_readbacks_.push_back({range, *offset, value});
  }
  return true;
}

bool GuestMemoryMirror::complete_cpu_visibility(bool wait) {
  if (pending_readbacks_.empty()) return true;
  if (!memory_ || !queue_) {
    error_ = "Vulkan guest-memory mirror is not initialized";
    return false;
  }
  if (wait) {
    const auto last = pending_readbacks_.back().value;
    if (queue_->completed_value() < last &&
        !(queue_->flush() && queue_->wait_for_completion(last))) {
      error_ = queue_->error();
      return false;
    }
  }
  return complete_through(queue_->completed_value());
}

bool GuestMemoryMirror::complete_through(std::uint64_t completed) {
  if (pending_readbacks_.empty() || pending_readbacks_.front().value > completed) return true;
  std::span<std::byte> readback_bytes;
  if (!readback_.map(readback_bytes)) {
    error_ = "failed to map Vulkan guest-memory readback buffer";
    return false;
  }
  // CPU threads may have written the same Xbox bytes while the copies were in
  // flight. Xenon Memory briefly quiesces physical writers, re-ingests those
  // CPU publications and applies only the subranges still owned by each
  // readback's exact GPU generation.
  auto write_window = memory_->physical_write_window();
  if (!write_window) {
    error_ = "failed to acquire Vulkan guest-memory readback window";
    return false;
  }
  while (!pending_readbacks_.empty() && pending_readbacks_.front().value <= completed) {
    const auto pending = pending_readbacks_.front();
    pending_readbacks_.pop_front();
    const auto safe_ranges =
        coherency_.prepare_gpu_download(memory_->coherency(), pending.range, kTransferSize);
    for (const auto& safe : safe_ranges) {
      const auto offset = pending.offset + (safe.address - pending.range.address);
      const auto previous_epoch =
          memory_->coherency().newest_page_epoch(safe.address, safe.size);
      std::uint64_t publication_epoch = 0u;
      if (!write_window.write(safe.address, readback_bytes.subspan(offset, safe.size),
                              &publication_epoch)) {
        error_ = "Vulkan guest-memory readback destination is outside physical RAM";
        return false;
      }
      (void)coherency_.commit_gpu_download(memory_->coherency(), safe, safe.address, safe.size,
                                           publication_epoch);
      if (publication_epoch)
        published_writes_.push_back({publication_epoch, previous_epoch, safe.address, safe.size});
    }
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
  // GPU resolves write the mirror from compute.
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
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
