#pragma once

#include <cstdint>

#include "xenon/cpu/runtime.hpp"

namespace xenon::recomp::runtime_helpers {

// Xbox 360 PPC jump-buffer layout used by the XDK/ReXCRT setjmp family.
// All scalar fields are stored in guest big-endian byte order.
inline constexpr std::uint32_t kJumpBufferFpr14Offset = 0u;
inline constexpr std::uint32_t kJumpBufferR1Offset = 144u;
inline constexpr std::uint32_t kJumpBufferR13Offset = 152u;
inline constexpr std::uint32_t kJumpBufferCrOffset = 304u;
inline constexpr std::uint32_t kJumpBufferLrOffset = 308u;
inline constexpr std::uint32_t kJumpBufferVr64Offset = 320u;
inline constexpr std::uint32_t kJumpBufferSize = 1344u;
inline constexpr std::uint32_t kJumpBufferAlignment = 16u;

cpu::ExecutionResult setjmp_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult longjmp_v2(cpu::ExecutionContext& context);

// PPC/Xenon-toolchain nonvolatile-register prologue/epilogue "fall-through
// ladder" spill helpers (analysis::RuntimeHelperKind::{Save,Restore}{GprLr,
// Fpr,Vmx,Vmx128} - see analysis_schema.hpp's header comment and
// docs/runtime/RUNTIME_HELPERS.md for the full derivation). `RegisterStart` is a
// compile-time template parameter rather than a runtime argument because
// Recomp Driver's generated `registry.cpp` (the only production caller)
// knows the concrete register-count variant for each guest address at
// codegen time and instantiates exactly the variants a title actually uses
// (analysis::expand_runtime_helpers()) - e.g.
// `&runtime_helpers::save_gpr_lr_v2<20>`.
//
// GPR/FPR offsets are direct register+immediate-displacement stores/loads
// relative to the guest r1 (stack pointer) the calling function already
// holds; VMX/VMX128 offsets are indexed stores/loads relative to the guest
// r12 the calling function sets up as a secondary frame-pointer alias
// (AltiVec/VMX128 load/store instructions have no immediate-displacement
// form, so the real compiled helper synthesizes the per-register offset
// into an index register - r11 - and reads it via r12 as the base; Xenon
// does not need to reproduce *how* r12 was computed, only read it, exactly
// as the real "lvx vN, r11, r12" instruction would).
[[nodiscard]] inline cpu::GuestAddress effective_address(std::uint64_t base,
                                                         std::int32_t offset) noexcept {
  return static_cast<cpu::GuestAddress>(
      static_cast<std::uint64_t>(static_cast<std::int64_t>(base) + offset));
}

// GPR/LR family: confirmed against real Xbox 360 binaries via community
// disassembly (e.g. hedge-dev/XenonRecomp issue discussion of Lost Odyssey/
// the "__savegprlr_14"/"__restgprlr_14" convention): the r14 slot sits at
// r1-0x98, ascending by 8 bytes per register up to r31, with the saved LR
// value in the next 8-byte slot beyond r31.
inline constexpr std::int32_t kGprLrBaseOffset = -0x98;
inline constexpr std::int32_t kGprLrLrOffset = kGprLrBaseOffset + 18 * 8;  // -0x08

// FPR family: NOT independently confirmed against a real disassembly during
// this pass (unlike GPR/VMX/VMX128 below) - a self-consistent placeholder
// placed clear of the other three families' confirmed offset ranges so a
// function using multiple register classes together cannot alias its own
// slots. Verify against a real title's disassembly before relying on this
// for actual FPR-spilling guest code; see docs/runtime/RUNTIME_HELPERS.md's "known
// limitation" note.
inline constexpr std::int32_t kFprBaseOffset = -0x420;

// VMX family (v14..v31): confirmed via community disassembly
// ("li r11,-0x120" + "lvx v14,r11,r12", ascending by 16 bytes per register).
inline constexpr std::int32_t kVmxBaseOffset = -0x120;

// Extended VMX128 family (v64..v127): confirmed via community disassembly
// ("li r11,-0x400" + "lvx128 v64,r11,r12", ascending by 16 bytes per
// register).
inline constexpr std::int32_t kVmx128BaseOffset = -0x400;

template <std::uint32_t RegisterStart>
cpu::ExecutionResult save_gpr_lr_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 14u && RegisterStart <= 31u);
  auto& state = context.state;
  const auto r1 = state.gpr[1];
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    context.memory.write64_be(
        effective_address(r1, kGprLrBaseOffset + static_cast<std::int32_t>((reg - 14u) * 8u)),
        state.gpr[reg]);
  }
  // r0 holds the value the calling function's own `mflr r0` placed there
  // immediately before its `bl` into this entry point - a real ABI
  // precondition of this shared helper, exactly like the real guest
  // routine relies on.
  context.memory.write64_be(effective_address(r1, kGprLrLrOffset), state.gpr[0]);
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult restore_gpr_lr_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 14u && RegisterStart <= 31u);
  auto& state = context.state;
  const auto r1 = state.gpr[1];
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    state.gpr[reg] = context.memory.read64_be(
        effective_address(r1, kGprLrBaseOffset + static_cast<std::int32_t>((reg - 14u) * 8u)));
  }
  // Restores the LR value the paired save_gpr_lr_v2 stashed (not whatever
  // is currently live in LR, which nested calls made since the save may
  // have clobbered) and returns through it - this entry point is a tail
  // substitute for the calling function's own epilogue, so it returns to
  // that function's *caller*, not back into itself.
  state.lr = context.memory.read64_be(effective_address(r1, kGprLrLrOffset));
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult save_fpr_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 14u && RegisterStart <= 31u);
  auto& state = context.state;
  const auto r1 = state.gpr[1];
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    context.memory.write64_be(
        effective_address(r1, kFprBaseOffset + static_cast<std::int32_t>((reg - 14u) * 8u)),
        state.fpr_bits[reg]);
  }
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult restore_fpr_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 14u && RegisterStart <= 31u);
  auto& state = context.state;
  const auto r1 = state.gpr[1];
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    state.fpr_bits[reg] = context.memory.read64_be(
        effective_address(r1, kFprBaseOffset + static_cast<std::int32_t>((reg - 14u) * 8u)));
  }
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult save_vmx_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 14u && RegisterStart <= 31u);
  auto& state = context.state;
  const auto r12 = state.gpr[12];
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    context.memory.write128(
        effective_address(r12, kVmxBaseOffset + static_cast<std::int32_t>((reg - 14u) * 16u)),
        state.vr[reg]);
  }
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult restore_vmx_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 14u && RegisterStart <= 31u);
  auto& state = context.state;
  const auto r12 = state.gpr[12];
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    state.vr[reg] = context.memory.read128(
        effective_address(r12, kVmxBaseOffset + static_cast<std::int32_t>((reg - 14u) * 16u)));
  }
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult save_vmx128_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 64u && RegisterStart <= 127u);
  auto& state = context.state;
  const auto r12 = state.gpr[12];
  for (std::uint32_t reg = RegisterStart; reg <= 127u; ++reg) {
    context.memory.write128(
        effective_address(r12, kVmx128BaseOffset + static_cast<std::int32_t>((reg - 64u) * 16u)),
        state.vr[reg]);
  }
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

template <std::uint32_t RegisterStart>
cpu::ExecutionResult restore_vmx128_v2(cpu::ExecutionContext& context) {
  static_assert(RegisterStart >= 64u && RegisterStart <= 127u);
  auto& state = context.state;
  const auto r12 = state.gpr[12];
  for (std::uint32_t reg = RegisterStart; reg <= 127u; ++reg) {
    state.vr[reg] = context.memory.read128(
        effective_address(r12, kVmx128BaseOffset + static_cast<std::int32_t>((reg - 64u) * 16u)));
  }
  return {cpu::FlowReason::Return, static_cast<cpu::GuestAddress>(state.lr & ~3ull), 0u};
}

}  // namespace xenon::recomp::runtime_helpers
