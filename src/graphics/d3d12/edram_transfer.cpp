#include "xenon/gpu/d3d12/edram_transfer.hpp"

#include <array>

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::d3d12 {

ID3D12PipelineState* EdramColorTransfer::pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                  MsaaSamples source_host,
                                                  MsaaSamples destination_host) {
#ifndef XENON_HAS_DXC
  error_ = "D3D12 EDRAM transfer requires DXC";
  return nullptr;
#else
  const auto key = std::uint64_t(format) | (std::uint64_t(source_host) << 32u) |
                   (std::uint64_t(destination_host) << 40u);
  if (const auto found = pipelines_.find(key); found != pipelines_.end())
    return found->second.Get();
  if (!root_signature_) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = sizeof(EdramTransferConstants) / 4u;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
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
      error_ = "D3D12 EDRAM transfer root signature creation failed";
      return nullptr;
    }
  }
  DxcShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  const auto ps = compiler.compile(make_edram_color_transfer_shader(source_host));
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                     : "D3D12 EDRAM transfer shader compilation failed";
    return nullptr;
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
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(destination_host);
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 EDRAM transfer pipeline creation failed";
    return nullptr;
  }
  return pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool EdramColorTransfer::transfer(ID3D12Device* device, CommandQueue& queue,
                                  RenderTargetImage& source, RenderTargetImage& destination,
                                  const EdramTransferConstants& constants,
                                  std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.resource() || !destination.resource() ||
      source.format() != destination.format()) {
    error_ = "D3D12 EDRAM transfer needs two color images of one format";
    return false;
  }
  if (regions.empty()) return true;
  auto* state = pipeline(device, destination.format(), source.host_msaa(),
                         destination.host_msaa());
  if (!state) return false;
  auto* source_heap = source.shader_resource_heap();
  if (!source_heap) {
    error_ = source.error();
    return false;
  }
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source.resource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        list->SetPipelineState(state);
        list->SetGraphicsRootSignature(root_signature_.Get());
        ID3D12DescriptorHeap* heaps[]{source_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        list->SetGraphicsRootDescriptorTable(1, source_heap->GetGPUDescriptorHandleForHeapStart());
        const auto rtv = destination.rtv();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(destination.width()),
                                      float(destination.height()), 0.0f, 1.0f};
        list->RSSetViewports(1, &viewport);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const auto& region : regions) {
          const D3D12_RECT scissor{region.left, region.top, region.right, region.bottom};
          list->RSSetScissorRects(1, &scissor);
          list->DrawInstanced(3, 1, 0, 0);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

}  // namespace xenon::gpu::d3d12
