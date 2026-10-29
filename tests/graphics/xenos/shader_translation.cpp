#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace {

xenon::gpu::DecodedShader make_shader(xenon::gpu::ShaderStage stage) {
  using namespace xenon::gpu;
  DecodedShader shader{};
  shader.stage = stage;
  shader.source_hash = stage == ShaderStage::Vertex ? 0x123456789ABCDEF0ull
                                                    : 0x0FEDCBA987654321ull;
  shader.complete = true;
  shader.reflection.temporary_register_count = 1;
  shader.reflection.exports = {stage == ShaderStage::Vertex ? std::uint8_t{62}
                                                             : std::uint8_t{0}};

  DecodedControlFlow cf{};
  cf.opcode = ControlFlowOpcode::ExecEnd;
  cf.target = 1;
  cf.count = 1;
  shader.control_flow.push_back(cf);

  DecodedInstruction instruction{};
  instruction.kind = ShaderInstructionKind::Alu;
  instruction.alu.address = 1;
  instruction.alu.vector_opcode = 0;
  instruction.alu.scalar_opcode = 50;
  instruction.alu.vector_destination = stage == ShaderStage::Vertex ? 62 : 0;
  instruction.alu.vector_write_mask = 0xF;
  instruction.alu.export_data = true;
  instruction.alu.sources[0].temporary = true;
  instruction.alu.sources[0].index = 0;
  instruction.alu.sources[0].swizzle = 0xE4;
  instruction.alu.sources[1] = instruction.alu.sources[0];
  instruction.alu.sources[2] = instruction.alu.sources[0];
  shader.instructions.push_back(instruction);
  return shader;
}

void test_lowering_is_deterministic() {
  using namespace xenon::gpu;
  const auto shader = make_shader(ShaderStage::Vertex);
  const auto first = HlslShaderLowerer::lower(shader);
  const auto second = HlslShaderLowerer::lower(shader);
  assert(first.complete);
  assert(first.hlsl == second.hlsl);
  assert(first.translation_hash == second.translation_hash);
  assert(first.profile == "vs_6_0");
  assert(first.hlsl.find("XenonFloatConstants") != std::string::npos);
  assert(first.hlsl.find("output.position=xenon_host_position(e[62])") != std::string::npos);
}

