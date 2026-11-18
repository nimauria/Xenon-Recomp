#include "xenon/gpu/d3d12/edram_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::d3d12 {

EdramTransferSourceImage edram_transfer_source(RenderTargetImage& image, std::string& error) {
  EdramTransferSourceImage source{};
  source.resource = image.resource();
  source.heap = image.shader_resource_heap();
  if (!source.heap) error = image.error();
  source.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  source.host_msaa = image.host_msaa();
  source.endpoint = {false, static_cast<std::uint8_t>(image.guest_format())};
  return source;
}

EdramTransferSourceImage edram_transfer_source(DepthTargetImage& image, std::string& error) {
  EdramTransferSourceImage source{};
  source.resource = image.resource();
  source.heap = image.shader_resource_heap();
  if (!source.heap) error = image.error();
  source.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  source.host_msaa = image.host_msaa();
  source.endpoint = {true, static_cast<std::uint8_t>(image.guest_format())};
  return source;
}

bool EdramCanonicalStore::create(ID3D12Device* device) {
  if (buffer_) return true;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = Edram::kSize;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // It rests in COPY_DEST; a load moves it to PIXEL_SHADER_RESOURCE and back,
  // a flush to UNORDERED_ACCESS and COPY_SOURCE and back.
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&buffer_)))) {
    error_ = "D3D12 canonical EDRAM buffer creation failed";
    return false;
  }
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  void* readback_mapped = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback_))) ||
      FAILED(readback_->Map(0, nullptr, &readback_mapped))) {
    buffer_.Reset();
    readback_.Reset();
    error_ = "D3D12 canonical EDRAM readback buffer creation failed";
    return false;
  }
  readback_mapped_ = static_cast<const std::byte*>(readback_mapped);
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  void* mapped = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&upload_))) ||
      FAILED(upload_->Map(0, nullptr, &mapped))) {
    buffer_.Reset();
    upload_.Reset();
    error_ = "D3D12 canonical EDRAM upload buffer creation failed";
    return false;
  }
  mapped_ = static_cast<std::byte*>(mapped);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 1;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_)))) {
    buffer_.Reset();
    upload_.Reset();
    mapped_ = nullptr;
    error_ = "D3D12 canonical EDRAM descriptor heap creation failed";
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Buffer.NumElements = Edram::kSize / 4u;
  view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(buffer_.Get(), &view,
                                   heap_->GetCPUDescriptorHandleForHeapStart());
  return true;
}

bool EdramCanonicalStore::upload(ID3D12Device* device, CommandQueue& queue, const Edram& edram,
                                 std::span<const std::uint16_t> tiles) {
  if (tiles.empty()) return true;
  if (!device || !create(device)) {
    if (error_.empty()) error_ = "D3D12 canonical EDRAM upload has no device";
    return false;
  }
  // A tile's upload bytes are reused only once the copy reading them is done.
  std::uint64_t busy = 0;
  for (const auto tile : tiles) busy = (std::max)(busy, tile_busy_until_[tile % Edram::kTileCount]);
  if (busy > queue.completed_value()) {
    if (busy > queue.last_submitted_value() && !queue.flush()) {
      error_ = queue.error();
      return false;
    }
    if (!queue.wait_for_completion(busy)) {
      error_ = queue.error();
      return false;
    }
  }
  // Runs of consecutive tiles, one copy each.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;  // first tile, count
  std::vector<std::uint16_t> sorted(tiles.begin(), tiles.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  const auto bytes = edram.bytes();
  for (const auto tile : sorted) {
    const auto offset = std::size_t(tile % Edram::kTileCount) * Edram::kTileBytes;
    std::memcpy(mapped_ + offset, bytes.data() + offset, Edram::kTileBytes);
    if (!runs.empty() && runs.back().first + runs.back().second == tile)
      ++runs.back().second;
    else
      runs.emplace_back(tile, 1u);
  }
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        for (const auto& [first, count] : runs)
          list->CopyBufferRegion(buffer_.Get(), std::uint64_t(first) * Edram::kTileBytes,
                                 upload_.Get(), std::uint64_t(first) * Edram::kTileBytes,
                                 std::uint64_t(count) * Edram::kTileBytes);
      })) {
    error_ = queue.error();
    return false;
  }
  const auto retirement = queue.retirement_value();
  for (const auto tile : sorted) tile_busy_until_[tile] = retirement;
  return true;
}

