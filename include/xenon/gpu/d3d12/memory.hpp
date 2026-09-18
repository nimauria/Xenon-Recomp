#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

namespace xenon::gpu::d3d12 {

class Buffer {
 public:
  [[nodiscard]] bool initialize(ID3D12Device* device, std::uint64_t size,
                                D3D12_HEAP_TYPE heap_type,
                                D3D12_RESOURCE_STATES initial_state,
                                D3D12_RESOURCE_FLAGS flags =
                                    D3D12_RESOURCE_FLAG_NONE);
  void reset() noexcept;
  [[nodiscard]] bool map(std::span<std::byte>& bytes);
  void unmap() noexcept;
  [[nodiscard]] ID3D12Resource* resource() const noexcept {
    return resource_.Get();
  }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
  std::uint64_t size_{};
  std::byte* mapped_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
