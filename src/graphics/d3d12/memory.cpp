#include "xenon/gpu/d3d12/memory.hpp"

namespace xenon::gpu::d3d12 {

bool Buffer::initialize(ID3D12Device* device, std::uint64_t size,
                        D3D12_HEAP_TYPE heap_type,
                        D3D12_RESOURCE_STATES initial_state,
                        D3D12_RESOURCE_FLAGS flags) {
  reset();
  error_.clear();
  if (!device || !size) {
    error_ = "D3D12 buffer requires a device and non-zero size";
    return false;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heap_type;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
          IID_PPV_ARGS(&resource_)))) {
    error_ = "CreateCommittedResource failed for D3D12 buffer";
    return false;
  }
  size_ = size;
  return true;
}

void Buffer::reset() noexcept {
  unmap();
  resource_.Reset();
  size_ = 0;
}

bool Buffer::map(std::span<std::byte>& bytes) {
  if (!resource_) {
    error_ = "D3D12 buffer is not initialized";
    return false;
  }
  if (!mapped_) {
    void* mapped = nullptr;
    if (FAILED(resource_->Map(0, nullptr, &mapped))) {
      error_ = "ID3D12Resource::Map failed";
      return false;
    }
    mapped_ = static_cast<std::byte*>(mapped);
  }
  bytes = {mapped_, static_cast<std::size_t>(size_)};
  return true;
}

void Buffer::unmap() noexcept {
  if (resource_ && mapped_) resource_->Unmap(0, nullptr);
  mapped_ = nullptr;
}

}  // namespace xenon::gpu::d3d12
