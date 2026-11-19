// VertexPositionEvaluator: the CPU twin of a lowered Xenos vertex shader that
// bounds unclipped draws (xenon/gpu/vertex_position_evaluator.hpp).

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iostream>
#include <vector>

#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/gpu/vertex_position_evaluator.hpp"

namespace {

using namespace xenon::gpu;

constexpr std::uint32_t kVertexData = 0x1000;

// The draw's constant buffer, laid out as ResourceStateTracker writes it.
struct Constants {
  std::array<std::byte, kShaderConstantBufferBytes> bytes{};
  void word(std::size_t offset, std::uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); }
  void float_constant(std::uint32_t index, std::array<float, 4> value) {
    std::memcpy(bytes.data() + std::size_t(index) * 16u, value.data(), 16);
  }
  void loop_constant(std::uint32_t index, std::uint32_t count, std::uint32_t start, std::int32_t step) {
    word(8320 + std::size_t(index) * 16u,
         count | (start << 8u) | ((static_cast<std::uint32_t>(step) & 0xFFu) << 16u));
  }
  void bool_constant(std::uint32_t index, bool value) {
    const auto offset = 8192 + ((index >> 7u) & 7u) * 16u + ((index >> 5u) & 3u) * 4u;
    std::uint32_t w{};
    std::memcpy(&w, bytes.data() + offset, 4);
    w = value ? (w | (1u << (index & 31u))) : (w & ~(1u << (index & 31u)));
    word(offset, w);
  }
  // Vertex fetch constant `slot`, select `select`: byte address | endian.
  void vertex_fetch(std::uint32_t slot, std::uint32_t select, std::uint32_t address, std::uint32_t endian) {
    word(8832 + std::size_t(slot) * 16u + (select == 0 ? 0u : select == 1 ? 8u : 12u), address | endian);
  }
  void draw_state(std::size_t index, std::uint32_t value) { word(kDrawStateOffset + index * 4u, value); }
  void draw_state_float(std::size_t index, float value) { draw_state(index, std::bit_cast<std::uint32_t>(value)); }

  Constants() {
    // VGT_MAX_VTX_INDX open, as Direct3D leaves it; identity viewport; W is W.
    draw_state(7, 0xFFFFFFu);
    draw_state_float(8, 1.0f);
    draw_state_float(9, 1.0f);
    draw_state(12, 4u);
  }
};

struct Memory {
  std::vector<std::byte> bytes = std::vector<std::byte>(0x10000);
  GuestPhysicalReader reader() {
    return [this](std::uint32_t address, std::span<std::byte> out) {
      if (std::uint64_t(address) + out.size() > bytes.size()) return false;
      std::memcpy(out.data(), bytes.data() + address, out.size());
      return true;
    };
  }
  void little(std::uint32_t address, std::uint32_t value) { std::memcpy(bytes.data() + address, &value, 4); }
  void big(std::uint32_t address, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[address + i] = std::byte(value >> (24u - i * 8u));
  }
};

AluSource temp(std::uint32_t index, std::uint8_t swizzle = 0xE4) {
  AluSource s{};
  s.index = index;
  s.temporary = true;
  s.swizzle = swizzle;
  return s;
}
AluSource constant(std::uint32_t index, std::uint8_t swizzle = 0xE4) {
  AluSource s{};
  s.index = index;
  s.swizzle = swizzle;
  return s;
}

DecodedInstruction alu(std::uint32_t address, std::uint8_t vector_opcode, std::uint8_t destination,
                       std::uint8_t mask, bool export_data, AluSource a, AluSource b, AluSource c,
                       std::uint8_t scalar_opcode = 50, std::uint8_t scalar_mask = 0) {
  DecodedInstruction instruction{};
  instruction.kind = ShaderInstructionKind::Alu;
  auto& out = instruction.alu;
  out.address = address;
  out.vector_opcode = vector_opcode;
  out.scalar_opcode = scalar_opcode;
  out.vector_destination = destination;
  out.scalar_destination = destination;
  out.vector_write_mask = mask;
  out.scalar_write_mask = scalar_mask;
  out.export_data = export_data;
  out.sources = {a, b, c};
  return instruction;
}

