#pragma once

// A hint set's instruction hooks (analysis::HookKind::Instruction) lowered
// for the function compiler and the C++ emitter. The module supplies the
// routines (see DriverOptions::hook_sources); Xenon places the calls.

#include <span>
#include <string>
#include <vector>

#include "xenon/cpu/function_compiler.hpp"
#include "xenon/recomp/analysis_schema.hpp"

namespace xenon::recomp {

struct InstructionHookPlan {
  // Sorted by address. Each hook's ordinal indexes `symbols`.
  std::vector<cpu::InstructionHook> hooks;
  // Distinct routine names, sorted, so an ordinal only shifts when the set of
  // names changes.
  std::vector<std::string> symbols;

  [[nodiscard]] bool empty() const noexcept { return hooks.empty(); }
  // The hooks at addresses in [begin, end), appended to `out`.
  void collect(cpu::GuestAddress begin, cpu::GuestAddress end,
               std::vector<cpu::InstructionHook>& out) const;
};

[[nodiscard]] InstructionHookPlan plan_instruction_hooks(
    const analysis::AnalysisHintSetV2& hint_set);

}  // namespace xenon::recomp
