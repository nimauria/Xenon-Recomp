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
      !readback_.initialize(device, kTransferSize, D3D12_HEAP_TYPE_READBACK,
                            D3D12_RESOURCE_STATE_COPY_DEST)) {
    error_ = "failed to allocate D3D12 Xbox physical-memory mirror transfers";
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
  state_ = D3D12_RESOURCE_STATE_COPY_DEST;
}

bool GuestMemoryMirror::synchronize() {
  return synchronize_range(0u, memory::kPhysicalMemorySize);
}

bool GuestMemoryMirror::synchronize_range(std::uint32_t address,
                                          std::uint32_t width) {
  if (!memory_ || !queue_) {
    error_ = "D3D12 guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_upload(memory_->coherency(), address, width,
                                            kTransferSize);
  if (plan.ranges.empty()) return true;

  std::span<std::byte> upload_bytes;
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map D3D12 upload buffer";
    return false;
  }

  for (const auto& range : plan.ranges) {
      const auto chunk_address = range.address;
      const auto chunk_size = range.size;
      // Release ownership before taking the snapshot. A CPU write racing after
      // this point will re-dirty the exact bytes and therefore cannot be lost.
      coherency_.commit_cpu_upload(chunk_address, chunk_size);
      if (!memory_->copy_physical_range(
              chunk_address, upload_bytes.first(chunk_size))) {
        coherency_.rollback_cpu_upload(chunk_address, chunk_size);
        error_ = "failed to snapshot Xbox physical memory";
        return false;
      }

      const auto state_before_copy = state_;
      if (!queue_->execute([&](ID3D12GraphicsCommandList* list) {
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
                                   upload_.resource(), 0, chunk_size);
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
      state_ = D3D12_RESOURCE_STATE_GENERIC_READ;
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
    error_ = "D3D12 guest-memory mirror is not initialized";
    return false;
  }
  const auto plan = coherency_.plan_readback(address, width, kTransferSize);
  if (plan.ranges.empty()) return true;

  std::span<std::byte> readback_bytes;
  if (!readback_.map(readback_bytes)) {
    error_ = "failed to map D3D12 guest-memory readback buffer";
    return false;
  }

  for (const auto& range : plan.ranges) {
      const auto chunk_address = range.address;
      const auto chunk_size = range.size;
      const auto state_before_copy = state_;
      if (!queue_->execute([&](ID3D12GraphicsCommandList* list) {
            if (state_before_copy != D3D12_RESOURCE_STATE_COPY_SOURCE) {
              D3D12_RESOURCE_BARRIER barrier{};
              barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              barrier.Transition.pResource = mirror_.resource();
              barrier.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              barrier.Transition.StateBefore = state_before_copy;
              barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
              list->ResourceBarrier(1, &barrier);
            }
            list->CopyBufferRegion(readback_.resource(), 0, mirror_.resource(),
                                   chunk_address, chunk_size);
          })) {
        error_ = queue_->error();
        return false;
      }
      state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;

      std::uint64_t publication_epoch = 0u;
      if (!memory_->write_physical(
              chunk_address, readback_bytes.first(chunk_size),
              &publication_epoch)) {
        error_ = "D3D12 guest-memory readback destination is outside physical RAM";
        return false;
      }

      // Xenon Memory acknowledges this mirror's exact publication epoch. That
      // prevents a GPU->CPU download from echoing back as a false CPU upload
      // while retaining unrelated CPU/DMA writes published in the same window.
      (void)coherency_.commit_gpu_download(
          memory_->coherency(), plan, chunk_address, chunk_size,
          publication_epoch);
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
