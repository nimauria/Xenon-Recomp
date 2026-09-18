#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"

#include <cstddef>
#include <span>

namespace xenon::gpu::d3d12 {

GuestMemoryMirror::~GuestMemoryMirror() { reset(); }

bool GuestMemoryMirror::initialize(ID3D12Device* device, CommandQueue& queue,
                                   memory::AddressSpace& memory) {
  reset();
  error_.clear();
  if (!mirror_.initialize(device, memory::kPhysicalMemorySize,
                          D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COPY_DEST) ||
      !upload_.initialize(device, kUploadSize, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ)) {
    error_ = "failed to allocate D3D12 Xbox physical-memory mirror";
    reset();
    return false;
  }
  memory_ = &memory;
  queue_ = &queue;
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
  generic_read_state_ = false;
}

bool GuestMemoryMirror::synchronize() {
  if (!memory_ || !queue_) return false;
  std::span<std::byte> upload_bytes;
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map D3D12 upload buffer";
    return false;
  }
  const auto through_epoch = memory_->coherency().current_epoch();
  memory_->coherency().collect_dirty_ranges(
      synchronized_epoch_, through_epoch, kUploadSize, dirty_ranges_);
  for (const auto& range : dirty_ranges_) {
    const auto address = range.address;
    const auto size = range.size;
    if (!memory_->copy_physical_range(address,
                                      upload_bytes.first(size))) {
      error_ = "failed to snapshot Xbox physical memory";
      return false;
    }
    if (!queue_->execute([&](ID3D12GraphicsCommandList* list) {
          if (generic_read_state_) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = mirror_.resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &barrier);
          }
          list->CopyBufferRegion(mirror_.resource(), address, upload_.resource(),
                                 0, size);
          D3D12_RESOURCE_BARRIER barrier{};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = mirror_.resource();
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
          list->ResourceBarrier(1, &barrier);
        })) {
      error_ = queue_->error();
      return false;
    }
    generic_read_state_ = true;
  }
  synchronized_epoch_ = through_epoch;
  return true;
}

}  // namespace xenon::gpu::d3d12