ID3D12PipelineState* EdramCanonicalStore::store_pipeline(ID3D12Device* device,
                                                         const EdramTransferSourceImage& source) {
#ifndef XENON_HAS_DXC
  error_ = "D3D12 canonical EDRAM flush requires DXC";
  return nullptr;
#else
  const auto key = std::uint64_t(source.host_msaa) | (std::uint64_t(source.endpoint.depth) << 8u) |
                   (std::uint64_t(source.endpoint.format) << 16u);
  if (const auto found = store_pipelines_.find(key); found != store_pipelines_.end())
    return found->second.Get();
  // Constants at b0, the source's SRVs from t0, the EDRAM buffer as a root
  // UAV at u0 in space 1.
  const std::uint32_t srv_count = source.endpoint.depth ? 2u : 1u;
  auto& root_signature = store_root_signatures_[srv_count - 1u];
  if (!root_signature) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = srv_count;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(EdramTransferConstants) / 4u;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor.ShaderRegister = 0;
    parameters[2].Descriptor.RegisterSpace = 1;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &diagnostics)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                           serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature)))) {
      error_ = "D3D12 canonical EDRAM flush root signature creation failed";
      return nullptr;
    }
  }
  UtilityShaderCompiler compiler;
  const auto shader = make_edram_canonical_store_shader(source.host_msaa, source.endpoint);
  const auto compiled = compiler.compile(shader);
  if (!shader.complete || !compiled.succeeded) {
    error_ = !shader.complete ? "D3D12 canonical EDRAM flush: unsupported format"
             : !compiled.diagnostics.empty() ? compiled.diagnostics.front()
                                             : "D3D12 canonical EDRAM flush shader compilation failed";
    return nullptr;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature.Get();
  desc.CS = {compiled.binary.data(), compiled.binary.size()};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
  if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state)))) {
    error_ = "D3D12 canonical EDRAM flush pipeline creation failed";
    return nullptr;
  }
  return store_pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool EdramCanonicalStore::flush(ID3D12Device* device, CommandQueue& queue,
                                const EdramTransferSourceImage& source,
                                const EdramTransferConstants& constants,
                                std::span<const EdramSurfaceRegion> regions,
                                std::span<const std::uint16_t> tiles, Edram& edram) {
  if (regions.empty() || tiles.empty()) return true;
  if (!device || !source.resource || !source.heap || source.canonical ||
      !edram_canonical_load_supported(source.endpoint)) {
    error_ = "D3D12 canonical EDRAM flush needs a supported native image";
    return false;
  }
  // Untouched words of the covered tiles come back as the CPU store holds them.
  if (!upload(device, queue, edram, tiles)) return false;
  auto* state = store_pipeline(device, source);
  if (!state) return false;
  auto* root_signature = store_root_signatures_[source.endpoint.depth ? 1u : 0u].Get();
  std::vector<std::uint16_t> sorted(tiles.begin(), tiles.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;  // first tile, count
  for (const auto tile : sorted) {
    if (!runs.empty() && runs.back().first + runs.back().second == tile)
      ++runs.back().second;
    else
      runs.emplace_back(tile, 1u);
  }
  const auto host_samples = 1u << static_cast<unsigned>(source.host_msaa);
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
        for (auto& barrier : barriers) {
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource = source.resource;
        barriers[0].Transition.StateBefore = source.state;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.pResource = buffer_.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(2, barriers.data());
        list->SetPipelineState(state);
        list->SetComputeRootSignature(root_signature);
        ID3D12DescriptorHeap* heaps[]{source.heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootDescriptorTable(1, source.heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(2, buffer_->GetGPUVirtualAddress());
        for (const auto& region : regions) {
          const auto width = std::uint32_t(region.right - region.left);
          const auto height = std::uint32_t(region.bottom - region.top);
          if (!width || !height) continue;
          auto region_constants = constants;
          region_constants.source = {std::uint32_t(region.left), std::uint32_t(region.top), width,
                                     height};
          list->SetComputeRoot32BitConstants(0, sizeof(region_constants) / 4u, &region_constants,
                                             0);
          list->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, host_samples);
        }
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(2, barriers.data());
        for (const auto& [first, count] : runs)
          list->CopyBufferRegion(readback_.Get(), std::uint64_t(first) * Edram::kTileBytes,
                                 buffer_.Get(), std::uint64_t(first) * Edram::kTileBytes,
                                 std::uint64_t(count) * Edram::kTileBytes);
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barriers[1]);
      })) {
    error_ = queue.error();
    return false;
  }
  const auto value = queue.retirement_value();
  if ((value > queue.last_submitted_value() && !queue.flush()) ||
      !queue.wait_for_completion(value)) {
    error_ = queue.error();
    return false;
  }
  auto bytes = edram.bytes();
  for (const auto& [first, count] : runs) {
    const auto offset = std::size_t(first) * Edram::kTileBytes;
    std::memcpy(bytes.data() + offset, readback_mapped_ + offset,
                std::size_t(count) * Edram::kTileBytes);
  }
  for (const auto tile : sorted) tile_busy_until_[tile] = value;
  return true;
}

