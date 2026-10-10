#include "cpu/codegen/fallback/fallback_interpreter.hpp"

namespace xenon::cpu::fallback {

// VMX/VMX128 loads, stores, permutes and arithmetic.
[[nodiscard]] bool execute_vector(const DecodedInstruction& i,
                                  ExecutionContext& context) {
  auto& state = context.state;
  auto& mem = context.memory_access;
  const auto m = i.mnemonic();

  // Real AC6 repro: the entire VMX/VMX128 vector unit - the largest single
  // gap this interpreter has ever had - had no case here at all, starting
  // with "lvx128" (a real load AC6's own compiled code executes constantly
  // for matrix/skinning math). Loads/stores are handled directly (mirroring
  // lifter_memory.cpp's own vector-memory cases exactly); every arithmetic/
  // logic/compare/permute/pack/splat/select/rotate/shift mnemonic reuses
  // aot::execute_vector() - the same runtime-dispatched semantic function
  // the AOT-compiled path calls for everything it does not special-case for
  // performance (see vector_semantic_for()'s own comment above).
  if (m == "lvx" || m == "lvxl" || m == "lvx128" || m == "lvxl128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>((base + state.gpr[i.rb()]) & ~0xFull);
    state.vr[vector_vd(i)] = mem.read128(ea);
    return true;
  }

  if (m == "stvx" || m == "stvxl" || m == "stvx128" || m == "stvxl128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>((base + state.gpr[i.rb()]) & ~0xFull);
    mem.write128(ea, state.vr[vector_vd(i)]);
    return true;
  }

  if (m == "lvebx" || m == "lvehx" || m == "lvewx" || m == "lvewx128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const unsigned width = m == "lvebx" ? 1u : (m == "lvehx" ? 2u : 4u);
    const auto reg = vector_vd(i);
    state.vr[reg] = aot::vector_load_element(state.vr[reg], mem, ea, width);
    return true;
  }

  if (m == "stvebx" || m == "stvehx" || m == "stvewx" || m == "stvewx128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const unsigned width = m == "stvebx" ? 1u : (m == "stvehx" ? 2u : 4u);
    aot::vector_store_element(state.vr[vector_vd(i)], mem, ea, width);
    return true;
  }

  if (m.starts_with("lvlx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const auto reg = vector_vd(i);
    state.vr[reg] = aot::vector_load_left(state.vr[reg], mem, ea);
    return true;
  }

  if (m.starts_with("lvrx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    const auto reg = vector_vd(i);
    state.vr[reg] = aot::vector_load_right(state.vr[reg], mem, ea);
    return true;
  }

  if (m.starts_with("stvlx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    aot::vector_store_left(state.vr[vector_vd(i)], mem, ea);
    return true;
  }

  if (m.starts_with("stvrx")) {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = static_cast<GuestAddress>(base + state.gpr[i.rb()]);
    aot::vector_store_right(state.vr[vector_vd(i)], mem, ea);
    return true;
  }

  if (m == "lvsl" || m == "lvsl128" || m == "lvsr" || m == "lvsr128") {
    const auto base = i.ra() ? state.gpr[i.ra()] : 0u;
    const auto ea = base + state.gpr[i.rb()];
    state.vr[vector_vd(i)] = m.starts_with("lvsl") ? aot::vector_load_shift_left(ea)
                                                   : aot::vector_load_shift_right(ea);
    return true;
  }

  if (m == "mfvscr") {
    Vector128 v{};
    v.set_u32_be(3, state.vscr);
    state.vr[i.vd5()] = v;
    return true;
  }

  if (m == "mtvscr") {
    state.vscr = state.vr[i.vb5()].u32_be(3);
    return true;
  }

  // A handful of mnemonic families take a genuinely different operand set on
  // the classic 3-register VA-form encoding than on Xbox's extended VX128
  // encodings - mirroring lifter_vector.cpp's own per-format branches exactly
  // (same real hardware operand wiring, independently duplicated here since
  // the interpreter has no IR::Builder to share that code through). Every
  // other mnemonic below uses the same (va, vb, vc) operand set regardless of
  // format, which the generic dispatch after this block handles uniformly.
  if (m.starts_with("vmaddfp") || m.starts_with("vmaddcfp") || m.starts_with("vnmsubfp")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    Vector128 a, b, c;
    if (i.info->format == InstructionFormat::VA) {
      a = state.vr[vector_va(i)];
      b = state.vr[vector_vb(i)];
      c = state.vr[vector_vc(i)];
    } else {
      a = state.vr[reg];
      b = state.vr[vector_va(i)];
      c = state.vr[vector_vb(i)];
    }
    state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    return true;
  }

  if (m.starts_with("vsel")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    Vector128 a, b, c;
    if (i.info->format == InstructionFormat::VA) {
      a = state.vr[vector_va(i)];
      b = state.vr[vector_vb(i)];
      c = state.vr[vector_vc(i)];
    } else {
      a = state.vr[reg];
      b = state.vr[vector_va(i)];
      c = state.vr[vector_vb(i)];
    }
    state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    return true;
  }

  if (m.starts_with("vsum") || m.starts_with("vmsum")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    const auto a = state.vr[vector_va(i)];
    const auto b = state.vr[vector_vb(i)];
    const auto c = i.info->format == InstructionFormat::VA ? state.vr[vector_vc(i)] : Vector128{};
    state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    return true;
  }

  if (m.starts_with("vpermwi")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    const auto b = state.vr[vector_vb(i)];
    state.vr[reg] =
        aot::execute_vector(*semantic, i.word, state, Vector128{}, b, Vector128{}, Vector128{});
    return true;
  }

  if (m.starts_with("vperm")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    if (i.info->format == InstructionFormat::VA || i.info->format == InstructionFormat::VX128_2) {
      const auto a = state.vr[vector_va(i)];
      const auto b = state.vr[vector_vb(i)];
      const auto c = state.vr[vector_vc(i)];
      state.vr[reg] = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    } else {
      const auto b = state.vr[vector_vb(i)];
      state.vr[reg] =
          aot::execute_vector(*semantic, i.word, state, Vector128{}, b, Vector128{}, Vector128{});
    }
    return true;
  }

  if (m.starts_with("vrlimi")) {
    const auto semantic = vector_semantic_for(m);
    if (!semantic) return false;
    const auto reg = vector_vd(i);
    const auto old = state.vr[reg];
    const auto b = state.vr[vector_vb(i)];
    state.vr[reg] =
        aot::execute_vector(*semantic, i.word, state, old, b, Vector128{}, Vector128{});
    return true;
  }

  if (const auto semantic = vector_semantic_for(m)) {
    const auto reg = vector_vd(i);
    const auto a = state.vr[vector_va(i)];
    const auto b = state.vr[vector_vb(i)];
    const auto c = state.vr[vector_vc(i)];
    const auto result = aot::execute_vector(*semantic, i.word, state, a, b, c, Vector128{});
    state.vr[reg] = result;
    if (m.starts_with("vcmp") && i.rc()) {
      aot::update_cr6_from_vector_compare(state, result);
    }
    return true;
  }
  return false;
}

}  // namespace xenon::cpu::fallback
