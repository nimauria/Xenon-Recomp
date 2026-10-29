// Executes native AOT code for every vector splat form and immediate.
//
// Regression (AC6): the native splat path emitted the lifter's mnemonic tag
// (IR imm0) as the element index/immediate. AC6's VMX memset builds its fill
// byte with `vspltisb v0,4 ... vspltb v0,v0,0`; with the tag those became
// shift 2 and element 10, so memset(p, 0, n) filled memory with 0x2A ('*').

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;

struct SplatCase {
  const char* mnemonic;
  std::uint32_t word;
  std::uint32_t field;
  bool vmx128;
  aot::VectorSemantic semantic;
  ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&);
};
extern const SplatCase kSplatCases[];
extern const unsigned kSplatCaseCount;

namespace {

Vector128 source_pattern() {
  Vector128 v{};
  for (unsigned i = 0; i < 16; ++i) v.bytes[i] = static_cast<std::uint8_t>(0x10u * i + i);  // 00 11 22 ..
  return v;
}

Vector128 filler() {
  Vector128 v{};
  v.bytes.fill(0xCCu);
  return v;
}

bool same(const Vector128& a, const Vector128& b) { return a.bytes == b.bytes; }

std::int32_t sign_extend5(std::uint32_t field) {
  return static_cast<std::int32_t>(field << 27) >> 27;
}

// The architected result for the VX forms, independent of aot_semantics.
Vector128 expected_vx(const SplatCase& c, const Vector128& src) {
  Vector128 r{};
  const std::string_view m = c.mnemonic;
  if (m == "vspltb") r.bytes.fill(src.bytes[c.field & 15u]);
  if (m == "vsplth") for (unsigned i = 0; i < 8; ++i) r.set_u16_be(i, src.u16_be(c.field & 7u));
  if (m == "vspltw") for (unsigned i = 0; i < 4; ++i) r.set_u32_be(i, src.u32_be(c.field & 3u));
  const auto simm = sign_extend5(c.field);
  if (m == "vspltisb") r.bytes.fill(static_cast<std::uint8_t>(simm));
  if (m == "vspltish") for (unsigned i = 0; i < 8; ++i) r.set_u16_be(i, static_cast<std::uint16_t>(simm));
  if (m == "vspltisw") for (unsigned i = 0; i < 4; ++i) r.set_u32_be(i, static_cast<std::uint32_t>(simm));
  return r;
}

}  // namespace

int main() {
  assert(kSplatCaseCount == 8u * 32u);
  FlatMemory memory(0x100u, 0x5000u);
  NullRuntimeServices runtime;
  const auto src = source_pattern();
  for (unsigned n = 0; n < kSplatCaseCount; ++n) {
    const auto& c = kSplatCases[n];
    CpuState reference_state{};
    const auto reference = aot::execute_vector(c.semantic, c.word, reference_state, src, src, {}, {});

    CpuState state{};
    for (auto& vr : state.vr) vr = src;
    if (!c.vmx128) state.vr[1] = filler();
    c.fn(state, memory, runtime);

    if (!c.vmx128) {
      assert(same(state.vr[1], expected_vx(c, src)) && "native splat must match the architected result");
      assert(same(reference, expected_vx(c, src)) && "reference splat must match the architected result");
      for (unsigned r = 0; r < state.vr.size(); ++r)
        if (r != 1u) assert(same(state.vr[r], src));
    } else {
      // Every register held `src`; exactly the destination may now differ.
      unsigned changed = 0;
      for (const auto& vr : state.vr) {
        if (same(vr, src)) continue;
        ++changed;
        assert(same(vr, reference) && "native VMX128 splat must match the reference semantics");
      }
      assert(changed <= 1u);
      if (changed == 0u) assert(same(reference, src));
    }
  }

  // AC6's memset fill-vector sequence for a zero fill must produce zero.
  const auto four = aot::vector_splat_immediate_byte<aot::VectorSemantic::vspltisb>(4u);
  assert(four.bytes[0] == 4u && four.bytes[15] == 4u);

  std::cout << "Vector splat codegen tests passed (" << kSplatCaseCount << " cases)\n";
  return 0;
}
