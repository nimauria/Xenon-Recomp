// memcpy/memmove/memcpy_s/memcmp replacements: large copies, overlapping
// ranges in both directions (correct as memmove), and compares that differ
// late, across the stage size the replacements copy through.

#include <cassert>
#include <cstdint>
#include <vector>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"
#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/native_replacements.hpp"

using namespace xenon::cpu;
namespace analysis = xenon::recomp::analysis;
namespace replacements = xenon::recomp::native_replacements;

namespace {
constexpr GuestAddress kBase = 0x1000u;
constexpr std::size_t kSize = 0x10000u;

std::uint8_t pattern(std::size_t i) { return static_cast<std::uint8_t>((i * 131u + 7u) ^ (i >> 8)); }

struct Rig {
  FlatMemory memory{kSize, kBase};
  NullRuntimeServices runtime;
  CpuState state{};
  ExecutionContext context{state, memory, runtime};
  Rig() {
    for (std::size_t i = 0; i < kSize; ++i) memory.write8(kBase + static_cast<GuestAddress>(i), pattern(i));
    state.lr = 0x9000u;
  }
  ExecutionResult run(analysis::NativeReplacementKind kind, std::uint64_t a, std::uint64_t b,
                      std::uint64_t c, std::uint64_t d = 0) {
    state.gpr[3] = a; state.gpr[4] = b; state.gpr[5] = c; state.gpr[6] = d;
    const auto fn = replacements::entry_for(kind);
    assert(fn);
    const auto r = fn(context);
    assert(r.reason == FlowReason::Return && r.next_address == 0x9000u);
    return r;
  }
  std::vector<std::uint8_t> bytes() {
    std::vector<std::uint8_t> v;
    for (std::size_t i = 0; i < kSize; ++i) v.push_back(memory.read8(kBase + static_cast<GuestAddress>(i)));
    return v;
  }
};

// The reference: memmove on a host copy.
void reference_move(std::vector<std::uint8_t>& v, std::size_t dst, std::size_t src, std::size_t n) {
  std::vector<std::uint8_t> tmp(v.begin() + src, v.begin() + src + n);
  std::copy(tmp.begin(), tmp.end(), v.begin() + dst);
}
}  // namespace

int main() {
  using K = analysis::NativeReplacementKind;
  struct Case { std::size_t dst, src, n; };
  // Small, exactly one stage, just over, large disjoint, and overlaps both ways.
  const Case cases[] = {
      {0x4000, 0x1000, 100},  {0x4000, 0x1000, 4096}, {0x4000, 0x1000, 4097},
      {0x6000, 0x1000, 0x4000},
      {0x1010, 0x1000, 9000}, {0x1000, 0x1010, 9000}, {0x1001, 0x1000, 5000},
      {0x1000, 0x1001, 5000}, {0x2000, 0x1000, 0x1000 + 1}, {0x1000, 0x2000, 0x1000 + 1},
      {0x1000, 0x1000, 8192},
  };
  for (const auto& c : cases) {
    for (const auto kind : {K::Memcpy, K::Memmove}) {
      Rig rig;
      auto expected = rig.bytes();
      reference_move(expected, c.dst, c.src, c.n);
      rig.run(kind, kBase + c.dst, kBase + c.src, c.n);
      assert(rig.bytes() == expected);
      assert(rig.state.gpr[3] == kBase + c.dst);
    }
    Rig rig;
    auto expected = rig.bytes();
    reference_move(expected, c.dst, c.src, c.n);
    rig.run(K::MemcpyChecked, kBase + c.dst, c.n, kBase + c.src, c.n);
    assert(rig.bytes() == expected && rig.state.gpr[3] == 0u);
  }
  {
    Rig rig;  // memcpy_s refuses to overrun the destination and copies nothing
    const auto before = rig.bytes();
    rig.run(K::MemcpyChecked, kBase + 0x4000, 10, kBase + 0x1000, 11);
    assert(rig.state.gpr[3] == 34u && rig.bytes() == before);
  }
  {
    Rig rig;  // memcmp over equal copies, then a difference at the end, in the middle
    constexpr std::uint64_t kA = kBase + 0x2000, kB = kBase + 0x9000;
    constexpr std::uint64_t kN = 0x6000;
    rig.run(K::Memcpy, kB, kA, kN);
    rig.run(K::Memcmp, kA, kB, kN);
    assert(rig.state.gpr[3] == 0u);
    rig.memory.write8(static_cast<GuestAddress>(kB + kN - 1), static_cast<std::uint8_t>(rig.memory.read8(static_cast<GuestAddress>(kA + kN - 1)) + 1u));
    rig.run(K::Memcmp, kA, kB, kN);
    assert(static_cast<std::int32_t>(rig.state.gpr[3]) == -1);
    rig.run(K::Memcmp, kB, kA, kN);
    assert(rig.state.gpr[3] == 1u);
    rig.run(K::Memcmp, kA, kB, kN - 1u);  // the difference lies past the count
    assert(rig.state.gpr[3] == 0u);
    rig.memory.write8(static_cast<GuestAddress>(kB + 5000u), static_cast<std::uint8_t>(rig.memory.read8(static_cast<GuestAddress>(kA + 5000u)) + 2u));
    rig.run(K::Memcmp, kA, kB, kN);
    assert(static_cast<std::int32_t>(rig.state.gpr[3]) == -2);
    rig.run(K::Memcmp, kA, kB, 0u);
    assert(rig.state.gpr[3] == 0u);
  }
  return 0;
}
