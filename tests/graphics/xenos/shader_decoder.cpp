#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/gpu/shader_ir.hpp"

namespace {
std::array<std::uint32_t, 3> pack_cf(std::uint32_t a0, std::uint16_t a1,
                                     std::uint32_t b0 = 0,
                                     std::uint16_t b1 = 0) {
  return {a0, std::uint32_t(a1) | ((b0 & 0xFFFFu) << 16),
          (b0 >> 16) | (std::uint32_t(b1) << 16)};
}

void test_mixed_exec_and_reflection() {
  using namespace xenon::gpu;
  const auto cf = pack_cf(1u | (2u << 12) | (1u << 16),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));
  std::uint32_t vf0 = (3u << 5) | (5u << 12) | (7u << 20) | (1u << 25);
  std::uint32_t vf1 = 0x688u | (12u << 16);
  std::uint32_t vf2 = 4u | (8u << 8);
  std::uint32_t alu0 = 2u | (1u << 15) | (0xFu << 16) | (2u << 26);
  std::uint32_t alu2 = 9u | (4u << 8) | (1u << 16) | (3u << 24);
  const std::vector<std::uint32_t> words = {
      cf[0], cf[1], cf[2], vf0, vf1, vf2, alu0, 0, alu2};
  ShaderProgram program(ShaderStage::Vertex, words, 11);
  const auto decoded = ShaderDecoder::decode(program);
  assert(decoded.complete && decoded.source_hash == program.hash());
  assert(decoded.start_slot == 11 && decoded.instructions.size() == 2);
  assert(decoded.instructions[0].kind == ShaderInstructionKind::VertexFetch);
  assert(decoded.instructions[0].vertex_fetch.fetch_constant == 7);
  assert(decoded.instructions[0].vertex_fetch.stride_dwords == 4);
  assert(decoded.instructions[1].kind == ShaderInstructionKind::Alu);
  assert(decoded.instructions[1].alu.export_data);
  assert(decoded.reflection.vertex_fetch_constants == std::vector<std::uint8_t>{7});
  assert(decoded.reflection.vertex_fetches == std::vector<std::uint8_t>{7 * 3 + 1});
  assert((decoded.reflection.float_constants == std::vector<std::uint16_t>{1, 4, 9}));
  assert(decoded.reflection.exports == std::vector<std::uint8_t>{2});
  assert(decoded.reflection.interpolator_exports == 1);
}

void test_texture_predicate_and_loop() {
  using namespace xenon::gpu;
  const auto cf = pack_cf(2u | (6u << 16),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::LoopStart) << 12),
      1u | (1u << 12) | (1u << 16),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));
  const std::uint32_t t0 = 1u | (2u << 5) | (3u << 12) | (11u << 20) | (0x24u << 26);
  const std::uint32_t t1 = 0x688u | (1u << 28) | (1u << 31);
  const std::uint32_t t2 = 1u | (3u << 14) | (31u << 16) | (1u << 31);
  const std::vector<std::uint32_t> words = {cf[0], cf[1], cf[2], t0, t1, t2};
  const auto decoded = ShaderDecoder::decode(ShaderProgram(ShaderStage::Pixel, words));
  assert(decoded.complete && decoded.reflection.uses_loops);
  assert(decoded.reflection.uses_predication);
  assert(decoded.reflection.loop_constants == std::vector<std::uint8_t>{6});
  assert(decoded.reflection.texture_fetch_constants == std::vector<std::uint8_t>{11});
  assert(decoded.instructions[0].texture_fetch.dimension == 3);
  assert(decoded.instructions[0].texture_fetch.offsets_half_texels[0] == -1);
}


void test_pixel_depth_export_reflection() {
  using namespace xenon::gpu;
  const auto cf = pack_cf(1u | (1u << 12),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));
  // PS export e61. Scalar destination deliberately differs: scalar export also
  // targets vector_destination on Xenos.
  const std::uint32_t alu0 = 61u | (7u << 8) | (1u << 15) |
                             (1u << 16) | (2u << 20);
  const std::uint32_t alu2 = 0u;
  const std::vector<std::uint32_t> words = {
      cf[0], cf[1], cf[2], alu0, 0u, alu2};
  const auto decoded = ShaderDecoder::decode(
      ShaderProgram(ShaderStage::Pixel, words));
  assert(decoded.complete);
  assert(decoded.reflection.writes_depth);
  assert(decoded.reflection.exports == std::vector<std::uint8_t>{61});
  assert(decoded.reflection.color_exports == 0);
  assert(decoded.reflection.memory_exports == 0);
}