DecodedInstruction vfetch(std::uint32_t address, std::uint8_t destination, std::uint8_t format,
                          std::uint8_t stride, bool is_signed = false, bool normalized = false,
                          std::int32_t offset = 0, bool mini = false) {
  DecodedInstruction instruction{};
  instruction.kind = ShaderInstructionKind::VertexFetch;
  auto& out = instruction.vertex_fetch;
  out.address = address;
  out.source_register = 0;
  out.source_swizzle = 0;
  out.destination_register = destination;
  out.destination_swizzle = 0x688;  // .xyzw
  out.fetch_constant = 0;
  out.data_format = format;
  out.stride_dwords = stride;
  out.offset_dwords = offset;
  out.signed_data = is_signed;
  out.normalized = normalized;
  out.mini_fetch = mini;
  return instruction;
}

DecodedControlFlow exec(std::uint32_t index, ControlFlowOpcode opcode, std::uint32_t target,
                        std::uint32_t count) {
  DecodedControlFlow cf{};
  cf.index = index;
  cf.opcode = opcode;
  cf.target = target;
  cf.count = count;
  return cf;
}

DecodedShader shader(std::vector<DecodedControlFlow> control_flow,
                     std::vector<DecodedInstruction> instructions) {
  DecodedShader result{};
  result.stage = ShaderStage::Vertex;
  result.complete = true;
  result.control_flow = std::move(control_flow);
  result.instructions = std::move(instructions);
  result.reflection.temporary_register_count = 8;
  return result;
}

// vfetch r1 (float4 at fetch constant 0, 4 dwords a vertex); e62 = max(r1, r1).
DecodedShader passthrough(std::uint8_t format = 38) {
  return shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 2)},
                {vfetch(0, 1, format, 4), alu(1, 2, 62, 0xF, true, temp(1), temp(1), temp(1))});
}

void write_vertices(Memory& memory, const std::vector<std::array<float, 4>>& vertices, bool big) {
  for (std::size_t v = 0; v < vertices.size(); ++v)
    for (unsigned c = 0; c < 4; ++c) {
      const auto address = static_cast<std::uint32_t>(kVertexData + (v * 4u + c) * 4u);
      const auto bits = std::bit_cast<std::uint32_t>(vertices[v][c]);
      big ? memory.big(address, bits) : memory.little(address, bits);
    }
}

// A screen-space rectangle's vertices come back as the pixels they name, and
// the D3D half-pixel and window offsets in XenonDrawState are applied.
void test_passthrough_reaches_its_pixels() {
  Memory memory;
  write_vertices(memory, {{0, 0, 0, 1}, {208, 0, 0, 1}, {0, 144, 0, 1}}, false);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  const auto vs = passthrough();
  assert(VertexPositionEvaluator::unsupported_reason(vs).empty());
  const auto v2 = VertexPositionEvaluator::evaluate(vs, constants.bytes, 2, memory.reader());
  assert(v2.valid && v2.pixel[0] == 0.0f && v2.pixel[1] == 144.0f);
  const auto v1 = VertexPositionEvaluator::evaluate(vs, constants.bytes, 1, memory.reader());
  assert(v1.valid && v1.pixel[0] == 208.0f && v1.pixel[1] == 0.0f);
  constants.draw_state_float(10, 0.5f);
  constants.draw_state_float(11, 16.5f);
  const auto offset = VertexPositionEvaluator::evaluate(vs, constants.bytes, 2, memory.reader());
  assert(offset.valid && offset.pixel[0] == 0.5f && offset.pixel[1] == 160.5f);
}

// Big-endian float2 data through an 8-in-32 fetch constant.
void test_endian_and_float2() {
  Memory memory;
  write_vertices(memory, {{3, 0, 0, 0}, {12.5f, 99.25f, 0, 0}}, true);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 2);
  const auto vs = passthrough(37);
  const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 1, memory.reader());
  assert(v.valid && v.position == (std::array<float, 4>{12.5f, 99.25f, 0.0f, 1.0f}));
}

// e62 = r1 * c0 + c1 (mad, Xenos legacy multiply).
void test_constant_transform() {
  Memory memory;
  write_vertices(memory, {{4, 5, 0, 1}}, false);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  constants.float_constant(0, {2, 3, 1, 1});
  constants.float_constant(1, {1, 10, 0, 0});
  const auto vs = shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 2)},
                         {vfetch(0, 1, 38, 4), alu(1, 11, 62, 0xF, true, temp(1), constant(0), constant(1))});
  const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
  assert(v.valid && v.pixel[0] == 9.0f && v.pixel[1] == 25.0f);
}

