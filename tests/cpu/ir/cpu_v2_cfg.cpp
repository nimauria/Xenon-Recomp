#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

namespace {

bool has_edge(const ir::Block& block, GuestAddress target, ir::EdgeKind kind,
              bool local) {
  for (const auto& edge : block.successors) {
    if (edge.target == target && edge.kind == kind && edge.local == local)
      return true;
  }
  return false;
}

bool has_predecessor(const ir::Block& block, GuestAddress address) {
  for (const auto predecessor : block.predecessors)
    if (predecessor == address) return true;
  return false;
}

std::size_t count_text(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos;
       pos += needle.size())
    ++count;
  return count;
}

}  // namespace

int main() {
  StaticFunctionCompiler compiler;

  // Acceptance rule: 100 straight-line PPC instructions are one guest block,
  // not 100 compiler blocks.
  std::vector<std::uint32_t> straight_line(100u, 0x38630001u);  // addi r3,r3,1
  const auto linear = compiler.compile(0x10000u, straight_line);
  assert(linear.ok);
  assert(linear.function.blocks.size() == 1u);
  const auto& linear_block = linear.function.blocks.front();
  assert(linear_block.guest_address == 0x10000u);
  assert(linear_block.end_address == 0x10190u);
  assert(linear_block.predecessors.empty());
  assert(linear_block.successors.empty());
  assert(linear_block.has_external_exit);

  // Straight-line arithmetic has no observable PC boundary until block exit.
  backend::CppAotBackend backend;
  const auto linear_source = backend.emit_function(linear.function, "linear_100");
  assert(linear_source.find("L_00010000: {") != std::string::npos);
  assert(linear_source.find("L_00010004: {") == std::string::npos);
  assert(count_text(linear_source, "state.cia=") == 1u);
  assert(linear_source.find("state.cia=65932u;") != std::string::npos);

  // A potentially faulting load must materialize its own guest PC, while the
  // arithmetic instructions on either side remain free of per-op PC stores.
  const std::array<std::uint32_t, 3> fault_words = {
      0x38630001u,  // addi r3,r3,1
      0x80830000u,  // lwz r4,0(r3)
      0x38630001u,  // addi r3,r3,1
  };
  const auto fault = compiler.compile(0x5000u, fault_words);
  assert(fault.ok);
  const auto fault_source = backend.emit_function(fault.function, "fault_site");
  assert(count_text(fault_source, "state.cia=") == 2u);
  assert(fault_source.find("state.cia=20484u;") != std::string::npos);

  // r3 = 0; r4 = 3; do { ++r3; } while (r3 != r4); blr
  constexpr GuestAddress loop_base = 0x2000u;
  const std::array<std::uint32_t, 6> loop_words = {
      0x38600000u,
      0x38800003u,
      0x38630001u,
      0x7C032000u,
      0x40000000u | (4u << 21) | (2u << 16) | 0xFFF8u,
      0x4E800020u,
  };
  const auto loop = compiler.compile(loop_base, loop_words);
  assert(loop.ok);
  assert(loop.function.blocks.size() == 3u);
  assert(loop.function.blocks[0].guest_address == 0x2000u);
  assert(loop.function.blocks[1].guest_address == 0x2008u);
  assert(loop.function.blocks[2].guest_address == 0x2014u);

  const auto& head = loop.function.blocks[0];
  const auto& body = loop.function.blocks[1];
  const auto& exit = loop.function.blocks[2];
  assert(has_edge(head, 0x2008u, ir::EdgeKind::Fallthrough, true));
  assert(has_edge(body, 0x2008u, ir::EdgeKind::Branch, true));
  assert(has_edge(body, 0x2014u, ir::EdgeKind::Fallthrough, true));
  assert(has_predecessor(body, 0x2000u));
  assert(has_predecessor(body, 0x2008u));
  assert(has_predecessor(exit, 0x2008u));
  assert(exit.has_indirect_exit);

  // A direct linked branch (call) is its own block boundary - deliberately,
  // not merely incidentally: this guarantees a setjmp continuation always has
  // an explicit local block label a propagated guest LongJump can resume at
  // (see ends_basic_block()'s comment in function_compiler.cpp). The call
  // itself is still call metadata, not a CFG successor/predecessor edge: the
  // call's target block's predecessor list must reflect only the real
  // fallthrough-CFG edge into it, never the calling block.
  constexpr GuestAddress call_base = 0x3000u;
  const std::array<std::uint32_t, 3> call_words = {
      0x48000009u,  // bl +8 -> 0x3008
      0x38630001u,
      0x4E800020u,
  };
  const auto calls = compiler.compile(call_base, call_words);
  assert(calls.ok);
  assert(calls.function.blocks.size() == 3u);
  const auto& call_block = calls.function.blocks[0];
  const auto& call_fallthrough_block = calls.function.blocks[1];
  const auto& call_target_block = calls.function.blocks[2];
  assert(call_block.guest_address == 0x3000u);
  assert(call_fallthrough_block.guest_address == 0x3004u);
  assert(call_target_block.guest_address == 0x3008u);
  assert(has_edge(call_block, 0x3008u, ir::EdgeKind::Call, true));
  // The call instruction ends its own block, so the block containing it also
  // falls through normally to the very next instruction.
  assert(has_edge(call_block, 0x3004u, ir::EdgeKind::Fallthrough, true));
  assert(has_predecessor(call_fallthrough_block, 0x3000u));
  assert(call_fallthrough_block.predecessors.size() == 1u);
  // 0x3008 is also the call's target, but the Call edge must never create a
  // predecessor entry - only the real fallthrough edge from 0x3004 may.
  assert(has_predecessor(call_target_block, 0x3004u));
  assert(call_target_block.predecessors.size() == 1u);

  // A direct branch outside the discovered function is an explicit external
  // exit and must never be mistaken for a local CFG edge.
  const std::array<std::uint32_t, 2> external_words = {
      0x48000100u,  // b +0x100
      0x38630001u,
  };
  const auto external = compiler.compile(0x4000u, external_words);
  assert(external.ok);
  assert(external.function.blocks.size() == 2u);
  assert(external.function.blocks[0].has_external_exit);
  assert(has_edge(external.function.blocks[0], 0x4100u,
                  ir::EdgeKind::Branch, false));

  // A conditional forward branch near the end of an AC6-shaped function must
  // split both destinations into local blocks. The target is internal to this
  // declared range; a later function start is intentionally outside it.
  constexpr GuestAddress ac6_shape_base = 0x8000u;
  constexpr GuestAddress ac6_shape_target = ac6_shape_base + 0x58u;
  std::vector<std::uint32_t> ac6_shape_words(24u, 0x38630001u);
  ac6_shape_words[0x40u / 4u] =
      (16u << 26) | (4u << 21) | (2u << 16) | (0x18u & 0xFFFCu);
  ac6_shape_words[0x58u / 4u] = 0x4E800020u;
  const auto ac6_shape = compiler.compile(ac6_shape_base, ac6_shape_words);
  assert(ac6_shape.ok);
  assert(std::any_of(ac6_shape.function.blocks.begin(), ac6_shape.function.blocks.end(),
                     [ac6_shape_target](const auto& block) {
                       return block.guest_address == ac6_shape_target;
                     }));
  const auto ac6_shape_source =
      backend.emit_function(ac6_shape.function, "ac6_shape");
  assert(ac6_shape_source.find("L_00008058: {") != std::string::npos);

  std::cout << "xenon_cpu_v2_cfg: ok\n";
  return 0;
}
