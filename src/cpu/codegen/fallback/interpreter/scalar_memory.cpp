#include "cpu/codegen/fallback/fallback_interpreter.hpp"

namespace xenon::cpu::fallback {

// Scalar loads/stores, load-reserve/store-conditional, multiple and string moves.
[[nodiscard]] bool execute_scalar_memory(const DecodedInstruction& i,
                                         ExecutionContext& context) {
  auto& state = context.state;
  auto& mem = context.memory_access;
  const auto m = i.mnemonic();

  if (const auto access = load_access(m)) {
    const auto ea = effective_address(i, state, *access);
    state.gpr[i.rt()] = load_scalar(mem, ea, *access);
    if (access->update) state.gpr[i.ra()] = ea;
    return true;
  }

  if (const auto access = store_access(m)) {
    const auto ea = effective_address(i, state, *access);
    store_scalar(mem, ea, *access, state.gpr[i.rs()]);
    if (access->update) state.gpr[i.ra()] = ea;
    return true;
  }

  // Atomics (load-reserve / store-conditional). The reservation protocol is the
  // compiled path's exactly (backend_cpp_aot.cpp): a load-reserve replaces any
  // held reservation and records its token/width/address; a store-conditional
  // succeeds only if a reservation of the same width is still valid, always
  // clears it, and reports success in CR0.EQ (with XER.SO copied to CR0.SO).
  if (m == "lwarx" || m == "ldarx") {
    const bool word = m == "lwarx";
    const auto ea = static_cast<GuestAddress>((i.ra() != 0u ? state.gpr[i.ra()] : 0u) +
                                              state.gpr[i.rb()]);
    if (state.reservation.valid) mem.cancel_reservation(state.reservation.token);
    std::uint64_t observed = 0u;
    if (word) {
      std::uint32_t value = 0u;
      state.reservation.token = mem.reserve32(ea, value);
      observed = value;
    } else {
      std::uint64_t value = 0u;
      state.reservation.token = mem.reserve64(ea, value);
      observed = value;
    }
    state.reservation.valid = state.reservation.token != 0u;
    state.reservation.width = word ? 4u : 8u;
    state.reservation.address = ea;
    state.reservation.observed_value = observed;
    state.gpr[i.rt()] = observed;
    return true;
  }

  if (m == "stwcx" || m == "stdcx") {
    const bool word = m == "stwcx";
    const auto ea = static_cast<GuestAddress>((i.ra() != 0u ? state.gpr[i.ra()] : 0u) +
                                              state.gpr[i.rb()]);
    bool stored = false;
    if (state.reservation.valid) {
      if (state.reservation.width == (word ? 4u : 8u)) {
        stored = word ? mem.store_conditional32(ea, state.reservation.token,
                                                static_cast<std::uint32_t>(state.gpr[i.rs()]))
                      : mem.store_conditional64(ea, state.reservation.token, state.gpr[i.rs()]);
      } else {
        mem.cancel_reservation(state.reservation.token);
      }
    }
    state.reservation.clear();
    state.set_cr_field(0, static_cast<std::uint8_t>((stored ? 0x2u : 0u) | (state.xer_so() ? 0x1u : 0u)));
    return true;
  }

  // Load/store multiple word: rT..r31 from/to consecutive words at (rA|0) + d.
  if (m == "lmw" || m == "stmw") {
    const std::uint64_t base = i.ra() != 0u ? state.gpr[i.ra()] : 0u;
    for (unsigned r = i.rt(); r < 32u; ++r) {
      const auto ea = static_cast<GuestAddress>(
          base + static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm16())) + (r - i.rt()) * 4u);
      if (m == "lmw") state.gpr[r] = mem.read32_be(ea);
      else mem.write32_be(ea, static_cast<std::uint32_t>(state.gpr[r]));
    }
    return true;
  }

  // Load/store string: a byte count and register wrap defined by the architecture,
  // shared with the compiled path (aot::string_load/string_store).
  if (m == "lswi" || m == "lswx" || m == "stswi" || m == "stswx") {
    const bool indexed = m == "lswx" || m == "stswx";
    const std::uint64_t base = i.ra() != 0u ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(indexed ? base + state.gpr[i.rb()] : base);
    const std::uint32_t count = indexed ? static_cast<std::uint32_t>(state.xer & 0x7Fu)
                                        : (i.rb() == 0u ? 32u : i.rb());
    if (m == "lswi" || m == "lswx") aot::string_load(state, mem, ea, count, i.rt());
    else aot::string_store(state, mem, ea, count, i.rt());
    return true;
  }
  return false;
}

}  // namespace xenon::cpu::fallback