// Regression (AC6 NaN blocks and exposure collapse): DP4, DP3, DP2ADD and
// DST multiply as Xenos does, 0 * anything = 0 with infinity included, as the
// translated shaders now do.
void test_dot_products_follow_zero_rule() {
  Memory memory;
  write_vertices(memory, {{0, 0, 0, 1}}, false);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  constexpr float inf = std::numeric_limits<float>::infinity();
  constants.float_constant(0, {inf, 0.5f, 0.25f, 0.25f});
  constants.float_constant(1, {0, 1, 1, 1});
  constants.float_constant(2, {0.25f, 0.5f, 0, 1});
  constants.float_constant(3, {0, inf, 0, 0});
  constants.float_constant(4, {0, 0, 0, 0});
  const auto vs = shader(
      {exec(0, ControlFlowOpcode::ExecEnd, 0, 7)},
      {vfetch(0, 1, 38, 4), alu(1, 15, 2, 0x1, false, constant(0), constant(1), constant(0)),
       alu(2, 16, 2, 0x2, false, constant(0), constant(1), constant(0)),
       alu(3, 17, 2, 0x4, false, constant(0), constant(1), constant(2)),
       alu(4, 28, 3, 0x2, false, constant(3), constant(4), constant(3)),
       alu(5, 11, 2, 0x8, false, temp(3, 0x55), constant(2, 0xFF), constant(2, 0x55)),
       alu(6, 2, 62, 0xF, true, temp(2), temp(2), temp(2))});
  const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
  assert(v.valid && v.position == (std::array<float, 4>{1.0f, 0.75f, 0.75f, 0.5f}));
}

// r0.x is the index after VGT_INDX_OFFSET and the min/max clamp - HLSL's
// clamp(), min(max(x, lo), hi), including a minimum above the maximum.
void test_index_offset_and_clamp() {
  Memory memory;
  write_vertices(memory, {{0, 10, 0, 1}, {0, 20, 0, 1}, {0, 30, 0, 1}}, false);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  const auto vs = passthrough();
  constants.draw_state(5, 1);
  assert(VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader()).pixel[1] == 20.0f);
  constants.draw_state(5, 0);
  constants.draw_state(6, 0);
  constants.draw_state(7, 1);
  assert(VertexPositionEvaluator::evaluate(vs, constants.bytes, 2, memory.reader()).pixel[1] == 20.0f);
  constants.draw_state(6, 2);
  constants.draw_state(7, 1);
  assert(VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader()).pixel[1] == 20.0f);
}

// Packed formats decode as xenon_vertex_decode does.
void test_packed_formats() {
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  {
    Memory memory;  // 8_8_8_8 unsigned normalized: 0x40 -> 64/255
    memory.little(kVertexData, 0xFF80'4000u);
    const auto vs = shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 2)},
                           {vfetch(0, 1, 6, 1, false, true), alu(1, 2, 62, 0xF, true, temp(1), temp(1), temp(1))});
    const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
    assert(v.position[0] == 0.0f && v.position[1] == 64.0f / 255.0f &&
           v.position[2] == 128.0f / 255.0f && v.position[3] == 1.0f);
  }
  {
    Memory memory;  // 16_16_16_16 signed normalized; -32768 clamps to -1.
    memory.little(kVertexData, 0x8000'4000u);
    memory.little(kVertexData + 4, 0x7FFF'0000u);
    const auto vs = shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 2)},
                           {vfetch(0, 1, 26, 2, true, true), alu(1, 2, 62, 0xF, true, temp(1), temp(1), temp(1))});
    const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
    assert(v.position[0] == 16384.0f / 32767.0f && v.position[1] == -1.0f && v.position[2] == 0.0f &&
           v.position[3] == 1.0f);
  }
  {
    Memory memory;  // 16_16_16_16_FLOAT: 1.0, -2.0, 0.5, 1.0
    memory.little(kVertexData, 0xC000'3C00u);
    memory.little(kVertexData + 4, 0x3C00'3800u);
    const auto vs = shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 2)},
                           {vfetch(0, 1, 32, 2), alu(1, 2, 62, 0xF, true, temp(1), temp(1), temp(1))});
    const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
    assert(v.position == (std::array<float, 4>{1.0f, -2.0f, 0.5f, 1.0f}));
  }
}