EdramTransferSourceImage EdramCanonicalStore::source() const noexcept {
  EdramTransferSourceImage source{};
  source.resource = buffer_.Get();
  source.heap = heap_.Get();
  source.state = D3D12_RESOURCE_STATE_COPY_DEST;
  source.canonical = true;
  return source;
}

namespace {

std::uint32_t source_srv_count(const EdramTransferSourceImage& source) noexcept {
  return !source.canonical && source.endpoint.depth ? 2u : 1u;
}

// Root constants at b0 for the pixel shader, then a table of the source's
// SRVs from t0.
ID3D12RootSignature* transfer_root_signature(
    ID3D12Device* device, Microsoft::WRL::ComPtr<ID3D12RootSignature>& root_signature,
    std::uint32_t srv_count, std::uint32_t constant_dwords, std::string& error) {
  if (root_signature) return root_signature.Get();
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = srv_count;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.Num32BitValues = constant_dwords;
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
                                         IID_PPV_ARGS(&root_signature)))) {
    error = "D3D12 EDRAM transfer root signature creation failed";
    return nullptr;
  }
  return root_signature.Get();
}

#ifdef XENON_HAS_DXC
// Compiles the fullscreen vertex shader and `pixel` into `desc`'s shader
// bytecode, kept alive in `vs`/`ps`.
bool compile_transfer_shaders(const LoweredShader& pixel, CompiledShader& vs, CompiledShader& ps,
                              std::string& error) {
  UtilityShaderCompiler compiler;
  vs = compiler.compile(make_transfer_fullscreen_vertex_shader());
  ps = compiler.compile(pixel);
  if (vs.succeeded && ps.succeeded) return true;
  error = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
          : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                  : "D3D12 EDRAM transfer shader compilation failed";
  return false;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> create_color_transfer_pipeline(
    ID3D12Device* device, ID3D12RootSignature* root_signature, const LoweredShader& pixel,
    DXGI_FORMAT format, MsaaSamples destination_host, std::string& error) {
  CompiledShader vs, ps;
  if (!compile_transfer_shaders(pixel, vs, ps, error)) return {};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature;
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
    error = "D3D12 EDRAM transfer pipeline creation failed";
    return {};
  }
  return state;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> create_depth_transfer_pipeline(
    ID3D12Device* device, ID3D12RootSignature* root_signature, const LoweredShader& pixel,
    DXGI_FORMAT format, MsaaSamples destination_host, EdramDepthTransferPass pass,
    std::uint32_t stencil_bit, std::string& error) {
  CompiledShader vs, ps;
  if (!compile_transfer_shaders(pixel, vs, ps, error)) return {};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature;
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
    error = "D3D12 EDRAM depth transfer pipeline creation failed";
    return {};
  }
  return state;
}
#endif

