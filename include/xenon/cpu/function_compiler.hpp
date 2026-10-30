#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/ir.hpp"
#include "xenon/cpu/lifter.hpp"

namespace xenon::cpu {

struct FunctionCompileResult {
  bool ok{};
  ir::Function function{};
  GuestAddress error_address{};
  std::uint32_t error_word{};
  std::string error{};
};

// One contiguous piece of a logical guest function. A function may own
// multiple non-contiguous ranges (FunctionChunk metadata) while still being
// compiled into one IR/CFG.
struct FunctionCodeRange {
  GuestAddress base{};
  std::span<const std::uint32_t> words{};
};

// What a hook's boolean result does to guest control flow.
enum class InstructionHookAction : std::uint8_t {
  Continue = 0,  // carry on with the guest instruction stream
  Jump = 1,      // branch to the action's guest target
  Return = 2,    // return to the guest link register, as blr does
};

// A module-supplied native routine run at one guest instruction (a
// mid-instruction hook). It runs before the instruction at `address`, or
// after it when `after_instruction` is set, with every guest register in
// CpuState and full memory access. A hook whose actions are all Continue is
// `void(ExecutionContext&)`; any other returns bool and takes the matching
// action. Modules supply the routine; this only places the call.
struct InstructionHook {
  GuestAddress address{};
  std::uint32_t ordinal{};  // index into the module's hook symbol table
  bool after_instruction{};
  InstructionHookAction on_true{InstructionHookAction::Continue};
  InstructionHookAction on_false{InstructionHookAction::Continue};
  GuestAddress jump_on_true{};
  GuestAddress jump_on_false{};

  [[nodiscard]] constexpr bool returns_bool() const noexcept {
    return on_true != InstructionHookAction::Continue ||
           on_false != InstructionHookAction::Continue;
  }
  // Packed into HostHook's imm1.
  [[nodiscard]] constexpr std::uint64_t packed_actions() const noexcept {
    return static_cast<std::uint64_t>(on_true) |
           (static_cast<std::uint64_t>(on_false) << 2u);
  }
};

// Builds architecture-neutral Xenon IR for a statically-known guest function.
// Function boundaries come from the game package / analysis layer; this class
// intentionally does not discover executable memory or depend on the RAM model.
class StaticFunctionCompiler {
 public:
  [[nodiscard]] FunctionCompileResult compile(
      GuestAddress base, std::span<const std::uint32_t> words) const;

  // Sparse whole-function path used by FunctionChunk metadata. All supplied
  // ranges participate in one CFG and retain their original guest addresses.
  // `extra_leaders` are block starts no direct branch reveals - resolved
  // switch-table cases - so each is a dispatchable block of this function.
  // `hooks` at addresses inside the ranges become HostHook operations; a
  // hook that cannot be placed (after a block-ending instruction, or at an
  // address no range covers) fails the compile rather than being dropped.
  [[nodiscard]] FunctionCompileResult compile_ranges(
      GuestAddress entry, std::span<const FunctionCodeRange> ranges,
      std::span<const GuestAddress> extra_leaders = {},
      std::span<const InstructionHook> hooks = {}) const;

 private:
  Decoder decoder_{};
  Lifter lifter_{};
};

}  // namespace xenon::cpu