void test_dxc_dual_target_and_cache() {
  using namespace xenon::gpu;
  auto compiler = std::make_shared<DxcShaderCompiler>();
  assert(compiler->available());
  ShaderCache cache(compiler);
  const auto lowered = HlslShaderLowerer::lower(make_shader(ShaderStage::Vertex));

  ShaderCompileOptions dxil_options{};
  dxil_options.format = ShaderBinaryFormat::Dxil;
  const auto dxil = cache.get_or_compile(lowered, dxil_options);
  if (!dxil->succeeded) {
    for (const auto& diagnostic : dxil->diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(dxil->succeeded && dxil->binary.size() > 4);
  assert(std::memcmp(dxil->binary.data(), "DXBC", 4) == 0);
  const auto dxil_again = cache.get_or_compile(lowered, dxil_options);
  assert(dxil == dxil_again);

  ShaderCompileOptions spirv_options{};
  spirv_options.format = ShaderBinaryFormat::Spirv;
  const auto spirv = cache.get_or_compile(lowered, spirv_options);
  if (!spirv->succeeded) {
    for (const auto& diagnostic : spirv->diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(spirv->succeeded && spirv->binary.size() > 4);
  std::uint32_t magic{};
  std::memcpy(&magic, spirv->binary.data(), sizeof(magic));
  assert(magic == 0x07230203u);
  assert(cache.size() == 2 && cache.hits() == 1 && cache.misses() == 2);
}

void test_pixel_target_and_incomplete_rejection() {
  using namespace xenon::gpu;
  DxcShaderCompiler compiler;
  auto pixel_shader = make_shader(ShaderStage::Pixel);
  pixel_shader.instructions[0].alu.sources[0].temporary = false;
  const auto pixel = HlslShaderLowerer::lower(pixel_shader);
  assert(pixel.complete && pixel.profile == "ps_6_0");
  assert(pixel.hlsl.find("& 255) + 256]") != std::string::npos);
  assert(pixel.hlsl.find("uint coverage : SV_Coverage") != std::string::npos);
  assert(pixel.hlsl.find("xenon_compare(e[0].w") != std::string::npos);
  assert(pixel.hlsl.find("xenon_alpha_to_mask(e[0].w") != std::string::npos);
  assert(pixel.hlsl.find("0.75 - o / 16.0") != std::string::npos);
  assert(compiler.compile(pixel, {}).succeeded);

  pixel_shader.reflection.exports.clear();
  const auto no_color_zero = HlslShaderLowerer::lower(pixel_shader);
  assert(no_color_zero.complete);
  assert(no_color_zero.hlsl.find("xenon_compare(e[0].w") == std::string::npos);
  assert(no_color_zero.hlsl.find("output.coverage = 0xFFFFFFFFu") !=
         std::string::npos);
  assert(compiler.compile(no_color_zero, {}).succeeded);

  DecodedShader incomplete{};
  incomplete.diagnostics.emplace_back("fixture failure");
  const auto rejected = HlslShaderLowerer::lower(incomplete);
  assert(!rejected.complete && !rejected.diagnostics.empty());
}

void test_control_flow_state_machine_and_explicit_rejection() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Vertex);
  shader.control_flow.clear();
  DecodedControlFlow loop_start{};
  loop_start.index = 0;
  loop_start.opcode = ControlFlowOpcode::LoopStart;
  loop_start.target = 3;
  loop_start.loop_constant = 2;
  shader.control_flow.push_back(loop_start);
  DecodedControlFlow body{};
  body.index = 1;
  body.opcode = ControlFlowOpcode::Exec;
  body.target = 1;
  body.count = 1;
  shader.control_flow.push_back(body);
  DecodedControlFlow loop_end{};
  loop_end.index = 2;
  loop_end.opcode = ControlFlowOpcode::LoopEnd;
  loop_end.target = 1;
  loop_end.loop_constant = 2;
  shader.control_flow.push_back(loop_end);
  DecodedControlFlow end = body;
  end.index = 3;
  end.opcode = ControlFlowOpcode::ExecEnd;
  shader.control_flow.push_back(end);
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("switch (pc)") != std::string::npos);
  assert(lowered.hlsl.find("loop_remaining") != std::string::npos);
  DxcShaderCompiler compiler;
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);

  // getTextureBorderColorFrac has no lowering (Xenia/rexglue lack one too):
  // it must be rejected explicitly, never silently produce a value.
  auto unsupported = make_shader(ShaderStage::Pixel);
  unsupported.instructions[0].kind = ShaderInstructionKind::TextureFetch;
  unsupported.instructions[0].texture_fetch.address = 1;
  unsupported.instructions[0].texture_fetch.opcode = 16;
  const auto rejected = HlslShaderLowerer::lower(unsupported);
  assert(!rejected.complete && !rejected.diagnostics.empty());
  // Derivative-based queries are undefined outside pixel shaders.
  auto vertex_query = make_shader(ShaderStage::Vertex);
  vertex_query.instructions[0].kind = ShaderInstructionKind::TextureFetch;
  vertex_query.instructions[0].texture_fetch.address = 1;
  vertex_query.instructions[0].texture_fetch.opcode = 17;
  assert(!HlslShaderLowerer::lower(vertex_query).complete);
  vertex_query.instructions[0].texture_fetch.opcode = 18;
  assert(!HlslShaderLowerer::lower(vertex_query).complete);
}

