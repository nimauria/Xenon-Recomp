#include "xenon/gpu/d3d12/pipeline.hpp"

namespace xenon::gpu::d3d12 {
namespace {

D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(HostPrimitiveTopology topology) {
  switch (topology) {
    case HostPrimitiveTopology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case HostPrimitiveTopology::LineList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case HostPrimitiveTopology::TriangleList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  }
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

D3D12_PRIMITIVE_TOPOLOGY command_topology(HostPrimitiveTopology topology) {
  switch (topology) {
    case HostPrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case HostPrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case HostPrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

UINT sample_count(MsaaSamples samples) {
  switch (samples) {
    case MsaaSamples::X1: return 1;
    case MsaaSamples::X2: return 2;
    case MsaaSamples::X4: return 4;
  }
  return 1;
}

D3D12_BLEND native_blend_factor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestColor: return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::InvDestColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InvDestAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    case BlendFactor::ConstantColor:
    case BlendFactor::ConstantAlpha: return D3D12_BLEND_BLEND_FACTOR;
    case BlendFactor::InvConstantColor:
    case BlendFactor::InvConstantAlpha: return D3D12_BLEND_INV_BLEND_FACTOR;
    case BlendFactor::SrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
  }
  return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP native_blend_operation(BlendOperation operation) {
  switch (operation) {
    case BlendOperation::Add: return D3D12_BLEND_OP_ADD;
    case BlendOperation::Subtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOperation::Minimum: return D3D12_BLEND_OP_MIN;
    case BlendOperation::Maximum: return D3D12_BLEND_OP_MAX;
    case BlendOperation::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
  }
  return D3D12_BLEND_OP_ADD;
}

D3D12_COMPARISON_FUNC native_compare(CompareFunction function) {
  return static_cast<D3D12_COMPARISON_FUNC>(D3D12_COMPARISON_FUNC_NEVER +
                                            static_cast<unsigned>(function));
}

D3D12_STENCIL_OP native_stencil(StencilOperation operation) {
  return static_cast<D3D12_STENCIL_OP>(D3D12_STENCIL_OP_KEEP +
                                       static_cast<unsigned>(operation));
}

D3D12_DEPTH_STENCILOP_DESC native_stencil_face(const StencilFaceState& face) {
  return {native_stencil(face.fail), native_stencil(face.depth_fail),
          native_stencil(face.depth_pass), native_compare(face.function)};
}

}  // namespace

bool GraphicsPipeline::initialize(ID3D12Device* device,
                                  ID3D12RootSignature* root_signature,
                                  const CompiledShader& vertex_shader,
                                  const CompiledShader& pixel_shader,
                                  std::span<const DXGI_FORMAT> color_formats,
                                  MsaaSamples samples,
                                  HostPrimitiveTopology topology,
                                  const RasterState& raster,
                                  std::span<const std::uint8_t> color_write_masks,
                                  std::span<const BlendState> blend_states,
                                  DXGI_FORMAT depth_format,
                                  const DepthTargetDescriptor* depth_state) {
  reset();
  error_.clear();
  if (!device || !root_signature || color_formats.empty() ||
      color_formats.size() > 4 || color_write_masks.size() != color_formats.size() ||
      blend_states.size() != color_formats.size() ||
      !vertex_shader.succeeded || !pixel_shader.succeeded ||
      vertex_shader.format != ShaderBinaryFormat::Dxil ||
      pixel_shader.format != ShaderBinaryFormat::Dxil ||
      vertex_shader.binary.empty() || pixel_shader.binary.empty()) {
    error_ = "D3D12 graphics pipeline requires valid DXIL shaders and render state";
    return false;
  }
  for (const auto format : color_formats) {
    if (format == DXGI_FORMAT_UNKNOWN) {
      error_ = "D3D12 graphics pipeline received an unsupported color format";
      return false;
    }
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature;
  desc.VS = {vertex_shader.binary.data(), vertex_shader.binary.size()};
  desc.PS = {pixel_shader.binary.data(), pixel_shader.binary.size()};
  desc.BlendState.IndependentBlendEnable = TRUE;
  for (std::size_t i = 0; i < color_formats.size(); ++i) {
    auto& target = desc.BlendState.RenderTarget[i];
    target.BlendEnable = blend_states[i].enabled;
    target.SrcBlend = native_blend_factor(blend_states[i].color_source);
    target.DestBlend = native_blend_factor(blend_states[i].color_destination);
    target.BlendOp = native_blend_operation(blend_states[i].color_operation);
    target.SrcBlendAlpha = native_blend_factor(blend_states[i].alpha_source);
    target.DestBlendAlpha = native_blend_factor(blend_states[i].alpha_destination);
    target.BlendOpAlpha = native_blend_operation(blend_states[i].alpha_operation);
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = color_write_masks[i] & 0xFu;
    desc.RTVFormats[i] = color_formats[i];
  }
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = raster.cull_front && raster.cull_back
                                      ? D3D12_CULL_MODE_NONE
                                      : raster.cull_front ? D3D12_CULL_MODE_FRONT
                                      : raster.cull_back ? D3D12_CULL_MODE_BACK
                                                         : D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = raster.front_face_clockwise ? FALSE : TRUE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  if (depth_state) {
    desc.DepthStencilState.DepthEnable = depth_state->test_enabled;
    desc.DepthStencilState.DepthWriteMask = depth_state->write_enabled
                                                ? D3D12_DEPTH_WRITE_MASK_ALL
                                                : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = native_compare(depth_state->function);
    desc.DepthStencilState.StencilEnable = depth_state->stencil_enabled;
    desc.DepthStencilState.StencilReadMask = depth_state->stencil_read_mask;
    desc.DepthStencilState.StencilWriteMask = depth_state->stencil_write_mask;
    desc.DepthStencilState.FrontFace = native_stencil_face(depth_state->stencil_front);
    desc.DepthStencilState.BackFace = depth_state->backface_stencil_enabled
                                          ? native_stencil_face(depth_state->stencil_back)
                                          : desc.DepthStencilState.FrontFace;
  }
  desc.InputLayout = {nullptr, 0};
  desc.PrimitiveTopologyType = topology_type(topology);
  desc.NumRenderTargets = static_cast<UINT>(color_formats.size());
  desc.DSVFormat = depth_format;
  desc.SampleDesc.Count = sample_count(samples);
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_)))) {
    error_ = "CreateGraphicsPipelineState failed for translated Xenos shaders";
    return false;
  }
  topology_ = command_topology(topology);
  return true;
}

void GraphicsPipeline::reset() noexcept {
  pipeline_.Reset();
  topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

}  // namespace xenon::gpu::d3d12