void draw_transfer_regions(ID3D12GraphicsCommandList* list,
                           std::span<const EdramSurfaceRegion> regions) {
  for (const auto& region : regions) {
    const D3D12_RECT scissor{region.left, region.top, region.right, region.bottom};
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);
  }
}

}  // namespace

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
  auto* root_signature = transfer_root_signature(device, root_signatures_[0], 1u,
                                                 sizeof(EdramTransferConstants) / 4u, error_);
  if (!root_signature) return nullptr;
  // The integer views are exactly the formats that are not format() itself.
  const bool integer = format == DXGI_FORMAT_R16G16_UINT ||
                       format == DXGI_FORMAT_R16G16B16A16_UINT;
  auto state = create_color_transfer_pipeline(
      device, root_signature, make_edram_color_transfer_shader(source_host, integer), format,
      destination_host, error_);
  if (!state) return nullptr;
  return pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

ID3D12PipelineState* EdramColorTransfer::converting_pipeline(
    ID3D12Device* device, const EdramTransferSourceImage& source,
    const RenderTargetImage& destination) {
#ifndef XENON_HAS_DXC
  error_ = "D3D12 EDRAM transfer requires DXC";
  return nullptr;
#else
  const auto key = std::uint64_t(destination.transfer_format()) |
                   (std::uint64_t(destination.guest_format()) << 8u) |
                   (std::uint64_t(source.endpoint.depth) << 16u) |
                   (std::uint64_t(source.endpoint.format) << 24u) |
                   (std::uint64_t(source.host_msaa) << 32u) |
                   (std::uint64_t(destination.host_msaa()) << 40u) |
                   (std::uint64_t(source.canonical) << 48u);
  if (const auto found = converting_pipelines_.find(key); found != converting_pipelines_.end())
    return found->second.Get();
  const auto srv_count = source_srv_count(source);
  auto* root_signature =
      transfer_root_signature(device, root_signatures_[srv_count - 1u], srv_count,
                              sizeof(EdramTransferConstants) / 4u, error_);
  if (!root_signature) return nullptr;
  auto state = create_color_transfer_pipeline(
      device, root_signature,
      source.canonical
          ? make_edram_canonical_color_load_shader(destination.guest_format())
          : make_edram_converting_color_transfer_shader(source.host_msaa, source.endpoint,
                                                        destination.guest_format()),
      destination.transfer_format(), destination.host_msaa(), error_);
  if (!state) return nullptr;
  return converting_pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool EdramColorTransfer::draw(CommandQueue& queue, ID3D12PipelineState* state,
                              ID3D12RootSignature* root_signature, ID3D12Resource* source,
                              ID3D12DescriptorHeap* source_heap,
                              D3D12_RESOURCE_STATES source_state,
                              RenderTargetImage& destination,
                              const EdramTransferConstants& constants,
                              std::span<const EdramSurfaceRegion> regions) {
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = source_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        list->SetPipelineState(state);
        list->SetGraphicsRootSignature(root_signature);
        ID3D12DescriptorHeap* heaps[]{source_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4u, &constants, 0);
        list->SetGraphicsRootDescriptorTable(1, source_heap->GetGPUDescriptorHandleForHeapStart());
        const auto rtv = destination.transfer_rtv();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(destination.width()),
                                      float(destination.height()), 0.0f, 1.0f};
        list->RSSetViewports(1, &viewport);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        draw_transfer_regions(list, regions);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
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
  auto* state = pipeline(device, destination.transfer_format(), source.host_msaa(),
                         destination.host_msaa());
  if (!state) return false;
  auto* source_heap = source.shader_resource_heap();
  if (!source_heap) {
    error_ = source.error();
    return false;
  }
  return draw(queue, state, root_signatures_[0].Get(), source.resource(), source_heap,
              D3D12_RESOURCE_STATE_RENDER_TARGET, destination, constants, regions);
}

