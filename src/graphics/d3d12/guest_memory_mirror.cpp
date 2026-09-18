#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"

#include <algorithm>
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
  generic_read_state_ = false;
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
  if (!memory_ || !queue_) return false;
  std::span<std::byte> upload_bytes;
  if (!upload_.map(upload_bytes)) {
    error_ = "failed to map D3D12 upload buffer";
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
    if (!memory_->copy_physical_range(address,
                                      upload_bytes.first(size))) {
      std::lock_guard lock(dirty_mutex_);
      std::fill(dirty_pages_.begin() + first,
                dirty_pages_.begin() + first + count, std::uint8_t{1});
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
      std::lock_guard lock(dirty_mutex_);
      std::fill(dirty_pages_.begin() + first,
                dirty_pages_.begin() + first + count, std::uint8_t{1});
      error_ = queue_->error();
      return false;
    }
    generic_read_state_ = true;
    page = first + count;
  }
  return true;
}

}  // namespace xenon::gpu::d3d12