// A loop over aL-relative constants: r2 += c[aL] three times, then e62.xyz
// = r2 with W 1.0 (both write masks select W on an export).
void test_loop_and_relative_constants() {
  Memory memory;
  Constants constants;
  constants.loop_constant(0, 3, 0, 1);
  constants.float_constant(0, {1, 1, 0, 0});
  constants.float_constant(1, {2, 2, 0, 0});
  constants.float_constant(2, {4, 4, 0, 0});
  auto relative = constant(0);
  relative.relative = true;
  DecodedControlFlow start{};
  start.index = 0;
  start.opcode = ControlFlowOpcode::LoopStart;
  start.target = 3;
  start.loop_constant = 0;
  DecodedControlFlow end{};
  end.index = 2;
  end.opcode = ControlFlowOpcode::LoopEnd;
  end.target = 1;
  auto add = alu(0, 0, 2, 0xF, false, temp(2), relative, temp(2));
  auto export_position = alu(1, 2, 62, 0xF, true, temp(2), temp(2), temp(2), 50, 0x8);
  const auto vs = shader({start, exec(1, ControlFlowOpcode::Exec, 0, 1), end,
                          exec(3, ControlFlowOpcode::ExecEnd, 1, 1)},
                         {add, export_position});
  const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
  assert(v.valid && v.position == (std::array<float, 4>{7.0f, 7.0f, 0.0f, 1.0f}));
}

// setp_eq sets the predicate; a predicated exec and a predicated ALU follow
// it, and a bool-constant conditional exec is skipped.
void test_predicates_and_bool_constants() {
  Memory memory;
  Constants constants;
  constants.float_constant(0, {0, 0, 0, 0});
  constants.float_constant(1, {5, 50, 0, 1});
  constants.float_constant(2, {9, 90, 0, 1});
  constants.bool_constant(3, false);
  // ALU 0: scalar setp_eq (27) on c0.w == 0 -> p0 true (no write).
  auto setp = alu(0, 2, 5, 0x0, false, constant(0), constant(0), constant(0), 27, 0x0);
  // ALU 1, predicated on p0: e62 = c1.
  auto taken = alu(1, 2, 62, 0xF, true, constant(1), constant(1), constant(1));
  taken.alu.predicate = {true, true, false};
  // ALU 2 in an exec that runs only when bool constant 3 is set: e62 = c2.
  auto skipped = alu(2, 2, 62, 0xF, true, constant(2), constant(2), constant(2));
  DecodedControlFlow conditional = exec(2, ControlFlowOpcode::CondExecEnd, 2, 1);
  conditional.bool_constant = 3;
  conditional.condition = true;
  DecodedControlFlow predicated = exec(1, ControlFlowOpcode::CondExecPred, 1, 1);
  predicated.predicate = {true, true, false};
  const auto vs = shader({exec(0, ControlFlowOpcode::Exec, 0, 1), predicated, conditional},
                         {setp, taken, skipped});
  const auto v = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
  assert(v.valid && v.pixel[0] == 5.0f && v.pixel[1] == 50.0f);
  constants.bool_constant(3, true);
  const auto w = VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader());
  assert(w.valid && w.pixel[0] == 9.0f && w.pixel[1] == 90.0f);
}

// PA_CL_VTE_CNTL formats: without VTX_W0_FMT the exported W is 1/W0, and
// VTX_XY_FMT means XY were already divided by W0.
void test_vte_formats() {
  Memory memory;
  write_vertices(memory, {{8, 10, 0, 0.5f}}, false);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  const auto vs = passthrough();
  constants.draw_state(12, 0u);  // W = 1/0.5 = 2: pixel = (10 * 1) / 2
  assert(VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader()).pixel[1] == 5.0f);
  constants.draw_state(12, 1u);  // XY *= W first: (20) / 2
  assert(VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader()).pixel[1] == 10.0f);
  constants.draw_state(12, 4u);  // W as exported: 10 / 0.5
  assert(VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader()).pixel[1] == 20.0f);
}

