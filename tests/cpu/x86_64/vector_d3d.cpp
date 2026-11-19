// VMX128 vpkd3d128/vupkd3d128 against the xenia-project/xenia PPC hardware
// test vectors, as real generated code and in the dynamic-fallback
// interpreter. Every other vector register is poisoned with CDCDCDCD, so a
// read of the immediate field as a register, or a lost vD merge, shows.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>

#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;

struct D3DVectorCase {
  const char* name;
  std::uint32_t word;
  unsigned src, dst;
  std::uint32_t src_in[4];
  std::uint32_t out[4];
  ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&);
};
extern const D3DVectorCase kD3DVectorCases[];
extern const unsigned kD3DVectorCaseCount;

namespace {

bool check(const char* path, const D3DVectorCase& c, const CpuState& state) {
  bool ok = true;
  for (unsigned lane = 0; lane < 4; ++lane) ok &= state.vr[c.dst].u32_be(lane) == c.out[lane];
  if (c.src != c.dst)
    for (unsigned lane = 0; lane < 4; ++lane) ok &= state.vr[c.src].u32_be(lane) == c.src_in[lane];
  if (!ok) {
    std::fprintf(stderr, "%s %s: v%u = %08X %08X %08X %08X, want %08X %08X %08X %08X\n", path, c.name,
                 c.dst, state.vr[c.dst].u32_be(0), state.vr[c.dst].u32_be(1),
                 state.vr[c.dst].u32_be(2), state.vr[c.dst].u32_be(3), c.out[0], c.out[1], c.out[2],
                 c.out[3]);
  }
  return ok;
}

}  // namespace

int main() {
  FlatMemory memory(0x100u, 0x6000u);
  NullRuntimeServices runtime;
  xenon::memory::AddressSpace guest;
  assert(guest.initialize());
  assert(guest.commit_fixed(0x00640000u, xenon::memory::kBasePageSize,
                            xenon::memory::kReadWriteExecute));
  bool all = true;
  for (unsigned n = 0; n < kD3DVectorCaseCount; ++n) {
    const auto& c = kD3DVectorCases[n];
    CpuState initial{};
    for (auto& v : initial.vr) v.bytes.fill(0xCD);
    for (unsigned lane = 0; lane < 4; ++lane) initial.vr[c.src].set_u32_be(lane, c.src_in[lane]);

    auto native = initial;
    c.fn(native, memory, runtime);
    all &= check("native", c, native);

    guest.write32_be(0x00640000u, c.word);
    guest.write32_be(0x00640004u, 0x4E800020u);  // blr
    auto interpreted = initial;
    interpreted.lr = 0x0064F000u;
    DynamicFallbackExecutor fallback;
    ExecutionContext context(interpreted, guest, runtime);
    fallback.bind(context);
    const auto result = context.try_dynamic_fallback(0x00640000u, CompiledLookupKind::Call);
    assert(result.handled && result.result.reason == FlowReason::Return);
    all &= check("fallback", c, interpreted);
  }
  assert(all);
  std::cout << "xenon_cpu_vector_d3d: " << kD3DVectorCaseCount << " cases ok\n";
}
