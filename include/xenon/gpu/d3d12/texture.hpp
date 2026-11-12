#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/gpu/staging_ring.hpp"
#include "xenon/gpu/texture.hpp"

namespace xenon::gpu::d3d12 {

[[nodiscard]] DXGI_FORMAT host_texture_format(TextureHostFormat format) noexcept;

class TextureImage {
 public:
  // Creates the host image for `layout`, in COPY_DEST, without contents.
  [[nodiscard]] bool create(ID3D12Device* device, const TextureLayout& layout);
  // Whether this image can hold `layout` (same format, extent, dimension and
  // mip chain), so reloading a texture reuses it instead of a new resource.
  [[nodiscard]] bool matches(const TextureLayout& layout) const noexcept;
  // Creates the image and uploads CPU-decoded texels (decode_texture()),
  // waiting for the copy. Tools and tests use it; the runtime loads textures
  // with GpuTextureLoader.
  [[nodiscard]] bool initialize(ID3D12Device* device, CommandQueue& queue,
                                const DecodedTexture& texture);
  void reset() noexcept;
  [[nodiscard]] ID3D12Resource* resource() const noexcept { return resource_.Get(); }
  [[nodiscard]] DXGI_FORMAT format() const noexcept { return format_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Part 7 of the AC6 Runtime Readiness pass: a structured signal alongside
  // error(), set exactly when create() failed specifically because the guest
  // texture format has no D3D12 mapping - distinct from every other failure
  // reason, without the caller having to string-match error().
  [[nodiscard]] bool unsupported_format() const noexcept { return unsupported_format_; }

  // The subresource index and copyable footprint of a layout subresource.
  [[nodiscard]] std::uint32_t subresource_index(const TextureSubresourceLayout& sub) const noexcept;
  [[nodiscard]] const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint(std::uint32_t index) const {
    return footprints_[index];
  }
  // The state the recorded work leaves the image in.
  [[nodiscard]] D3D12_RESOURCE_STATES state() const noexcept { return state_; }
  void set_state(D3D12_RESOURCE_STATES state) noexcept { state_ = state; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
  DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints_{};
  std::vector<UINT> rows_{};
  std::uint64_t upload_size_{};
  TextureDescriptor descriptor_{};
  std::uint32_t mip_levels_{};
  D3D12_RESOURCE_STATES state_{D3D12_RESOURCE_STATE_COPY_DEST};
  std::string error_{};
  bool unsupported_format_{};
};

// Textures loaded on the GPU (see GpuTextureLoadConstants): a compute pass
// untiles and converts the guest texels into a staging buffer, and a copy
// moves them into the image. Everything is recorded into the queue's open
// batch, ahead of the draw that samples the image; nothing waits.
class GpuTextureLoader {
 public:
  // From the guest-memory mirror, which must hold the texture's bytes
  // (GuestMemoryMirror::synchronize_range()).
  [[nodiscard]] bool load_from_mirror(ID3D12Device* device, CommandQueue& queue,
                                      GuestMemoryMirror& mirror, TextureImage& image,
                                      const TextureLayout& layout);
  // From `bytes`, a copy of guest memory [base, base + bytes.size()) taken
  // at a sync point.
  [[nodiscard]] bool load_from_bytes(ID3D12Device* device, CommandQueue& queue,
                                     TextureImage& image, const TextureLayout& layout,
                                     std::span<const std::byte> bytes, std::uint32_t base);
  void reset() noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  static constexpr std::uint64_t kStagingSize = 64ull * 1024ull * 1024ull;
  static constexpr std::uint64_t kUploadSize = 32ull * 1024ull * 1024ull;

  [[nodiscard]] bool initialize(ID3D12Device* device);
  [[nodiscard]] bool run(CommandQueue& queue, TextureImage& image, const TextureLayout& layout,
                         D3D12_GPU_VIRTUAL_ADDRESS source, std::uint32_t source_base,
                         std::uint32_t source_limit,
                         const std::function<void(ID3D12GraphicsCommandList*)>& prepare_source);
  void retire(std::uint64_t completed);

  ID3D12Device* device_{};
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_{};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_{};
  Buffer staging_{};
  StagingRing staging_ring_{};
  D3D12_RESOURCE_STATES staging_state_{D3D12_RESOURCE_STATE_COPY_SOURCE};
  Buffer upload_{};
  StagingRing upload_ring_{};
  // Uploads too large for the ring, kept until the GPU has read them.
  struct Transient {
    std::unique_ptr<Buffer> buffer{};
    std::uint64_t value{};
  };
  std::vector<Transient> transients_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