bool EdramColorTransfer::convert(ID3D12Device* device, CommandQueue& queue,
                                 const EdramTransferSourceImage& source,
                                 RenderTargetImage& destination,
                                 const EdramTransferConstants& constants,
                                 std::span<const EdramSurfaceRegion> regions) {
  const EdramTransferEndpoint target{false, static_cast<std::uint8_t>(destination.guest_format())};
  if (!device || !source.resource || !source.heap || !destination.resource() ||
      source.resource == destination.resource() ||
      !edram_converting_transfer_supported(source.endpoint, target)) {
    error_ = "D3D12 converting EDRAM transfer needs a supported source and color destination";
    return false;
  }
  if (regions.empty()) return true;
  auto* state = converting_pipeline(device, source, destination);
  if (!state) return false;
  return draw(queue, state, root_signatures_[source_srv_count(source) - 1u].Get(),
              source.resource, source.heap, source.state, destination, constants, regions);
}

bool EdramColorTransfer::load_canonical(ID3D12Device* device, CommandQueue& queue,
                                        const EdramTransferSourceImage& canonical,
                                        RenderTargetImage& destination,
                                        const EdramTransferConstants& constants,
                                        std::span<const EdramSurfaceRegion> regions) {
  const EdramTransferEndpoint target{false, static_cast<std::uint8_t>(destination.guest_format())};
  if (!device || !canonical.canonical || !canonical.resource || !canonical.heap ||
      !destination.resource() || !edram_canonical_load_supported(target)) {
    error_ = "D3D12 canonical EDRAM load needs the canonical store and a supported color image";
    return false;
  }
  if (regions.empty()) return true;
  auto* state = converting_pipeline(device, canonical, destination);
  if (!state) return false;
  return draw(queue, state, root_signatures_[0].Get(), canonical.resource, canonical.heap,
              canonical.state, destination, constants, regions);
}

ID3D12PipelineState* EdramDepthTransfer::pipeline(ID3D12Device* device,
                                                  const EdramTransferSourceImage& source,
                                                  const DepthTargetImage& destination,
                                                  bool converting, EdramDepthTransferPass pass,
                                                  std::uint32_t stencil_bit) {
#ifndef XENON_HAS_DXC
  error_ = "D3D12 EDRAM depth transfer requires DXC";
  return nullptr;
#else
  const auto source_host = source.host_msaa, destination_host = destination.host_msaa();
  const auto format = destination.format();
  const auto key = std::uint64_t(format) | (std::uint64_t(destination.guest_format()) << 8u) |
                   (std::uint64_t(converting) << 12u) |
                   (std::uint64_t(source.endpoint.depth) << 13u) |
                   (std::uint64_t(source.endpoint.format) << 16u) |
                   (std::uint64_t(source_host) << 32u) | (std::uint64_t(destination_host) << 40u) |
                   (std::uint64_t(pass) << 48u) | (std::uint64_t(stencil_bit & 7u) << 56u) |
                   (std::uint64_t(destination.unorm24_depth_write()) << 60u) |
                   (std::uint64_t(source.canonical) << 63u);
  if (const auto found = pipelines_.find(key); found != pipelines_.end())
    return found->second.Get();
  const auto srv_count = source_srv_count(source);
  auto* root_signature =
      transfer_root_signature(device, root_signatures_[srv_count - 1u], srv_count,
                              sizeof(EdramDepthTransferConstants) / 4u, error_);
  if (!root_signature) return nullptr;
  const auto write = destination.unorm24_depth_write();
  const auto pixel =
      source.canonical
          ? make_edram_canonical_depth_load_shader(destination.guest_format(), pass, write)
      : converting ? make_edram_converting_depth_transfer_shader(
                         source_host, source.endpoint, destination.guest_format(), pass, write)
                   : make_edram_depth_transfer_shader(source_host, pass,
                                                      destination.guest_format(), write);
  auto state = create_depth_transfer_pipeline(device, root_signature, pixel, format,
                                              destination_host, pass, stencil_bit, error_);
  if (!state) return nullptr;
  return pipelines_.emplace(key, std::move(state)).first->second.Get();
#endif
}

