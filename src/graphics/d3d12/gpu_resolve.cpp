#include "xenon/gpu/d3d12/gpu_resolve.hpp"

#include <array>
#include <span>

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::d3d12 {
namespace {

bool create_root(ID3D12Device* device, std::span<const D3D12_ROOT_PARAMETER> parameters,
                 Microsoft::WRL::ComPtr<ID3D12RootSignature>& root) {
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = static_cast<UINT>(parameters.size());
  desc.pParameters = parameters.data();
  Microsoft::WRL::ComPtr<ID3DBlob> serialized;
  Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
  return SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                               &diagnostics)) &&
         SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                               serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&root)));
}

}  // namespace

ID3D12PipelineState* GpuResolver::resolve_pipeline(ID3D12Device* device, MsaaSamples samples) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)samples;
  error_ = "D3D12 GPU resolve requires DXC";
  return nullptr;
#else
  const auto key = static_cast<std::uint32_t>(samples);
  if (const auto found = resolve_pipelines_.find(key); found != resolve_pipelines_.end())
    return found->second.Get();
  if (!resolve_root_) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(GpuResolveConstants) / 4u;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    // The mirror, written by physical address.
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    if (!create_root(device, parameters, resolve_root_)) {
      error_ = "D3D12 GPU resolve root signature creation failed";
      return nullptr;
    }
  }
  UtilityShaderCompiler compiler;
  const auto cs = compiler.compile(make_gpu_resolve_shader(samples));
  if (!cs.succeeded) {
    error_ = cs.diagnostics.empty() ? "D3D12 GPU resolve shader compilation failed"
                                    : cs.diagnostics.front();
    return nullptr;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = resolve_root_.Get();
  desc.CS = {cs.binary.data(), cs.binary.size()};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 GPU resolve pipeline creation failed";
    return nullptr;
  }
  return resolve_pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

ID3D12PipelineState* GpuResolver::clear_pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                 MsaaSamples samples) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)format;
  (void)samples;
  error_ = "D3D12 GPU resolve clear requires DXC";
  return nullptr;
#else
  const auto key = std::uint64_t(format) | (std::uint64_t(samples) << 32u);
  if (const auto found = clear_pipelines_.find(key); found != clear_pipelines_.end())
    return found->second.Get();
  if (!clear_root_) {
    std::array<D3D12_ROOT_PARAMETER, 1> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(GpuClearConstants) / 4u;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    if (!create_root(device, parameters, clear_root_)) {
      error_ = "D3D12 GPU resolve clear root signature creation failed";
      return nullptr;
    }
  }
  UtilityShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  const auto ps = compiler.compile(make_edram_clear_shader());
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                     : "D3D12 GPU resolve clear shader compilation failed";
    return nullptr;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = clear_root_.Get();
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
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(samples);
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 GPU resolve clear pipeline creation failed";
    return nullptr;
  }
  return clear_pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool GpuResolver::resolve(ID3D12Device* device, CommandQueue& queue, GuestMemoryMirror& mirror,
                          RenderTargetImage& owner, const GpuResolveConstants& constants) {
  if (!constants.width || !constants.height) return true;
  auto* state = resolve_pipeline(device, owner.host_msaa());
  if (!state) return false;
  auto* source_heap = owner.shader_resource_heap();
  if (!source_heap || !mirror.resource()) {
    error_ = source_heap ? "D3D12 GPU resolve has no guest-memory mirror" : owner.error();
    return false;
  }
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = owner.resource();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        mirror.prepare_shader_access(list, true);
        list->SetPipelineState(state);
        list->SetComputeRootSignature(resolve_root_.Get());
        ID3D12DescriptorHeap* heaps[]{source_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        list->SetComputeRootDescriptorTable(1, source_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(2, mirror.resource()->GetGPUVirtualAddress());
        list->Dispatch((constants.width + 7u) / 8u, (constants.height + 7u) / 8u, 1u);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

bool GpuResolver::clear(ID3D12Device* device, CommandQueue& queue, RenderTargetImage& owner,
                        const GpuClearConstants& constants) {
  auto* state = clear_pipeline(device, owner.format(), owner.host_msaa());
  if (!state) return false;
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        list->SetPipelineState(state);
        list->SetGraphicsRootSignature(clear_root_.Get());
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        const auto rtv = owner.rtv();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(owner.width()), float(owner.height()),
                                      0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, LONG(owner.width()), LONG(owner.height())};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

}  // namespace xenon::gpu::d3d12
