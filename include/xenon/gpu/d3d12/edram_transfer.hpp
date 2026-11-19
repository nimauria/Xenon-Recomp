#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/depth_target.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu::d3d12 {

// The native image a converting transfer reads: a color image (one SRV, t0)
// or a depth image (depth t0 and stencil t1), with the state it rests in
// between draws and the EDRAM view it holds.
struct EdramTransferSourceImage {
  ID3D12Resource* resource{};
  ID3D12DescriptorHeap* heap{};
  D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_RENDER_TARGET};
  MsaaSamples host_msaa{MsaaSamples::X1};
  EdramTransferEndpoint endpoint{};
  // Canonical EDRAM (EdramCanonicalStore::source()): the EDRAM image as a raw
  // buffer at t0 instead of an image; `endpoint` is unused.
  bool canonical{};
};

// GPU-resident canonical EDRAM: the 10 MiB EDRAM image in a default-heap
// buffer the canonical loads read (one raw SRV at t0), filled from the CPU
// canonical store tile by tile through a persistently mapped upload buffer
// laid out as EDRAM. upload() records the copies into the queue's open batch;
// a tile's upload bytes are rewritten only once the GPU has copied the
// previous ones (each tile remembers the batch that reads it).
class EdramCanonicalStore {
 public:
  [[nodiscard]] bool upload(ID3D12Device* device, CommandQueue& queue, const Edram& edram,
                            std::span<const std::uint16_t> tiles);
  [[nodiscard]] EdramTransferSourceImage source() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Phase 3: a native image's `regions` packed into canonical EDRAM on the
  // GPU (make_edram_canonical_store_shader) and the covered `tiles` brought
  // back into `edram` - one readback instead of one per sample, and no CPU
  // conversion. The tiles are uploaded first, so words of a tile the regions
  // do not cover come back as `edram` held them. Waits for the GPU (the
  // caller needs the canonical bytes). For edram_canonical_load_supported()
  // formats.
  [[nodiscard]] bool flush(ID3D12Device* device, CommandQueue& queue,
                           const EdramTransferSourceImage& source,
                           const EdramTransferConstants& constants,
                           std::span<const EdramSurfaceRegion> regions,
                           std::span<const std::uint16_t> tiles, Edram& edram);

 private:
  [[nodiscard]] bool create(ID3D12Device* device);
  [[nodiscard]] ID3D12PipelineState* store_pipeline(ID3D12Device* device,
                                                    const EdramTransferSourceImage& source);
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer_{};
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_{};
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_{};
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_{};
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> store_root_signatures_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> store_pipelines_{};
  std::byte* mapped_{};
  const std::byte* readback_mapped_{};
  std::array<std::uint64_t, Edram::kTileCount> tile_busy_until_{};
  std::string error_{};
};
// Null `heap` (with `error` set) when the image has no shader-resource view.
[[nodiscard]] EdramTransferSourceImage edram_transfer_source(RenderTargetImage& image,
                                                             std::string& error);
[[nodiscard]] EdramTransferSourceImage edram_transfer_source(DepthTargetImage& image,
                                                             std::string& error);

// Moves EDRAM tiles directly from one native color image to another of the
// same format, on the GPU: a sample-frequency draw into the destination that
// loads, for each sample, the source sample holding the same EDRAM word. It is
// recorded into the queue's open batch like a draw, with no CPU round trip.
// Pipelines are built once per format and sample-count pair.
//
// `convert` does the same from an image of another view - a color image of a
// different format, or a depth image - when edram_converting_transfer_supported()
// accepts the pair: each sample's EDRAM word is packed from the source's host
// value and unpacked as the destination's format.
class EdramColorTransfer {
 public:
  [[nodiscard]] bool transfer(ID3D12Device* device, CommandQueue& queue,
                              RenderTargetImage& source, RenderTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] bool convert(ID3D12Device* device, CommandQueue& queue,
                             const EdramTransferSourceImage& source,
                             RenderTargetImage& destination,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions);
  // Loads canonical EDRAM words (`canonical`, EdramCanonicalStore::source(),
  // already holding the regions' tiles) into `destination`, for the formats
  // edram_canonical_load_supported() accepts.
  [[nodiscard]] bool load_canonical(ID3D12Device* device, CommandQueue& queue,
                                    const EdramTransferSourceImage& canonical,
                                    RenderTargetImage& destination,
                                    const EdramTransferConstants& constants,
                                    std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] ID3D12PipelineState* pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                              MsaaSamples source_host,
                                              MsaaSamples destination_host);
  [[nodiscard]] ID3D12PipelineState* converting_pipeline(
      ID3D12Device* device, const EdramTransferSourceImage& source,
      const RenderTargetImage& destination);
  [[nodiscard]] bool draw(CommandQueue& queue, ID3D12PipelineState* state,
                          ID3D12RootSignature* root_signature, ID3D12Resource* source,
                          ID3D12DescriptorHeap* source_heap,
                          D3D12_RESOURCE_STATES source_state, RenderTargetImage& destination,
                          const EdramTransferConstants& constants,
                          std::span<const EdramSurfaceRegion> regions);

  // Indexed by the source's SRV count less one (color, depth).
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> root_signatures_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>>
      converting_pipelines_{};
  std::string error_{};
};

// The same direct transfer between two native depth images of one format.
// Depth and stencil move together in one pass where the device takes
// SV_StencilRef; otherwise depth moves first (stencil replaced with zero)
// and each stencil bit follows in its own pass, still with no CPU round trip.
// `convert` takes a source of another view, as EdramColorTransfer's does.
class EdramDepthTransfer {
 public:
  [[nodiscard]] bool transfer(ID3D12Device* device, CommandQueue& queue,
                              DepthTargetImage& source, DepthTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] bool convert(ID3D12Device* device, CommandQueue& queue,
                             const EdramTransferSourceImage& source,
                             DepthTargetImage& destination,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions);
  // As EdramColorTransfer::load_canonical(), into a depth image.
  [[nodiscard]] bool load_canonical(ID3D12Device* device, CommandQueue& queue,
                                    const EdramTransferSourceImage& canonical,
                                    DepthTargetImage& destination,
                                    const EdramTransferConstants& constants,
                                    std::span<const EdramSurfaceRegion> regions);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  // The pipeline of one pass. `converting` selects the converting shader for
  // `source` (else the same-format one; a canonical source always takes the
  // canonical load); `stencil_bit` selects a StencilBit pass's write mask
  // (ignored otherwise).
  [[nodiscard]] ID3D12PipelineState* pipeline(ID3D12Device* device,
                                              const EdramTransferSourceImage& source,
                                              const DepthTargetImage& destination,
                                              bool converting, EdramDepthTransferPass pass,
                                              std::uint32_t stencil_bit);
  [[nodiscard]] bool run(ID3D12Device* device, CommandQueue& queue,
                         const EdramTransferSourceImage& source,
                         DepthTargetImage& destination, bool converting,
                         const EdramTransferConstants& constants,
                         std::span<const EdramSurfaceRegion> regions);

  // Indexed by the source's SRV count less one (color, depth).
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> root_signatures_{};
  std::unordered_map<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_{};
  int stencil_ref_supported_{-1};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