void test_memexport_reflection_and_stream_constant() {
  using namespace xenon::gpu;
  const auto cf = pack_cf(1u | (2u << 12),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));

  // mad eA, r3, c5, c7 - c7 is the conventional stream constant source.
  const std::uint32_t ea0 = 32u | (1u << 15) | (0xFu << 16) | (50u << 26);
  const std::uint32_t ea1 = 0u;  // Relative swizzle fields of 0 are .xyzw.
  const std::uint32_t ea2 = 7u | (5u << 8) | (3u << 16) |
                            (11u << 24) | (1u << 31);

  // eM2 write. Only the export register identity matters to reflection here.
  const std::uint32_t em0 = 35u | (1u << 15) | (0xFu << 16) | (50u << 26);
  const std::uint32_t em1 = 0u;
  const std::uint32_t em2 = 1u | (1u << 8) | (1u << 16) |
                            (1u << 29) | (1u << 30) | (1u << 31);

  const std::vector<std::uint32_t> words = {
      cf[0], cf[1], cf[2], ea0, ea1, ea2, em0, em1, em2};
  const auto decoded = ShaderDecoder::decode(
      ShaderProgram(ShaderStage::Vertex, words));
  assert(decoded.complete);
  assert(decoded.reflection.writes_export_address);
  assert(decoded.reflection.memory_exports == 1);
  assert(decoded.reflection.memory_export_mask == (1u << 2));
  assert(decoded.reflection.memexport_stream_constants ==
         std::vector<std::uint16_t>{7});
  assert(!decoded.reflection.requires_dynamic_memexport_address);
  assert((decoded.reflection.exports ==
          std::vector<std::uint8_t>{32, 35}));
}

void test_noncanonical_memexport_address_stays_dynamic() {
  using namespace xenon::gpu;
  const auto cf = pack_cf(1u | (2u << 12),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));

  // A partial/clamped MAD to eA must not be treated as the conventional
  // statically recoverable stream-address pattern.
  const std::uint32_t ea0 = 32u | (1u << 15) | (0x7u << 16) |
                            (1u << 24) | (50u << 26);
  const std::uint32_t ea1 = 0u;  // Relative swizzle fields of 0 are .xyzw.
  const std::uint32_t ea2 = 7u | (5u << 8) | (3u << 16) |
                            (11u << 24) | (1u << 31);
  const std::uint32_t em0 = 33u | (1u << 15) | (0xFu << 16) | (50u << 26);
  const std::uint32_t em1 = 0u;
  const std::uint32_t em2 = 1u | (1u << 8) | (1u << 16) |
                            (1u << 29) | (1u << 30) | (1u << 31);

  const std::vector<std::uint32_t> words = {
      cf[0], cf[1], cf[2], ea0, ea1, ea2, em0, em1, em2};
  const auto decoded = ShaderDecoder::decode(
      ShaderProgram(ShaderStage::Vertex, words));
  assert(decoded.complete);
  assert(decoded.reflection.writes_export_address);
  assert(decoded.reflection.memory_export_mask == 1u);
  assert(decoded.reflection.memexport_stream_constants.empty());
  assert(decoded.reflection.requires_dynamic_memexport_address);
}

void test_malformed_stream_is_diagnostic() {
  using namespace xenon::gpu;
  const std::uint32_t words[] = {1, 2};
  const auto decoded = ShaderDecoder::decode(ShaderProgram(ShaderStage::Vertex, words));
  assert(!decoded.complete && !decoded.diagnostics.empty());
}

// One exec of ALU instructions, each given as its three raw words.
xenon::gpu::DecodedShader decode_alu_exec(
    const std::vector<std::array<std::uint32_t, 3>>& alus) {
  using namespace xenon::gpu;
  const auto cf = pack_cf(1u | (std::uint32_t(alus.size()) << 12),
      std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));
  std::vector<std::uint32_t> words = {cf[0], cf[1], cf[2]};
  for (const auto& alu : alus) words.insert(words.end(), alu.begin(), alu.end());
  const auto decoded = ShaderDecoder::decode(ShaderProgram(ShaderStage::Vertex, words));
  assert(decoded.complete && decoded.instructions.size() == alus.size());
  return decoded;
}

// ALU swizzles in microcode are component-relative. These are the AC6 boot
// logo vertex shader's position and frcs instructions as they appear in the
// microcode: dp2add e62.x, r0.yz, c40.xy, c40.w and frcs r0.x, r0.x.
void test_relative_alu_swizzles() {
  using namespace xenon::gpu;
  const std::array<std::uint32_t, 3> position = {
      62u | (1u << 15) | (1u << 16) | (50u << 26),
      0x1Bu | (0xB0u << 8) | (0xC5u << 16),
      40u | (40u << 8) | (0u << 16) | (17u << 24) | (1u << 31)};
  const std::array<std::uint32_t, 3> fraction = {
      (1u << 20) | (11u << 26), 0x6Cu, (2u << 24) | (7u << 29)};
  const auto decoded = decode_alu_exec({position, fraction});
  const auto& dp2add = decoded.instructions[0].alu;
  assert(dp2add.sources[0].temporary && dp2add.sources[0].swizzle == 0xA9);  // .yzzz
  assert(!dp2add.sources[1].temporary && dp2add.sources[1].swizzle == 0x54);  // .xyyy
  assert(!dp2add.sources[2].temporary && dp2add.sources[2].swizzle == 0xFF);  // .wwww
  const auto& frcs = decoded.instructions[1].alu;
  assert(frcs.sources[0].swizzle == 0xE4 && frcs.sources[1].swizzle == 0xE4);
  assert(frcs.sources[2].swizzle == 0x00);  // Scalar `a` (W) is r0.x.
}