bool EdramDepthTransfer::run(ID3D12Device* device, CommandQueue& queue,
                             const EdramTransferSourceImage& source,
                             DepthTargetImage& destination, bool converting,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions) {
  if (regions.empty()) return true;
  if (stencil_ref_supported_ < 0) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    stencil_ref_supported_ =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                              sizeof(options))) &&
        options.PSSpecifiedStencilRefSupported;
  }
  const bool combined = stencil_ref_supported_ == 1;
  std::array<ID3D12PipelineState*, 9> passes{};
  passes[0] = pipeline(device, source, destination, converting,
                       combined ? EdramDepthTransferPass::DepthAndStencil
                                : EdramDepthTransferPass::Depth,
                       0u);
  if (!passes[0]) return false;
  if (!combined) {
    for (std::uint32_t bit = 0; bit < 8u; ++bit) {
      passes[bit + 1u] = pipeline(device, source, destination, converting,
                                  EdramDepthTransferPass::StencilBit, bit);
      if (!passes[bit + 1u]) return false;
    }
  }
  auto* root_signature = root_signatures_[source_srv_count(source) - 1u].Get();
  EdramDepthTransferConstants values{};
  values.transfer = constants;
  if (!queue.execute_async([&](ID3D12GraphicsCommandList* list, std::uint32_t, std::uint32_t) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source.resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = source.state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        list->SetGraphicsRootSignature(root_signature);
        ID3D12DescriptorHeap* heaps[]{source.heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(1, source.heap->GetGPUDescriptorHandleForHeapStart());
        const auto dsv = destination.dsv();
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(destination.width()),
                                      float(destination.height()), 0.0f, 1.0f};
        list->RSSetViewports(1, &viewport);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Depth (and stencil, or stencil cleared to zero).
        list->SetPipelineState(passes[0]);
        list->SetGraphicsRoot32BitConstants(0, sizeof(values) / 4u, &values, 0);
        list->OMSetStencilRef(0u);
        draw_transfer_regions(list, regions);
        if (!combined) {
          // Each stencil bit the source sample has set, written alone.
          list->OMSetStencilRef(0xFFu);
          for (std::uint32_t bit = 0; bit < 8u; ++bit) {
            values.stencil[0] = 1u << bit;
            list->SetPipelineState(passes[bit + 1u]);
            list->SetGraphicsRoot32BitConstants(0, sizeof(values) / 4u, &values, 0);
            draw_transfer_regions(list, regions);
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
  const auto view = edram_transfer_source(source, error_);
  if (!view.heap) return false;
  return run(device, queue, view, destination, false, constants, regions);
}

bool EdramDepthTransfer::convert(ID3D12Device* device, CommandQueue& queue,
                                 const EdramTransferSourceImage& source,
                                 DepthTargetImage& destination,
                                 const EdramTransferConstants& constants,
                                 std::span<const EdramSurfaceRegion> regions) {
  const EdramTransferEndpoint target{true, static_cast<std::uint8_t>(destination.guest_format())};
  if (!device || !source.resource || !source.heap || !destination.resource() ||
      source.resource == destination.resource() ||
      !edram_converting_transfer_supported(source.endpoint, target)) {
    error_ = "D3D12 converting EDRAM transfer needs a supported source and depth destination";
    return false;
  }
  return run(device, queue, source, destination, true, constants, regions);
}

bool EdramDepthTransfer::load_canonical(ID3D12Device* device, CommandQueue& queue,
                                        const EdramTransferSourceImage& canonical,
                                        DepthTargetImage& destination,
                                        const EdramTransferConstants& constants,
                                        std::span<const EdramSurfaceRegion> regions) {
  const EdramTransferEndpoint target{true, static_cast<std::uint8_t>(destination.guest_format())};
  if (!device || !canonical.canonical || !canonical.resource || !canonical.heap ||
      !destination.resource() || !edram_canonical_load_supported(target)) {
    error_ = "D3D12 canonical EDRAM load needs the canonical store and a supported depth image";
    return false;
  }
  return run(device, queue, canonical, destination, true, constants, regions);
}

}  // namespace xenon::gpu::d3d12