// What cannot be run on the CPU is reported, never guessed: a texture fetch,
// a vertex outside guest memory, a position without a usable W.
void test_unsupported_is_reported() {
  Memory memory;
  Constants constants;
  DecodedInstruction tfetch{};
  tfetch.kind = ShaderInstructionKind::TextureFetch;
  tfetch.texture_fetch.opcode = 1;
  auto vs = shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 1)}, {tfetch});
  assert(!VertexPositionEvaluator::unsupported_reason(vs).empty());
  assert(!VertexPositionEvaluator::evaluate(vs, constants.bytes, 0, memory.reader()).valid);

  constants.vertex_fetch(0, 0, 0x00F00000u, 0);
  const auto outside = VertexPositionEvaluator::evaluate(passthrough(), constants.bytes, 0, memory.reader());
  assert(!outside.valid && !outside.error.empty());

  const auto no_w = shader({exec(0, ControlFlowOpcode::ExecEnd, 0, 1)},
                           {alu(0, 2, 62, 0x7, true, constant(0), constant(0), constant(0))});
  assert(!VertexPositionEvaluator::evaluate(no_w, constants.bytes, 0, memory.reader()).valid);
}

// Decoded microcode end to end: the AC6 boot logo vertex shader's position
// instruction, dp2add e62.x, r0.yz, c40.xy, c40.w (shader_decoder tests).
void test_decoded_microcode() {
  const std::array<std::uint32_t, 3> position = {
      62u | (1u << 15) | (1u << 16) | (50u << 26), 0x1Bu | (0xB0u << 8) | (0xC5u << 16),
      40u | (40u << 8) | (0u << 16) | (17u << 24) | (1u << 31)};
  const std::uint32_t cf0 = 1u | (1u << 12);
  const std::uint32_t cf1 = std::uint32_t(ControlFlowOpcode::ExecEnd) << 12;
  const std::vector<std::uint32_t> words = {cf0, cf1, 0u, position[0], position[1], position[2]};
  const auto decoded = ShaderDecoder::decode(ShaderProgram(ShaderStage::Vertex, words));
  assert(decoded.complete);
  Memory memory;
  Constants constants;
  constants.float_constant(40, {2, 3, 0, 7});
  const auto v = VertexPositionEvaluator::evaluate(decoded, constants.bytes, 5, memory.reader());
  // r0 = (5, 0, 0, 0): 0 * 2 + 0 * 3 + 7. W stays 0, so there is no pixel.
  assert(v.position[0] == 7.0f && !v.valid);
}

// A plan built once and reused - the renderer's path - evaluates every vertex
// exactly as a one-shot evaluation does, and carries the support verdict.
void test_plan_reuse_matches_one_shot() {
  Memory memory;
  write_vertices(memory, {{0, 0, 0, 1}, {208, 0, 0, 1}, {0, 144, 0, 1}, {64, 32, 0, 1}}, false);
  Constants constants;
  constants.vertex_fetch(0, 0, kVertexData, 0);
  const auto vs = passthrough();
  const VertexEvaluatorPlan plan(vs);
  assert(plan.supported() && plan.unsupported_reason().empty());
  for (int pass = 0; pass < 3; ++pass) {
    for (std::uint32_t vertex = 0; vertex < 4; ++vertex) {
      const auto once = VertexPositionEvaluator::evaluate(vs, constants.bytes, vertex, memory.reader());
      const auto planned =
          VertexPositionEvaluator::evaluate(plan, constants.bytes, vertex, memory.reader());
      assert(once.valid == planned.valid);
      assert(once.position == planned.position && once.pixel == planned.pixel);
    }
    constants.draw_state_float(11, 2.0f * float(pass + 1));  // the constants change between draws
  }
  // An unsupported shader's plan says why, and evaluation reports it.
  auto sampled = passthrough();
  DecodedInstruction fetch{};
  fetch.kind = ShaderInstructionKind::TextureFetch;
  fetch.texture_fetch.opcode = 1u;  // tfetch: no CPU evaluation
  sampled.instructions.push_back(fetch);
  const VertexEvaluatorPlan unsupported(sampled);
  assert(!unsupported.supported() && !unsupported.unsupported_reason().empty());
  const auto refused = VertexPositionEvaluator::evaluate(unsupported, constants.bytes, 0, memory.reader());
  assert(!refused.valid && refused.error == unsupported.unsupported_reason());
}

}  // namespace

int main() {
  test_plan_reuse_matches_one_shot();
  test_passthrough_reaches_its_pixels();
  test_endian_and_float2();
  test_constant_transform();
  test_dot_products_follow_zero_rule();
  test_index_offset_and_clamp();
  test_packed_formats();
  test_loop_and_relative_constants();
  test_predicates_and_bool_constants();
  test_vte_formats();
  test_unsupported_is_reported();
  test_decoded_microcode();
  std::cout << "xenon_vertex_position_evaluator_tests: ok\n";
  return 0;
}
