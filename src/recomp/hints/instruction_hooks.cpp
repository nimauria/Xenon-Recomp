#include "xenon/recomp/instruction_hooks.hpp"

#include <algorithm>

namespace xenon::recomp {

namespace {

cpu::InstructionHookAction lower(analysis::HookAction action) noexcept {
  switch (action) {
    case analysis::HookAction::Jump: return cpu::InstructionHookAction::Jump;
    case analysis::HookAction::Return: return cpu::InstructionHookAction::Return;
    case analysis::HookAction::Continue: break;
  }
  return cpu::InstructionHookAction::Continue;
}

}  // namespace

void InstructionHookPlan::collect(cpu::GuestAddress begin, cpu::GuestAddress end,
                                  std::vector<cpu::InstructionHook>& out) const {
  const auto first = std::lower_bound(
      hooks.begin(), hooks.end(), begin,
      [](const cpu::InstructionHook& hook, cpu::GuestAddress address) { return hook.address < address; });
  for (auto it = first; it != hooks.end() && it->address < end; ++it) out.push_back(*it);
}

InstructionHookPlan plan_instruction_hooks(const analysis::AnalysisHintSetV2& hint_set) {
  InstructionHookPlan plan;
  for (const auto& hook : hint_set.hooks)
    if (hook.kind == analysis::HookKind::Instruction) plan.symbols.push_back(hook.symbol);
  std::sort(plan.symbols.begin(), plan.symbols.end());
  plan.symbols.erase(std::unique(plan.symbols.begin(), plan.symbols.end()), plan.symbols.end());

  for (const auto& hook : hint_set.hooks) {
    if (hook.kind != analysis::HookKind::Instruction) continue;
    cpu::InstructionHook lowered{};
    lowered.address = hook.address;
    lowered.ordinal = static_cast<std::uint32_t>(
        std::lower_bound(plan.symbols.begin(), plan.symbols.end(), hook.symbol) -
        plan.symbols.begin());
    lowered.after_instruction = hook.after_instruction;
    lowered.on_true = lower(hook.on_true);
    lowered.on_false = lower(hook.on_false);
    lowered.jump_on_true = hook.jump_on_true;
    lowered.jump_on_false = hook.jump_on_false;
    plan.hooks.push_back(lowered);
  }
  // Stable: hooks sharing an address keep their declaration order.
  std::stable_sort(plan.hooks.begin(), plan.hooks.end(),
                   [](const auto& a, const auto& b) { return a.address < b.address; });
  return plan;
}

}  // namespace xenon::recomp
