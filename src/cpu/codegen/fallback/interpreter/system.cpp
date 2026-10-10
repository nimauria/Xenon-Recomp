#include "cpu/codegen/fallback/fallback_interpreter.hpp"

namespace xenon::cpu::fallback {

// MSR, condition register, SPR and storage-control (sync, cache) instructions.
[[nodiscard]] bool execute_system(const DecodedInstruction& i,
                                  ExecutionContext& context) {
  auto& state = context.state;
  auto& mem = context.memory_access;
  const auto m = i.mnemonic();

  // Machine state register.
  if (m == "mfmsr") {
    state.gpr[i.rt()] = state.msr;
    return true;
  }

  if (m == "mtmsr" || m == "mtmsrd") {
    aot::write_msr(state, state.gpr[i.rs()], ((i.word >> 16u) & 1u) != 0u, m == "mtmsrd");
    return true;
  }

  if (m == "crand" || m == "crandc" || m == "creqv" || m == "crnand" ||
      m == "crnor" || m == "cror" || m == "crorc" || m == "crxor") {
    const auto bt = (i.word >> 21u) & 31u;
    const auto ba = (i.word >> 16u) & 31u;
    const auto bb = (i.word >> 11u) & 31u;
    const bool a = state.cr_bit(ba);
    const bool b = state.cr_bit(bb);
    bool result = false;
    if (m == "crand") result = a && b;
    else if (m == "crandc") result = a && !b;
    else if (m == "creqv") result = a == b;
    else if (m == "crnand") result = !(a && b);
    else if (m == "crnor") result = !(a || b);
    else if (m == "cror") result = a || b;
    else if (m == "crorc") result = a || !b;
    else result = a != b;
    state.set_cr_bit(bt, result);
    return true;
  }

  if (m == "mcrf") {
    const auto bf = (i.word >> 23u) & 7u;
    const auto bfa = (i.word >> 18u) & 7u;
    state.set_cr_field(bf, state.cr_field(bfa));
    return true;
  }

  if (m == "mcrxr") {
    const auto field = static_cast<std::uint8_t>(
        (state.xer_so() ? 8u : 0u) | (state.xer_ov() ? 4u : 0u) |
        (state.xer_ca() ? 2u : 0u));
    state.set_cr_field(i.crfd(), field);
    state.xer &= ~(xer_bits::SO | xer_bits::OV | xer_bits::CA);
    return true;
  }

  if (m == "mfcr") {
    state.gpr[i.rt()] = state.cr;
    return true;
  }

  if (m == "mtcrf") {
    const auto fxm = (i.word >> 12u) & 0xFFu;
    const auto source = static_cast<std::uint32_t>(state.gpr[i.rs()]);
    for (unsigned field = 0; field < 8u; ++field) {
      if ((fxm & (0x80u >> field)) == 0u) continue;
      const auto shift = (7u - field) * 4u;
      state.cr = (state.cr & ~(0xFu << shift)) | (source & (0xFu << shift));
    }
    return true;
  }

  if (m == "mfspr" || m == "mftb") {
    const auto spr = i.spr();
    std::uint64_t value = 0u;
    if (spr == 1u) value = state.xer;
    else if (spr == 8u) value = state.lr;
    else if (spr == 9u) value = state.ctr;
    else if (spr == 256u) value = state.vrsave;
    else if (spr == 268u) value = context.runtime.read_time_base(state);
    else if (spr == 269u) value = context.runtime.read_time_base(state) >> 32u;
    else if (spr == 287u) value = state.pvr;
    else value = context.runtime.read_spr(spr, state);
    state.gpr[i.rt()] = value;
    return true;
  }

  if (m == "mtspr") {
    const auto spr = i.spr();
    const auto value = state.gpr[i.rs()];
    if (spr == 1u) state.xer = static_cast<std::uint32_t>(value);
    else if (spr == 8u) state.lr = value;
    else if (spr == 9u) state.ctr = value;
    else if (spr == 256u) state.vrsave = static_cast<std::uint32_t>(value);
    else context.runtime.write_spr(spr, value, state);
    return true;
  }

  if (m == "sync") {
    mem.barrier(BarrierKind::Sync);
    return true;
  }

  if (m == "eieio") {
    mem.barrier(BarrierKind::Eieio);
    return true;
  }

  if (m == "isync") {
    mem.barrier(BarrierKind::InstructionSync);
    return true;
  }

  if (m == "dcbz" || m == "dcbz128") {
    const auto ea = static_cast<GuestAddress>((i.ra() ? state.gpr[i.ra()] : 0u) + state.gpr[i.rb()]);
    mem.zero_cache_block(ea, m == "dcbz128" ? 128u : 32u);
    return true;
  }

  if (m == "icbi") {
    const auto ea = static_cast<GuestAddress>((i.ra() ? state.gpr[i.ra()] : 0u) + state.gpr[i.rb()]);
    mem.instruction_cache_invalidate(ea);
    return true;
  }

  // Cache hints have no architecturally visible data result in this fallback.
  if (m == "dcbf" || m == "dcbi" || m == "dcbst" || m == "dcbt" || m == "dcbtst")
    return true;
  return false;
}

}  // namespace xenon::cpu::fallback
