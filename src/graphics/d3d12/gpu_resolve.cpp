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

ID3D12RootSignature* GpuResolver::resolve_root(ID3D12Device* device, std::uint32_t srvs) {
  auto& root = resolve_roots_[srvs - 1u];
  if (root) return root.Get();
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = srvs;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = sizeof(GpuResolveConstants) / 4u;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &range;
  // The mirror, written by physical address.
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  if (!create_root(device, parameters, root)) {
    error_ = "D3D12 GPU resolve root signature creation failed";
    return nullptr;
  }
  return root.Get();
}

ID3D12PipelineState* GpuResolver::resolve_pipeline(ID3D12Device* device, MsaaSamples samples,
                                                   const EdramTransferEndpoint& source) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)samples;
  (void)source;
  error_ = "D3D12 GPU resolve requires DXC";
  return nullptr;
#else
  const auto key = static_cast<std::uint32_t>(samples) | (source.depth ? 0x100u : 0u) |
                   (source.depth ? std::uint32_t(source.format) << 16u : 0u);
  if (const auto found = resolve_pipelines_.find(key); found != resolve_pipelines_.end())
    return found->second.Get();
  auto* root = resolve_root(device, source.depth ? 2u : 1u);
  if (!root) return nullptr;
  UtilityShaderCompiler compiler;
  const auto cs = compiler.compile(
      source.depth ? make_gpu_depth_resolve_shader(
                         samples, static_cast<DepthRenderTargetFormat>(source.format))
                   : make_gpu_resolve_shader(samples));
  if (!cs.succeeded) {
    error_ = cs.diagnostics.empty() ? "D3D12 GPU resolve shader compilation failed"
                                    : cs.diagnostics.front();
    return nullptr;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.CS = {cs.binary.data(), cs.binary.size()};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 GPU resolve pipeline creation failed";
    return nullptr;
  }
  return resolve_pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

ID3D12RootSignature* GpuResolver::clear_root(ID3D12Device* device) {
  if (clear_root_) return clear_root_.Get();
  std::array<D3D12_ROOT_PARAMETER, 1> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = sizeof(GpuClearConstants) / 4u;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  if (!create_root(device, parameters, clear_root_)) {
    error_ = "D3D12 GPU resolve clear root signature creation failed";
    return nullptr;
  }
  return clear_root_.Get();
}

ID3D12PipelineState* GpuResolver::clear_pipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                 MsaaSamples samples, bool depth) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)format;
  (void)samples;
  (void)depth;
  error_ = "D3D12 GPU resolve clear requires DXC";
  return nullptr;
#else
  const auto key = std::uint64_t(format) | (std::uint64_t(samples) << 32u) |
                   (std::uint64_t(depth) << 40u);
  if (const auto found = clear_pipelines_.find(key); found != clear_pipelines_.end())
    return found->second.Get();
  auto* root = clear_root(device);
  if (!root) return nullptr;
  UtilityShaderCompiler compiler;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  const auto ps =
      compiler.compile(depth ? make_edram_depth_clear_shader() : make_edram_clear_shader());
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                     : "D3D12 GPU resolve clear shader compilation failed";
    return nullptr;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.VS = {vs.binary.data(), vs.binary.size()};
  desc.PS = {ps.binary.data(), ps.binary.size()};
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.SampleDesc.Count = 1u << static_cast<unsigned>(samples);
  if (depth) {
    // Every sample the shader keeps takes the clear depth, and the stencil
    // reference through REPLACE.
    desc.DSVFormat = format;
    auto& depth_stencil = desc.DepthStencilState;
    depth_stencil.DepthEnable = TRUE;
    depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth_stencil.StencilEnable = TRUE;
    depth_stencil.StencilReadMask = 0xFF;
    depth_stencil.StencilWriteMask = 0xFF;
    depth_stencil.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depth_stencil.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth_stencil.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    depth_stencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth_stencil.BackFace = depth_stencil.FrontFace;
  } else {
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
  }
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 GPU resolve clear pipeline creation failed";
    return nullptr;
  }
  return clear_pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool GpuResolver::run_resolve(ID3D12Device* device, CommandQueue& queue,
                              GuestMemoryMirror& mirror, const EdramTransferSourceImage& source,
                              const GpuResolveConstants& constants) {
  if (!constants.width || !constants.height) return true;
  auto* state = resolve_pipeline(device, source.host_msaa, source.endpoint);
  if (!state) return false;
  auto* root = resolve_roots_[source.endpoint.depth ? 1 : 0].Get();
  if (!mirror.resource()) {
    error_ = "D3D12 GPU resolve has no guest-memory mirror";
    return false;
  }
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source.resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = source.state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        mirror.prepare_shader_access(list, true);
        list->SetPipelineState(state);
        list->SetComputeRootSignature(root);
        ID3D12DescriptorHeap* heaps[]{source.heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        list->SetComputeRootDescriptorTable(1, source.heap->GetGPUDescriptorHandleForHeapStart());
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

bool GpuResolver::resolve(ID3D12Device* device, CommandQueue& queue, GuestMemoryMirror& mirror,
                          RenderTargetImage& owner, const GpuResolveConstants& constants) {
  std::string source_error;
  const auto source = edram_transfer_source(owner, source_error);
  if (!source.heap) {
    error_ = source_error;
    return false;
  }
  return run_resolve(device, queue, mirror, source, constants);
}

bool GpuResolver::resolve_depth(ID3D12Device* device, CommandQueue& queue,
                                GuestMemoryMirror& mirror, DepthTargetImage& owner,
                                const GpuResolveConstants& constants) {
  std::string source_error;
  const auto source = edram_transfer_source(owner, source_error);
  if (!source.heap) {
    error_ = source_error;
    return false;
  }
  return run_resolve(device, queue, mirror, source, constants);
}

namespace {

// The clear draw over the whole owner; its shader keeps only the samples the
// rectangle covers.
void draw_clear(ID3D12GraphicsCommandList* list, std::uint32_t width, std::uint32_t height) {
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace

bool GpuResolver::clear(ID3D12Device* device, CommandQueue& queue, RenderTargetImage& owner,
                        const GpuClearConstants& constants) {
  auto* state = clear_pipeline(device, owner.format(), owner.host_msaa(), false);
  if (!state) return false;
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        list->SetPipelineState(state);
        list->SetGraphicsRootSignature(clear_root_.Get());
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        const auto rtv = owner.rtv();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        draw_clear(list, owner.width(), owner.height());
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

bool GpuResolver::clear_depth(ID3D12Device* device, CommandQueue& queue, DepthTargetImage& owner,
                              const GpuClearConstants& constants, std::uint8_t stencil) {
  auto* state = clear_pipeline(device, owner.format(), owner.host_msaa(), true);
  if (!state) return false;
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        list->SetPipelineState(state);
        list->SetGraphicsRootSignature(clear_root_.Get());
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        const auto dsv = owner.dsv();
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        list->OMSetStencilRef(stencil);
        draw_clear(list, owner.width(), owner.height());
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

}  // namespace xenon::gpu::d3d12
