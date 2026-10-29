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
  UtilityShaderCompiler compiler;
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

ID3D12PipelineState* EdramDepthTransfer::pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                  MsaaSamples source_host,
                                                  MsaaSamples destination_host,
                                                  EdramDepthTransferPass pass,
                                                  std::uint32_t stencil_bit) {
#ifndef XENON_HAS_DXC
  error_ = "D3D12 EDRAM depth transfer requires DXC";
  return nullptr;
#else
  const auto key = std::uint64_t(format) | (std::uint64_t(source_host) << 32u) |
                   (std::uint64_t(destination_host) << 40u) |
                   (std::uint64_t(pass) << 48u) | (std::uint64_t(stencil_bit & 7u) << 56u);
  if (const auto found = pipelines_.find(key); found != pipelines_.end())
    return found->second.Get();
  if (!root_signature_) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = sizeof(EdramDepthTransferConstants) / 4u;
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
      error_ = "D3D12 EDRAM depth transfer root signature creation failed";
      return nullptr;
    }
  }
  UtilityShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  const auto ps = compiler.compile(make_edram_depth_transfer_shader(source_host, pass));
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty() ? ps.diagnostics.front()
                                       : "D3D12 EDRAM depth transfer shader compilation failed";
    return nullptr;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature_.Get();
  desc.VS = {vs.binary.data(), vs.binary.size()};
  desc.PS = {ps.binary.data(), ps.binary.size()};
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.DSVFormat = format;
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(destination_host);
  auto& depth_stencil = desc.DepthStencilState;
  depth_stencil.DepthEnable = TRUE;
  depth_stencil.DepthWriteMask = pass == EdramDepthTransferPass::StencilBit
                                     ? D3D12_DEPTH_WRITE_MASK_ZERO
                                     : D3D12_DEPTH_WRITE_MASK_ALL;
  depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  depth_stencil.StencilEnable = TRUE;
  depth_stencil.StencilReadMask = 0xFF;
  depth_stencil.StencilWriteMask = pass == EdramDepthTransferPass::StencilBit
                                       ? static_cast<UINT8>(1u << (stencil_bit & 7u))
                                       : 0xFF;
  depth_stencil.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  depth_stencil.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  depth_stencil.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
  depth_stencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  depth_stencil.BackFace = depth_stencil.FrontFace;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 EDRAM depth transfer pipeline creation failed";
    return nullptr;
  }
  return pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool EdramDepthTransfer::transfer(ID3D12Device* device, CommandQueue& queue,
                                  DepthTargetImage& source, DepthTargetImage& destination,
                                  const EdramTransferConstants& constants,
                                  std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.resource() || !destination.resource() ||
      source.format() != destination.format() ||
      source.guest_format() != destination.guest_format()) {
    error_ = "D3D12 EDRAM depth transfer needs two depth images of one format";
    return false;
  }
  if (regions.empty()) return true;
  if (stencil_ref_supported_ < 0) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    stencil_ref_supported_ =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                              sizeof(options))) &&
        options.PSSpecifiedStencilRefSupported;
  }
  const bool combined = stencil_ref_supported_ == 1;
  const auto format = destination.format();
  const auto source_host = source.host_msaa(), destination_host = destination.host_msaa();
  std::array<ID3D12PipelineState*, 9> passes{};
  passes[0] = pipeline(device, format, source_host, destination_host,
                       combined ? EdramDepthTransferPass::DepthAndStencil
                                : EdramDepthTransferPass::Depth,
                       0u);
  if (!passes[0]) return false;
  if (!combined) {
    for (std::uint32_t bit = 0; bit < 8u; ++bit) {
      passes[bit + 1u] = pipeline(device, format, source_host, destination_host,
                                  EdramDepthTransferPass::StencilBit, bit);
      if (!passes[bit + 1u]) return false;
    }
  }
  auto* source_heap = source.shader_resource_heap();
  if (!source_heap) {
    error_ = source.error();
    return false;
  }
  EdramDepthTransferConstants values{};
  values.transfer = constants;
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source.resource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        list->SetGraphicsRootSignature(root_signature_.Get());
        ID3D12DescriptorHeap* heaps[]{source_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(1, source_heap->GetGPUDescriptorHandleForHeapStart());
        const auto dsv = destination.dsv();
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(destination.width()),
                                      float(destination.height()), 0.0f, 1.0f};
        list->RSSetViewports(1, &viewport);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto draw_regions = [&] {
          for (const auto& region : regions) {
            const D3D12_RECT scissor{region.left, region.top, region.right, region.bottom};
            list->RSSetScissorRects(1, &scissor);
            list->DrawInstanced(3, 1, 0, 0);
          }
        };
        // Depth (and stencil, or stencil cleared to zero).
        list->SetPipelineState(passes[0]);
        list->SetGraphicsRoot32BitConstants(0, sizeof(values) / 4u, &values, 0);
        list->OMSetStencilRef(0u);
        draw_regions();
        if (!combined) {
          // Each stencil bit the source sample has set, written alone.
          list->OMSetStencilRef(0xFFu);
          for (std::uint32_t bit = 0; bit < 8u; ++bit) {
            values.stencil[0] = 1u << bit;
            list->SetPipelineState(passes[bit + 1u]);
            list->SetGraphicsRoot32BitConstants(0, sizeof(values) / 4u, &values, 0);
            draw_regions();
          }
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