// abs_constants applies to every constant operand; a constant's relative flag
// is const_0_rel_abs for the first constant and const_1_rel_abs for later ones.
void test_constant_absolute_and_relative_addressing() {
  using namespace xenon::gpu;
  // mad r1, |c5[a0]|, |r2|, |c7|: c5 is the first constant, c7 the second.
  const std::array<std::uint32_t, 3> mad = {
      1u | (1u << 7) | (0xFu << 16) | (50u << 26), (1u << 29) | (1u << 31),
      7u | ((0x80u | 2u) << 8) | (5u << 16) | (11u << 24) | (1u << 30)};
  // mad r1, r1, r2, c7[aL]: c7 is the first constant.
  const std::array<std::uint32_t, 3> first_in_source3 = {
      1u | (0xFu << 16) | (50u << 26), 1u << 31,
      7u | (2u << 8) | (1u << 16) | (11u << 24) | (1u << 30) | (1u << 31)};
  const auto decoded = decode_alu_exec({mad, first_in_source3});
  const auto& a = decoded.instructions[0].alu;
  assert(a.absolute_constants && a.constant_address_register_relative);
  assert(!a.sources[0].temporary && a.sources[0].index == 5);
  assert(a.sources[0].relative && a.sources[0].absolute_value);
  assert(a.sources[1].temporary && a.sources[1].index == 2);
  assert(!a.sources[1].relative && a.sources[1].absolute_value);
  assert(!a.sources[2].temporary && a.sources[2].index == 7);
  assert(!a.sources[2].relative && a.sources[2].absolute_value);
  const auto& b = decoded.instructions[1].alu;
  assert(!b.sources[2].temporary && b.sources[2].relative);
  assert(!b.sources[2].absolute_value && !b.constant_address_register_relative);
}

// mulsc/addsc/subsc: source 3 is always a constant; the temporary index is
// assembled from the opcode's low bit, the source 3 select bit and swizzle
// bits 2-5.
void test_scalar_constant_operation_operands() {
  using namespace xenon::gpu;
  // mulsc r4.x, c9.w, r23.x (MUL_CONST_1).
  const std::array<std::uint32_t, 3> mulsc = {
      (4u << 8) | (1u << 20) | (43u << 26), 0x14u,
      9u | (2u << 24) | (1u << 29) | (1u << 30) | (1u << 31)};
  const auto decoded = decode_alu_exec({mulsc});
  const auto& alu = decoded.instructions[0].alu;
  assert(!alu.sources[2].temporary && alu.sources[2].index == 9);
  assert(alu.scalar_constant_temporary == 23);
  assert((alu.sources[2].swizzle >> 6) == 3 && (alu.sources[2].swizzle & 3) == 0);
  assert(decoded.reflection.temporary_register_count >= 24);
  assert(decoded.reflection.float_constants == std::vector<std::uint16_t>{9});
}

// Raw 0xE4 swizzle fields are .xzxz, not the identity, so a stream address
// built with them is not the conventional statically recoverable pattern.
void test_memexport_raw_e4_swizzle_is_not_identity() {
  using namespace xenon::gpu;
  const std::array<std::uint32_t, 3> address = {
      32u | (1u << 15) | (0xFu << 16) | (50u << 26),
      0xE4u | (0xE4u << 8) | (0xE4u << 16),
      7u | (5u << 8) | (3u << 16) | (11u << 24) | (1u << 31)};
  const auto decoded = decode_alu_exec({address});
  assert(decoded.instructions[0].alu.sources[2].swizzle == 0x88);  // .xzxz
  assert(decoded.reflection.memexport_stream_constants.empty());
  assert(decoded.reflection.requires_dynamic_memexport_address);
}
}  // namespace

int main() {
  test_mixed_exec_and_reflection();
  test_texture_predicate_and_loop();
  test_pixel_depth_export_reflection();
  test_memexport_reflection_and_stream_constant();
  test_noncanonical_memexport_address_stays_dynamic();
  test_malformed_stream_is_diagnostic();
  test_relative_alu_swizzles();
  test_constant_absolute_and_relative_addressing();
  test_scalar_constant_operation_operands();
  test_memexport_raw_e4_swizzle_is_not_identity();
  std::cout << "xenon_shader_decoder_tests: ok\n";
}
