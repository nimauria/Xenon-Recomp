#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/cpu/runtime.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;
using xenon::memory::AddressSpace;
using xenon::memory::kReadWrite;

#define FN(n) ExecutionResult n(CpuState&, MemoryPort&, RuntimeServices&)
FN(xm_store); FN(xm_load); FN(xm_lwarx); FN(xm_stwcx); FN(xm_dcbz); FN(xm_icbi);
#undef FN

int main() {
  AddressSpace memory;
  assert(memory.initialize());
  assert(memory.commit_fixed(0x00100000u, 0x10000u, kReadWrite));

  NullRuntimeServices runtime;
  CpuState state{};
  state.gpr[3] = 0x00100040u;
  state.gpr[4] = 0xA1B2C3D4u;

  // Actual generated PPC store/load uses the production AddressSpace.
  xm_store(state, memory, runtime);
  assert(memory.read32_be(0x00100040u) == 0xA1B2C3D4u);
  state.gpr[5] = 0;
  xm_load(state, memory, runtime);
  assert(state.gpr[5] == 0xA1B2C3D4u);

  // CPU and physical alias see exactly the same bytes.
  const auto physical = memory.get_physical_address(0x00100040u);
  assert(physical != 0xFFFFFFFFu);
  memory.write32_be(0xA0000000u + physical, 0x11223344u);
  xm_load(state, memory, runtime);
  assert(state.gpr[5] == 0x11223344u);

  // Load-reserve is invalidated by a write through a different physical alias.
  xm_lwarx(state, memory, runtime);
  assert(state.gpr[6] == 0x11223344u && state.reservation.valid);
  memory.write8(0xA0000000u + physical + 8u, 0xAAu);
  state.gpr[7] = 0x55667788u;
  xm_stwcx(state, memory, runtime);
  assert(memory.read32_be(0x00100040u) == 0x11223344u);
  assert((state.cr_field(0) & 0x2u) == 0);

  xm_lwarx(state, memory, runtime);
  xm_stwcx(state, memory, runtime);
  assert(memory.read32_be(0x00100040u) == 0x55667788u);
  assert((state.cr_field(0) & 0x2u) != 0);

  // dcbz changes the same physical backing used by CPU/GPU aliases.
  state.gpr[3] = 0x0010007Fu;
  for (unsigned i = 0; i < 32; ++i) memory.write8(0x00100060u + i, 0xCCu);
  xm_dcbz(state, memory, runtime);
  for (unsigned i = 0; i < 32; ++i) assert(memory.read8(0x00100060u + i) == 0);

  std::uint32_t invalidated = 0;
  memory.add_invalidation_callback([&](GuestAddress address) { invalidated = address; });
  state.gpr[3] = 0x00100044u;
  xm_icbi(state, memory, runtime);
  assert(invalidated == 0x00100044u);

  std::cout << "xenon_memory_cpu_integration: ok\n";
}