void test_texture_lod_and_gradient_lowering() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Pixel);
  auto& instruction = shader.instructions[0];
  instruction.kind = ShaderInstructionKind::TextureFetch;
  instruction.texture_fetch.address = 1;
  instruction.texture_fetch.opcode = 1;
  instruction.texture_fetch.fetch_constant = 3;
  instruction.texture_fetch.dimension = 1;
  instruction.texture_fetch.source_register = 4;
  instruction.texture_fetch.destination_register = 5;
  instruction.texture_fetch.destination_swizzle = 0x688;
  instruction.texture_fetch.use_register_gradients = true;
  instruction.texture_fetch.unnormalized_coordinates = true;
  instruction.texture_fetch.offsets_half_texels = {1, -1, 0};
  shader.reflection.texture_fetch_constants = {3};
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("SampleGrad") != std::string::npos);
  // Register gradients come from setTextureGradientsHorz/Vert state, not
  // from whatever happens to sit in the temporaries after the coordinate.
  assert(lowered.hlsl.find(
             "true, xenon_grad_h, xenon_grad_v, xenon_register_lod, true") !=
         std::string::npos);
  assert(lowered.hlsl.find("float4 grad_h = r[") == std::string::npos);
  assert(lowered.hlsl.find("xenon_apply_texture_exp_adjust(3, fetched)") !=
         std::string::npos);
  assert(lowered.hlsl.find(
             "XenonVertexFetchConstants[fetch_constant & 31].y") !=
         std::string::npos);
  DxcShaderCompiler compiler;
  assert(compiler.compile(lowered, {}).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

// Regressions: vertex fetches ignored const_index_sel (two of the three
// vertex fetch constants in each slot were unreachable), used the raw word 0
// (type bits included) as the byte address, and never applied the fetch
// constant's endian swap to big-endian guest vertex data. 10_11_11/11_11_10
// were rejected outright.
void test_vertex_fetch_select_endian_and_packed_formats() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Vertex);
  DecodedInstruction fetch{};
  fetch.kind = ShaderInstructionKind::VertexFetch;
  fetch.vertex_fetch.address = 1;
  fetch.vertex_fetch.fetch_constant = 5;
  fetch.vertex_fetch.fetch_constant_select = 2;
  fetch.vertex_fetch.destination_register = 0;
  fetch.vertex_fetch.destination_swizzle = 0x688;
  fetch.vertex_fetch.stride_dwords = 3;
  fetch.vertex_fetch.data_format = 16;  // 10_11_11
  fetch.vertex_fetch.normalized = true;
  fetch.vertex_fetch.signed_data = true;
  shader.instructions.insert(shader.instructions.begin(), fetch);
  shader.instructions[1].alu.address = 2;
  shader.control_flow[0].count = 2;
  const auto lowered = HlslShaderLowerer::lower(shader);
  if (!lowered.complete) {
    for (const auto& diagnostic : lowered.diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(lowered.complete);
  assert(lowered.hlsl.find(
             "uint source_index = (uint)floor(r[(0) & 63][0] + 0.0);") !=
         std::string::npos);
  assert(lowered.hlsl.find("source_index = asuint") == std::string::npos);
  assert(lowered.hlsl.find("xenon_vertex_fetch(5, 2, source_index") != std::string::npos);
  assert(lowered.hlsl.find("select == 0u ? slot.x : (select == 1u ? slot.z : slot.w)") !=
         std::string::npos);
  assert(lowered.hlsl.find("(packed & ~3u)") != std::string::npos);
  assert(lowered.hlsl.find("xenon_endian_swap(XenonGuestMemory.Load4(address), packed & 3u)") !=
         std::string::npos);
  DxcShaderCompiler compiler;
  assert(compiler.compile(lowered, {}).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);

  shader.instructions[0].vertex_fetch.data_format = 17;  // 11_11_10
  assert(HlslShaderLowerer::lower(shader).complete);
  shader.instructions[0].vertex_fetch.index_rounded = true;
  const auto rounded = HlslShaderLowerer::lower(shader);
  assert(rounded.complete);
  assert(rounded.hlsl.find(
             "uint source_index = (uint)floor(r[(0) & 63][0] + 0.5);") !=
         std::string::npos);
  shader.instructions[0].vertex_fetch.data_format = 63;  // not a vertex format
  assert(!HlslShaderLowerer::lower(shader).complete);
}

// Regression (Ace Combat 6 logos): the vertex shader's r0.x is the vertex
// grouper's index - the draw's index plus VGT_INDX_OFFSET, 24 bits, clamped
// to [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX] - not the host's raw vertex id, or
// every quad drawn at a StartVertex fetches the buffer's first vertices.
void test_vertex_index_applies_grouper_offset_and_clamp() {
  using namespace xenon::gpu;
  const auto lowered = HlslShaderLowerer::lower(make_shader(ShaderStage::Vertex));
  assert(lowered.complete);
  assert(lowered.hlsl.find("r[0].x = (float)xenon_vertex_index(vertex_id);") !=
         std::string::npos);
  assert(lowered.hlsl.find("r[0].x = (float)vertex_id;") == std::string::npos);
  assert(lowered.hlsl.find("uint4 vgt = XenonDrawState[1];") != std::string::npos);
  assert(lowered.hlsl.find(
             "clamp((index + vgt.y) & 0xFFFFFFu, vgt.z & 0xFFFFFFu, vgt.w & 0xFFFFFFu)") !=
         std::string::npos);
  // Regression (Ace Combat 6 age-rating screen): the guest viewport
  // transform runs in the shader, so a pre-transformed Clear rectangle in
  // render-target pixels lands where the guest put it instead of being
  // scaled by a stale viewport and clipped.
  assert(lowered.hlsl.find("output.position=xenon_host_position(e[62]);") != std::string::npos);
  assert(lowered.hlsl.find("if ((flags & 4u) == 0u) position.w = 1.0 / position.w;") !=
         std::string::npos);
  assert(lowered.hlsl.find(
             "float2 pixels = position.xy * transform.xy + transform.zw * position.w;") !=
         std::string::npos);
  assert(lowered.hlsl.find("pixels.x * to_ndc.x - position.w, position.w - pixels.y * to_ndc.y") !=
         std::string::npos);
  DxcShaderCompiler compiler;
  assert(compiler.compile(lowered, {}).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

// Every Xenos texture fetch opcode with defined semantics lowers and compiles
// for both DXIL and SPIR-V: the LOD/gradient setters feed per-invocation state
// read by later fetches, and the queries return LOD, derivatives and bilinear
// weights.
void test_texture_state_and_query_opcodes() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Pixel);
  const auto export_alu = shader.instructions[0];
  shader.instructions.clear();
  const auto texture = [&](std::uint32_t address, std::uint8_t opcode) {
    DecodedInstruction instruction{};
    instruction.kind = ShaderInstructionKind::TextureFetch;
    instruction.texture_fetch.address = address;
    instruction.texture_fetch.opcode = opcode;
    instruction.texture_fetch.fetch_constant = 2;
    instruction.texture_fetch.dimension = 1;
    instruction.texture_fetch.source_register = 1;
    instruction.texture_fetch.destination_register = 2;
    instruction.texture_fetch.destination_swizzle = 0x688;
    instruction.texture_fetch.source_swizzle = 0xE4 & 0x3F;
    return instruction;
  };
  shader.instructions.push_back(texture(1, 24));  // setTextureLod
  shader.instructions.push_back(texture(2, 25));  // setTextureGradientsHorz
  shader.instructions.push_back(texture(3, 26));  // setTextureGradientsVert
  auto fetch_lod = texture(4, 1);
  fetch_lod.texture_fetch.use_register_lod = true;
  shader.instructions.push_back(fetch_lod);
  auto fetch_grad = texture(5, 1);
  fetch_grad.texture_fetch.use_computed_lod = true;
  fetch_grad.texture_fetch.use_register_gradients = true;
  shader.instructions.push_back(fetch_grad);
  shader.instructions.push_back(texture(6, 17));  // getTextureComputedLod
  shader.instructions.push_back(texture(7, 18));  // getTextureGradients
  shader.instructions.push_back(texture(8, 19));  // getTextureWeights
  auto alu = export_alu;
  alu.alu.address = 9;
  shader.instructions.push_back(alu);
  shader.control_flow[0].target = 1;
  shader.control_flow[0].count = 9;
  shader.reflection.texture_fetch_constants = {2};
  shader.reflection.temporary_register_count = 3;

  const auto lowered = HlslShaderLowerer::lower(shader);
  if (!lowered.complete) {
    for (const auto& diagnostic : lowered.diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(lowered.complete);
  assert(lowered.hlsl.find("xenon_register_lod = coord.x;") != std::string::npos);
  assert(lowered.hlsl.find("xenon_grad_h = float4(coord.xyz, 0.0);") != std::string::npos);
  assert(lowered.hlsl.find("xenon_grad_v = float4(coord.xyz, 0.0);") != std::string::npos);
  assert(lowered.hlsl.find("xenon_texture_computed_lod(2, 1, coord") != std::string::npos);
  assert(lowered.hlsl.find("ddx_coarse(coord.x), ddy_coarse(coord.x)") != std::string::npos);
  assert(lowered.hlsl.find("xenon_texture_weights(2, 1, coord") != std::string::npos);
  // The fetch-constant LOD bias is part of every explicit LOD.
  assert(lowered.hlsl.find("xenon_texture_constant_lod_bias(fetch_constant) + instruction_lod_bias") !=
         std::string::npos);
  assert(lowered.hlsl.find("#define XENON_PIXEL_SHADER 1") != std::string::npos);
  DxcShaderCompiler compiler;
  const auto dxil = compiler.compile(lowered, {});
  if (!dxil.succeeded) {
    for (const auto& diagnostic : dxil.diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(dxil.succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

// Regression: a vertex shader tfetch with use_computed_lod (common - the
// compiler sets it by default) emitted SampleBias, which needs implicit
// derivatives that vertex shaders do not have. It must use an explicit LOD.
void test_vertex_shader_texture_fetch_uses_explicit_lod() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Vertex);
  DecodedInstruction fetch{};
  fetch.kind = ShaderInstructionKind::TextureFetch;
  fetch.texture_fetch.address = 1;
  fetch.texture_fetch.opcode = 1;
  fetch.texture_fetch.fetch_constant = 4;
  fetch.texture_fetch.dimension = 1;
  fetch.texture_fetch.use_computed_lod = true;
  fetch.texture_fetch.destination_swizzle = 0x688;
  shader.instructions.insert(shader.instructions.begin(), fetch);
  shader.instructions[1].alu.address = 2;
  shader.control_flow[0].count = 2;
  shader.reflection.texture_fetch_constants = {4};
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("#define XENON_PIXEL_SHADER") == std::string::npos);
  DxcShaderCompiler compiler;
  const auto dxil = compiler.compile(lowered, {});
  if (!dxil.succeeded) {
    for (const auto& diagnostic : dxil.diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(dxil.succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

struct Point2 {
  double x, y;
};

double signed_area(Point2 p, Point2 q, Point2 r) {
  return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
}

bool strictly_inside(Point2 p, Point2 a, Point2 b, Point2 c) {
  const double s0 = signed_area(a, b, p), s1 = signed_area(b, c, p),
               s2 = signed_area(c, a, p);
  return (s0 > 0 && s1 > 0 && s2 > 0) || (s0 < 0 && s1 < 0 && s2 < 0);
}

// Regression (AC6 logo fade): the geometry shader emitted the strip A,C,B,D,
// whose shared v1-v2 edge C-B is a side, not the diagonal, so a corner wedge of
// every rectangle stayed undrawn. Model the emitted strip on the CPU: every
// interior point must be covered exactly once, with the input's winding.
bool rectangle_strip_covers(const std::vector<char>& order,
                            const std::array<Point2, 3>& input) {
  // The shader's own vertex classification: A-B is the longest edge.
  const auto d2 = [](Point2 p, Point2 q) {
    return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
  };
  const double d01 = d2(input[0], input[1]), d12 = d2(input[1], input[2]),
               d20 = d2(input[2], input[0]);
  Point2 a, b, c;
  if (d01 >= d12 && d01 >= d20) { a = input[0]; b = input[1]; c = input[2]; }
  else if (d12 >= d20) { a = input[1]; b = input[2]; c = input[0]; }
  else { a = input[2]; b = input[0]; c = input[1]; }
  const Point2 d{a.x + b.x - c.x, a.y + b.y - c.y};
  std::array<Point2, 4> strip{};
  if (order.size() != 4u) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    switch (order[i]) {
      case 'a': strip[i] = a; break;
      case 'b': strip[i] = b; break;
      case 'c': strip[i] = c; break;
      case 'd': strip[i] = d; break;
      default: return false;
    }
  }
  // D3D/Vulkan strips: (v0,v1,v2), then (v2,v1,v3) for the odd triangle.
  const double input_winding = signed_area(input[0], input[1], input[2]);
  if (signed_area(strip[0], strip[1], strip[2]) * input_winding <= 0) return false;
  if (signed_area(strip[2], strip[1], strip[3]) * input_winding <= 0) return false;

  double min_x = a.x, max_x = a.x, min_y = a.y, max_y = a.y;
  for (const auto& p : {b, c, d}) {
    min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y);
  }
  for (int iy = 1; iy < 64; ++iy) {
    for (int ix = 1; ix < 64; ++ix) {
      const Point2 p{min_x + (max_x - min_x) * (ix + 0.37) / 64.0,
                     min_y + (max_y - min_y) * (iy + 0.21) / 64.0};
      const int covered = int(strictly_inside(p, strip[0], strip[1], strip[2])) +
                          int(strictly_inside(p, strip[1], strip[2], strip[3]));
      if (covered != 1) return false;
    }
  }
  return true;
}

void test_rectangle_list_geometry_shader() {
  using namespace xenon::gpu;
  const auto lowered = make_rectangle_list_geometry_shader();
  assert(lowered.complete && lowered.stage == ShaderStage::Geometry);
  assert(lowered.profile == "gs_6_0");
  assert(lowered.hlsl.find("d01 >= d12") != std::string::npos);
  assert(lowered.hlsl.find("maxvertexcount(4)") != std::string::npos);
  // The emitted strip, as the shader appends it.
  std::vector<char> order;
  for (std::size_t at = lowered.hlsl.find("stream.Append("); at != std::string::npos;
       at = lowered.hlsl.find("stream.Append(", at + 1)) {
    order.push_back(lowered.hlsl[at + std::strlen("stream.Append(")]);
    assert(lowered.hlsl[at + std::strlen("stream.Append(") + 1] == ')');
  }
  // Full-screen rectangle in each vertex rotation (the longest edge lands on
  // every classification branch), plus a non-square, off-center one.
  const std::array<Point2, 3> tl_tr_bl{{{-1, 1}, {1, 1}, {-1, -1}}};
  assert(rectangle_strip_covers(order, tl_tr_bl));
  assert(rectangle_strip_covers(order, {tl_tr_bl[1], tl_tr_bl[2], tl_tr_bl[0]}));
  assert(rectangle_strip_covers(order, {tl_tr_bl[2], tl_tr_bl[0], tl_tr_bl[1]}));
  assert(rectangle_strip_covers(order, {{{0.25, -0.5}, {0.25, 0.75}, {-0.6, -0.5}}}));
  // The previous A,C,B,D strip shares side C-B and leaves a corner wedge.
  assert(!rectangle_strip_covers({'a', 'c', 'b', 'd'}, tl_tr_bl));
  DxcShaderCompiler compiler;
  assert(compiler.compile(lowered, {}).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

void test_sample_transfer_shaders() {
  using namespace xenon::gpu;
  DxcShaderCompiler compiler;
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  for (const auto& shader : {make_transfer_fullscreen_vertex_shader(),
                             make_color_sample_read_shader(MsaaSamples::X2),
                             make_color_sample_read_shader(MsaaSamples::X4),
                             make_color_sample_write_shader()}) {
    assert(shader.complete);
    assert(compiler.compile(shader, {}).succeeded);
    assert(compiler.compile(shader, spirv).succeeded);
  }
  assert(make_color_sample_read_shader(MsaaSamples::X2).hlsl.find(
             "Texture2DMS<float4,2>") != std::string::npos);
  assert(make_color_sample_write_shader().hlsl.find("SV_SampleIndex") !=
         std::string::npos);
  for (const auto samples : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4}) {
    const auto read = make_depth_sample_read_shader(samples);
    assert(read.complete);
    assert(compiler.compile(read, {}).succeeded);
    assert(compiler.compile(read, spirv).succeeded);
  }
  const auto write = make_depth_sample_write_shader();
  assert(write.complete && write.hlsl.find("SV_StencilRef") != std::string::npos);
  assert(compiler.compile(write, {}).succeeded);
  spirv.spirv_stencil_export = true;
  assert(compiler.compile(write, spirv).succeeded);
  for (const auto& fallback : {make_depth_only_sample_write_shader(),
                               make_stencil_mask_write_shader()}) {
    assert(fallback.complete);
    assert(compiler.compile(fallback, {}).succeeded);
    assert(compiler.compile(fallback, spirv).succeeded);
  }
  // Direct EDRAM depth transfers: every pass at every source sample count.
  for (const auto samples : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4}) {
    for (const auto pass : {EdramDepthTransferPass::DepthAndStencil, EdramDepthTransferPass::Depth,
                            EdramDepthTransferPass::StencilBit}) {
      const auto shader = make_edram_depth_transfer_shader(samples, pass);
      assert(shader.complete);
      assert(shader.hlsl.find("SV_SampleIndex") != std::string::npos);
      assert((shader.hlsl.find("SV_StencilRef") != std::string::npos) ==
             (pass == EdramDepthTransferPass::DepthAndStencil));
      assert((shader.hlsl.find("SV_Depth") != std::string::npos) ==
             (pass != EdramDepthTransferPass::StencilBit));
      const auto dxil = compiler.compile(shader, {});
      if (!dxil.succeeded) {
        for (const auto& line : dxil.diagnostics) std::cerr << line << '\n';
        assert(false);
      }
      auto options = spirv;
      options.spirv_stencil_export = pass == EdramDepthTransferPass::DepthAndStencil;
      assert(compiler.compile(shader, options).succeeded);
    }
  }
  // It shares the color transfer's EDRAM mapping verbatim.
  const auto color = make_edram_color_transfer_shader(MsaaSamples::X1).hlsl;
  const auto depth =
      make_edram_depth_transfer_shader(MsaaSamples::X1, EdramDepthTransferPass::Depth).hlsl;
  const auto mapping_at = [](const std::string& hlsl) {
    const auto begin = hlsl.find("  const uint x = uint(position.x);");
    const auto end = hlsl.find(": source_guest);");
    assert(begin != std::string::npos && end != std::string::npos);
    return hlsl.substr(begin, end - begin);
  };
  assert(mapping_at(color) == mapping_at(depth));
}


void test_memexport_translation_targets() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Vertex);
  shader.reflection.memory_exports = 1;
  shader.reflection.memory_export_mask = 1;
  shader.reflection.writes_export_address = true;
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("#define XENON_MEMORY_RW 1") != std::string::npos);
  assert(lowered.hlsl.find("xenon_memexport_flush") != std::string::npos);

  DxcShaderCompiler compiler;
  ShaderCompileOptions dxil{};
  assert(compiler.compile(lowered, dxil).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);

  auto companion = make_shader(ShaderStage::Vertex);
  ShaderLoweringOptions rw_options{};
  rw_options.force_guest_memory_rw = true;
  const auto rw = HlslShaderLowerer::lower(companion, rw_options);
  assert(rw.complete);
  assert(rw.hlsl.find("#define XENON_MEMORY_RW 1") != std::string::npos);
  assert(rw.hlsl.find("xenon_memexport_flush") == std::string::npos);
  assert(compiler.compile(rw, dxil).succeeded);
  assert(compiler.compile(rw, spirv).succeeded);
}


}  // namespace

// Scalar operations read source 3, mulsc/addsc/subsc add a temporary operand,
// and exports can write constant components: all of it must compile for both
// DXIL and SPIR-V.
void test_scalar_source_and_export_constants_compile() {
  using namespace xenon::gpu;
  for (const auto stage : {ShaderStage::Vertex, ShaderStage::Pixel}) {
    auto shader = make_shader(stage);
    auto& alu = shader.instructions[0].alu;
    alu.scalar_opcode = 45;  // addsc
    alu.sources[2].temporary = false;
    alu.sources[2].index = 3;
    alu.sources[2].relative = true;
    alu.constant_address_register_relative = true;
    alu.scalar_constant_temporary = 0;
    alu.vector_write_mask = 0x3;
    alu.scalar_write_mask = 0x6;
    alu.scalar_destination_relative = true;
    const auto lowered = HlslShaderLowerer::lower(shader);
    assert(lowered.complete);
    assert(lowered.hlsl.find("xenon_scalar_op(45, s2, s3t, ps") != std::string::npos);
    DxcShaderCompiler compiler;
    const auto dxil = compiler.compile(lowered, {});
    if (!dxil.succeeded) {
      for (const auto& diagnostic : dxil.diagnostics) std::cerr << diagnostic << '\n';
    }
    assert(dxil.succeeded);
    ShaderCompileOptions spirv{};
    spirv.format = ShaderBinaryFormat::Spirv;
    assert(compiler.compile(lowered, spirv).succeeded);
  }
}
// Regression (AC6's first cutscene ran at about 5 fps): backends build their
// transfer pipelines per depth readback and upload, and each build ran DXC.
// A utility shader is compiled once per process; later requests are served
// from memory with the same result, and distinct inputs never share one.
void test_utility_shaders_compile_once() {
  using namespace xenon::gpu;
  UtilityShaderCompiler compiler;
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  const auto vertex = make_transfer_fullscreen_vertex_shader();
  const auto first = compiler.compile(vertex);
  const auto after_first = UtilityShaderCompiler::compiles();
  assert(first.succeeded);
  for (int i = 0; i < 8; ++i) {
    const auto again = compiler.compile(vertex);
    assert(again.succeeded && again.binary == first.binary);
  }
  assert(UtilityShaderCompiler::compiles() == after_first);

  // Same shader, other target: a separate entry.
  const auto spirv_vertex = compiler.compile(vertex, spirv);
  assert(spirv_vertex.succeeded && spirv_vertex.format == ShaderBinaryFormat::Spirv);
  assert(UtilityShaderCompiler::compiles() == after_first + 1u);
  // Other shaders: separate entries with their own binaries.
  const auto read2 = compiler.compile(make_depth_sample_read_shader(MsaaSamples::X2));
  const auto read4 = compiler.compile(make_depth_sample_read_shader(MsaaSamples::X4));
  assert(read2.succeeded && read4.succeeded && read2.binary != read4.binary);
  assert(UtilityShaderCompiler::compiles() == after_first + 3u);
  assert(compiler.compile(make_depth_sample_read_shader(MsaaSamples::X2)).binary ==
         read2.binary);
  assert(UtilityShaderCompiler::compiles() == after_first + 3u);

  // An incomplete shader is refused the same way every time.
  LoweredShader incomplete = vertex;
  incomplete.complete = false;
  assert(!compiler.compile(incomplete).succeeded);
  assert(!compiler.compile(incomplete).succeeded);
}

int main() {
  test_lowering_is_deterministic();
  test_dxc_dual_target_and_cache();
  test_pixel_target_and_incomplete_rejection();
  test_control_flow_state_machine_and_explicit_rejection();
  test_texture_lod_and_gradient_lowering();
  test_vertex_fetch_select_endian_and_packed_formats();
  test_vertex_index_applies_grouper_offset_and_clamp();
  test_texture_state_and_query_opcodes();
  test_vertex_shader_texture_fetch_uses_explicit_lod();
  test_rectangle_list_geometry_shader();
  test_sample_transfer_shaders();
  test_utility_shaders_compile_once();
  test_memexport_translation_targets();
  test_scalar_source_and_export_constants_compile();
  std::cout << "xenon_shader_translation_tests: ok\n";
}
