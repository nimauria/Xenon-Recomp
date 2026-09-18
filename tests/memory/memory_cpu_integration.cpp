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
FN(xm_sync); FN(xm_lwsync); FN(xm_eieio); FN(xm_isync);
FN(xm_lswi); FN(xm_stswi); FN(xm_lvlx); FN(xm_stvlx);
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

  // Generated stwcx. delegates identity to Xenon Memory, so a different guest
  // alias of the exact same physical word is valid. The old AOT path compared
  // guest virtual addresses and incorrectly rejected this case.
  state.gpr[3] = 0x00100040u;
  memory.write32_be(state.gpr[3], 0xA0B0C0D0u);
  xm_lwarx(state, memory, runtime);
  assert(state.reservation.valid);
  state.gpr[3] = 0xA0000000u + physical;
  state.gpr[7] = 0xD0C0B0A0u;
  xm_stwcx(state, memory, runtime);
  assert(memory.read32_be(0x00100040u) == 0xD0C0B0A0u);
  assert((state.cr_field(0) & 0x2u) != 0);

  // A second generated load-reserve replaces the reservation previously held
  // by this CpuState rather than leaking one of the six hardware-thread slots.
  state.gpr[3] = 0x00100040u;
  xm_lwarx(state, memory, runtime);
  const auto replaced_token = state.reservation.token;
  state.gpr[3] = 0x00100140u;
  memory.write32_be(state.gpr[3], 0x01020304u);
  xm_lwarx(state, memory, runtime);
  assert(state.reservation.valid && state.reservation.token != replaced_token);
  assert(!memory.store_conditional32(0x00100040u, replaced_token, 0xFFFFFFFFu));
  memory.cancel_reservation(state.reservation.token);
  state.reservation.clear();

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

  // Generated PPC barriers retain their distinct canonical kinds all the way
  // through the AOT backend into Xenon Memory. These execute the production
  // host-ordering implementation rather than a generic seq_cst placeholder.
  xm_sync(state, memory, runtime);
  xm_lwsync(state, memory, runtime);
  xm_eieio(state, memory, runtime);
  xm_isync(state, memory, runtime);

  // Generated PPC string operations use MemoryAccessContext range access. Put
  // the six-byte transfer across a 4 KiB boundary so both hot pages are used.
  state.gpr[3] = 0x00100FFDu;
  const std::uint8_t string_bytes[6] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
  for (unsigned i = 0; i < 6; ++i) memory.write8(state.gpr[3] + i, string_bytes[i]);
  state.gpr[30] = state.gpr[31] = ~0ull;
  xm_lswi(state, memory, runtime);
  assert(state.gpr[30] == 0x10111213u);
  assert(state.gpr[31] == 0x14150000u);
  state.gpr[30] = 0xA1B2C3D4u;
  state.gpr[31] = 0xE5F60000u;
  xm_stswi(state, memory, runtime);
  const std::uint8_t stored_string[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
  for (unsigned i = 0; i < 6; ++i) assert(memory.read8(state.gpr[3] + i) == stored_string[i]);

  // VMX left partial transfers are range operations too; unspecified lanes keep
  // Xenon's existing deterministic zero behavior.
  state.gpr[3] = 0x00100103u;
  for (unsigned i = 0; i < 16; ++i) memory.write8(0x00100100u + i, 0x80u + i);
  state.vr[1].bytes.fill(0xCCu);
  xm_lvlx(state, memory, runtime);
  for (unsigned i = 0; i < 16; ++i) {
    const auto expected = i < 13u ? static_cast<std::uint8_t>(0x83u + i) : 0u;
    assert(state.vr[1].bytes[i] == expected);
  }
  for (unsigned i = 0; i < 16; ++i) state.vr[1].bytes[i] = 0x20u + i;
  for (unsigned i = 0; i < 16; ++i) memory.write8(0x00100100u + i, 0xEEu);
  xm_stvlx(state, memory, runtime);
  for (unsigned i = 0; i < 16; ++i) {
    const auto expected = i < 3u ? 0xEEu : static_cast<std::uint8_t>(0x20u + i - 3u);
    assert(memory.read8(0x00100100u + i) == expected);
  }

  std::cout << "xenon_memory_cpu_integration: ok\n";
}
