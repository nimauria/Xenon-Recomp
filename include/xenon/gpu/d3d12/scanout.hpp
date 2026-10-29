#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/scanout.hpp"

namespace xenon::gpu::d3d12 {

// Draws a front buffer into a host render target on the GPU: one fullscreen
// pass whose pixel shader (make_scanout_shader) reads the front buffer from
// the guest-memory mirror, untiles, converts and filters it. No CPU readback,
// decode or scaling is involved. Pipelines are built once per target format.
class ScanoutRenderer {
 public:
  // Builds the pipeline for `format` if needed; call before record().
  [[nodiscard]] bool prepare(ID3D12Device* device, DXGI_FORMAT format);
  // Records the pass into `target`, which must be in RENDER_TARGET state and
  // of the format last passed to prepare().
  void record(ID3D12GraphicsCommandList* list, GuestMemoryMirror& mirror,
              D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format,
              const ScanoutConstants& constants);
  // Draws into an offscreen RGBA8 image and reads it back: a screenshot of
  // scan-out at the constants' target size.
  [[nodiscard]] bool capture(ID3D12Device* device, CommandQueue& queue,
                             GuestMemoryMirror& mirror, const ScanoutConstants& constants,
                             PreparedPresentationFrame& output);
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_{};
  std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
