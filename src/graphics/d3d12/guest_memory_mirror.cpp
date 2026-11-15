#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

namespace xenon::gpu::d3d12 {

GuestMemoryMirror::~GuestMemoryMirror() { reset(); }

bool GuestMemoryMirror::initialize(ID3D12Device* device, CommandQueue& queue,
                                   memory::AddressSpace& memory) {
  reset();
  error_.clear();
  if (!mirror_.initialize(device, memory::kPhysicalMemorySize,
                          D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
      !upload_.initialize(device, kTransferSize, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ) ||
      !readback_.initialize(device, kReadbackSize, D3D12_HEAP_TYPE_READBACK,
                            D3D12_RESOURCE_STATE_COPY_DEST)) {
    error_ = "failed to allocate D3D12 Xbox physical-memory mirror transfers";
    reset();
    return false;
  }
  memory_ = &memory;
  queue_ = &queue;
  coherency_.reset(memory::kPhysicalMemorySize, false);
  upload_ring_.reset(kTransferSize);
  readback_ring_.reset(kReadbackSize);
  std::span<std::byte> readback_bytes;
  if (!readback_.map(readback_bytes)) {
    error_ = "failed to map D3D12 guest-memory readback buffer";
    reset();
    return false;
  }
  publisher_.start(memory, coherency_, readback_bytes, kTransferSize,
                   [&queue](std::uint64_t value, std::uint32_t timeout_ms) {
                     return queue.wait_completed_concurrent(value, timeout_ms);
                   });
  return true;
}

void GuestMemoryMirror::reset() noexcept {
  // The publisher reads readback_ and coherency_; it stops first.
  publisher_.stop();
  memory_ = nullptr;
  queue_ = nullptr;
  readback_.reset();
  upload_.reset();
  mirror_.reset();
  coherency_.clear();
  upload_ring_.reset(0);
  readback_ring_.reset(0);
  state_ = D3D12_RESOURCE_STATE_COPY_DEST;
}

bool GuestMemoryMirror::synchronize() {
  return synchronize_range(0u, memory::kPhysicalMemorySize,
                           memory::GpuRangeUsage::Unrestricted);
}

bool GuestMemoryMirror::synchronize_range(
    std::uint32_t address, std::uint32_t width,
    memory::GpuRangeUsage usage, std::uint64_t read_epoch) {
  if (!memory_ || !queue_) {
    error_ = "D3D12 guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_upload(memory_->coherency(), address, width,
                                            kTransferSize, usage, read_epoch);
  if (plan.ranges.empty()) return true;

  std::span<std::byte> upload_bytes;
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map D3D12 upload buffer";
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
        error_ = queue_->error().empty() ? "D3D12 upload ring reservation failed"
                                         : queue_->error();
        return false;
      }
      // Release ownership before taking the snapshot. A CPU write racing after
      // this point will re-dirty the exact bytes and therefore cannot be lost.
      coherency_.commit_cpu_upload(chunk_address, chunk_size);
      if (!memory_->copy_physical_range(
              chunk_address, upload_bytes.subspan(*offset, chunk_size))) {
        coherency_.rollback_cpu_upload(chunk_address, chunk_size);
        error_ = "failed to snapshot Xbox physical memory";
        return false;
      }

      const auto state_before_copy = state_;
      if (!queue_->execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t,
                                     std::uint32_t) {
            if (state_before_copy != D3D12_RESOURCE_STATE_COPY_DEST) {
              D3D12_RESOURCE_BARRIER barrier{};
              barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              barrier.Transition.pResource = mirror_.resource();
              barrier.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              barrier.Transition.StateBefore = state_before_copy;
              barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
              list->ResourceBarrier(1, &barrier);
            }
            list->CopyBufferRegion(mirror_.resource(), chunk_address,
                                   upload_.resource(), *offset, chunk_size);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = mirror_.resource();
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            list->ResourceBarrier(1, &barrier);
          })) {
        coherency_.rollback_cpu_upload(chunk_address, chunk_size);
        error_ = queue_->error();
        return false;
      }
      upload_ring_.commit(*offset, chunk_size, queue_->retirement_value());
      state_ = D3D12_RESOURCE_STATE_GENERIC_READ;
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
    error_ = "D3D12 guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_readback(memory_->coherency(), address, width,
                                             kTransferSize, usage);
  for (const auto& range : plan.ranges) {
    // A region is free again once the publisher has written its readback
    // into guest memory, which it does only after the GPU finished the copy.
    const auto consumed =
        (std::min)(publisher_.published_through(), queue_->completed_value());
    const auto offset = readback_ring_.reserve(
        range.size, consumed, [&](std::uint64_t value) { return wait_readbacks(value); });
    if (!offset) {
      if (error_.empty())
        error_ = queue_->error().empty() ? "D3D12 readback ring reservation failed"
                                         : queue_->error();
      return false;
    }
    const auto state_before_copy = state_;
    if (!queue_->execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t,
                                   std::uint32_t) {
          if (state_before_copy != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = mirror_.resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = state_before_copy;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier);
          }
          list->CopyBufferRegion(readback_.resource(), *offset, mirror_.resource(),
                                 range.address, range.size);
        })) {
      error_ = queue_->error();
      return false;
    }
    state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto value = queue_->retirement_value();
    readback_ring_.commit(*offset, range.size, value);
    publisher_.push(range, *offset, value);
  }
  return true;
}

bool GuestMemoryMirror::wait_readbacks(std::uint64_t value) {
  if (!queue_->flush()) {
    error_ = queue_->error();
    return false;
  }
  if (!publisher_.wait_published(value)) {
    error_ = "D3D12 guest-memory readback failed: " + publisher_.error();
    return false;
  }
  return true;
}

bool GuestMemoryMirror::complete_cpu_visibility(bool wait) {
  if (!memory_ || !queue_) {
    if (!publisher_.pending()) return true;
    error_ = "D3D12 guest-memory mirror is not initialized";
    return false;
  }
  // The publisher writes each readback into guest memory as soon as the GPU
  // finishes it; waiting means submitting the open batch and letting it
  // catch up with everything queued so far.
  if (wait && publisher_.pending() && !wait_readbacks(publisher_.last_value())) return false;
  if (const auto failure = publisher_.error(); !failure.empty()) {
    error_ = "D3D12 guest-memory readback failed: " + failure;
    return false;
  }
  return true;
}

void GuestMemoryMirror::prepare_shader_access(ID3D12GraphicsCommandList* list,
                                               bool writable) {
  if (!list || !mirror_.resource()) return;
  const auto target = writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                               : D3D12_RESOURCE_STATE_GENERIC_READ;
  if (state_ != target) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = mirror_.resource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state_;
    barrier.Transition.StateAfter = target;
    list->ResourceBarrier(1, &barrier);
    state_ = target;
  } else if (writable) {
    // Consecutive memexport draws may both keep the mirror in UAV state. An
    // explicit UAV barrier makes writes from the previous draw visible to
    // vertex fetches or later exports without an unnecessary state transition.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = mirror_.resource();
    list->ResourceBarrier(1, &barrier);
  }
}

}  // namespace xenon::gpu::d3d12
