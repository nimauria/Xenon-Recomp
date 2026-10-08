#include "xenon/recomp/runtime_helpers.hpp"

#include <algorithm>
#include <limits>

#include "xenon/kernel/process.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::recomp::runtime_helpers {
namespace {

using cpu::ExecutionContext;
using cpu::ExecutionResult;
using cpu::FlowReason;
using cpu::GuestAddress;

constexpr std::uint32_t kInvalidJumpBufferTrap = 0x80000002u;

[[nodiscard]] bool valid_jump_buffer(ExecutionContext& context,
                                     GuestAddress address) noexcept {
  if (address == 0u || (address & (kJumpBufferAlignment - 1u)) != 0u ||
      address > (std::numeric_limits<GuestAddress>::max)() -
                    (kJumpBufferSize - 1u)) {
    return false;
  }

  auto* process = context.runtime.current_process();
  if (!process) return false;

  // Validate the full guest range rather than relying on a host fault. Jump
  // buffers normally live on the guest stack, but may legally straddle page
  // boundaries. Every covered mapping must therefore be committed and RW.
  const auto last = static_cast<GuestAddress>(address + kJumpBufferSize - 1u);
  for (GuestAddress cursor = address;;) {
    memory::MappingInfo info{};
    if (!process->memory().query_virtual(cursor, info) ||
        info.state != memory::PageState::Committed ||
        !memory::has(info.current_protect, memory::Protect::Read) ||
        !memory::has(info.current_protect, memory::Protect::Write)) {
      return false;
    }
    if (cursor >= last) break;
    const auto next_page = static_cast<std::uint64_t>(
        (cursor & ~(memory::kBasePageSize - 1u)) + memory::kBasePageSize);
    if (next_page > last) break;
    cursor = static_cast<GuestAddress>(next_page);
  }
  return true;
}

ExecutionResult invalid_buffer(cpu::CpuState& state) noexcept {
  return {FlowReason::Trap, state.cia, kInvalidJumpBufferTrap};
}

ExecutionResult guest_return(cpu::CpuState& state) noexcept {
  return {FlowReason::Return, static_cast<GuestAddress>(state.lr & ~3ull), 0u};
}

}  // namespace

ExecutionResult setjmp_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto buffer = static_cast<GuestAddress>(state.gpr[3]);
  if (!valid_jump_buffer(context, buffer)) return invalid_buffer(state);

  // Xbox PPC nonvolatile context. This is deliberately guest memory rather
  // than a host jmp_buf: the saved state remains endian-correct, inspectable
  // by guest code and independent of the host C++ stack architecture.
  for (std::uint32_t reg = 14u; reg <= 31u; ++reg) {
    context.memory.write64_be(buffer + kJumpBufferFpr14Offset + (reg - 14u) * 8u,
                              state.fpr_bits[reg]);
  }
  context.memory.write64_be(buffer + kJumpBufferR1Offset, state.gpr[1]);
  for (std::uint32_t reg = 13u; reg <= 31u; ++reg) {
    context.memory.write64_be(buffer + kJumpBufferR13Offset + (reg - 13u) * 8u,
                              state.gpr[reg]);
  }
  context.memory.write32_be(buffer + kJumpBufferCrOffset, state.cr);
  context.memory.write32_be(buffer + kJumpBufferLrOffset,
                            static_cast<std::uint32_t>(state.lr));
  // Eight bytes of ABI padding separate LR from the vector save area.
  context.memory.write64_be(buffer + 312u, 0u);
  for (std::uint32_t reg = 64u; reg <= 127u; ++reg) {
    context.memory.write128(buffer + kJumpBufferVr64Offset + (reg - 64u) * 16u,
                            state.vr[reg]);
  }

  state.gpr[3] = 0u;
  return guest_return(state);
}

ExecutionResult longjmp_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto buffer = static_cast<GuestAddress>(state.gpr[3]);
  const auto requested_value = static_cast<std::uint32_t>(state.gpr[4]);
  if (!valid_jump_buffer(context, buffer)) return invalid_buffer(state);

  for (std::uint32_t reg = 14u; reg <= 31u; ++reg) {
    state.fpr_bits[reg] = context.memory.read64_be(
        buffer + kJumpBufferFpr14Offset + (reg - 14u) * 8u);
  }
  state.gpr[1] = context.memory.read64_be(buffer + kJumpBufferR1Offset);
  for (std::uint32_t reg = 13u; reg <= 31u; ++reg) {
    state.gpr[reg] = context.memory.read64_be(
        buffer + kJumpBufferR13Offset + (reg - 13u) * 8u);
  }
  state.cr = context.memory.read32_be(buffer + kJumpBufferCrOffset);
  state.lr = context.memory.read32_be(buffer + kJumpBufferLrOffset);
  for (std::uint32_t reg = 64u; reg <= 127u; ++reg) {
    state.vr[reg] = context.memory.read128(
        buffer + kJumpBufferVr64Offset + (reg - 64u) * 16u);
  }

  const auto value = requested_value == 0u ? 1u : requested_value;
  state.gpr[3] = value;
  const auto continuation = static_cast<GuestAddress>(state.lr & ~3ull);
  state.cia = continuation;
  state.nia = continuation + 4u;

  // Do not host-longjmp through generated C++ frames. The explicit result is
  // propagated by CPU V2 call sites until the compiled parent containing this
  // continuation performs a native goto to the saved guest block.
  return {FlowReason::LongJump, continuation, value};
}

}  // namespace xenon::recomp::runtime_helpers
