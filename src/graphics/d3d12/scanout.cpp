#include "xenon/gpu/d3d12/scanout.hpp"

#include <array>
#include <cstring>

#include "xenon/gpu/d3d12/memory.hpp"

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::d3d12 {

bool ScanoutRenderer::prepare(ID3D12Device* device, DXGI_FORMAT format) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)format;
  error_ = "D3D12 scan-out requires DXC";
  return false;
#else
  if (pipelines_.contains(format)) return true;
  if (!root_signature_) {
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = sizeof(ScanoutConstants) / 4u;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // The mirror is a raw buffer of all physical memory, bound by address.
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &diagnostics)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                           serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature_)))) {
      error_ = "D3D12 scan-out root signature creation failed";
      return false;
    }
  }
  DxcShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  const auto ps = compiler.compile(make_scanout_shader());
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                     : "D3D12 scan-out shader compilation failed";
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature_.Get();
  desc.VS = {vs.binary.data(), vs.binary.size()};
  desc.PS = {ps.binary.data(), ps.binary.size()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = 1;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 scan-out pipeline creation failed";
    return false;
  }
  pipelines_.emplace(format, std::move(state));
  return true;
#endif
}

void ScanoutRenderer::record(ID3D12GraphicsCommandList* list, GuestMemoryMirror& mirror,
                             D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format,
                             const ScanoutConstants& constants) {
  const auto found = pipelines_.find(format);
  if (found == pipelines_.end()) return;
  mirror.prepare_shader_access(list, false);
  list->SetPipelineState(found->second.Get());
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
  list->SetGraphicsRootShaderResourceView(1, mirror.resource()->GetGPUVirtualAddress());
  list->OMSetRenderTargets(1, &target, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(constants.target_width),
                                float(constants.target_height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, LONG(constants.target_width), LONG(constants.target_height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
}

bool ScanoutRenderer::capture(ID3D12Device* device, CommandQueue& queue,
                              GuestMemoryMirror& mirror, const ScanoutConstants& constants,
                              PreparedPresentationFrame& output) {
  constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  output = {};
  if (!constants.target_width || !constants.target_height) {
    error_ = "D3D12 scan-out capture needs a non-zero extent";
    return false;
  }
  if (!prepare(device, kFormat)) return false;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC image_desc{};
  image_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  image_desc.Width = constants.target_width;
  image_desc.Height = constants.target_height;
  image_desc.DepthOrArraySize = 1;
  image_desc.MipLevels = 1;
  image_desc.Format = kFormat;
  image_desc.SampleDesc.Count = 1;
  image_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  Microsoft::WRL::ComPtr<ID3D12Resource> image;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &image_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                             IID_PPV_ARGS(&image)))) {
    error_ = "D3D12 scan-out capture image creation failed";
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
  if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)))) {
    error_ = "D3D12 scan-out capture RTV heap creation failed";
    return false;
  }
  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(image.Get(), nullptr, rtv);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 readback_size = 0;
  device->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                &readback_size);
  Buffer readback;
  if (!readback.initialize(device, readback_size, D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST)) {
    error_ = readback.error();
    return false;
  }
  if (!queue.execute([&](ID3D12GraphicsCommandList* list) {
        record(list, mirror, rtv, kFormat, constants);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = image.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = image.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.resource();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      })) {
    error_ = queue.error();
    return false;
  }
  std::span<std::byte> bytes;
  if (!readback.map(bytes)) {
    error_ = readback.error();
    return false;
  }
  output.width = constants.target_width;
  output.height = constants.target_height;
  output.row_pitch = constants.target_width * 4u;
  output.rgba8.resize(std::size_t(output.row_pitch) * output.height);
  for (std::uint32_t y = 0; y < output.height; ++y) {
    std::memcpy(output.rgba8.data() + std::size_t(y) * output.row_pitch,
                bytes.data() + footprint.Offset + std::size_t(y) * footprint.Footprint.RowPitch,
                output.row_pitch);
  }
  readback.unmap();
  output.valid = true;
  return true;
}

}  // namespace xenon::gpu::d3d12
