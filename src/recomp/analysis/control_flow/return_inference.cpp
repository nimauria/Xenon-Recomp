#include <algorithm>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

ReturnInferenceStats infer_return_behaviors(std::vector<DiscoveredFunction>& functions,
                                            std::vector<std::string>& warnings) {
  ReturnInferenceStats stats{};

  for (auto& function : functions) {
    if (function.return_behavior_explicit && function.return_behavior == ReturnBehavior::NoReturn &&
        function.has_explicit_return) {
      warnings.push_back("NoReturn metadata conflicts with an explicit return in function 0x" +
                         hex_string(function.guest_start) + "; preserving explicit module metadata");
    } else if (function.has_explicit_return && !function.return_behavior_explicit) {
      function.return_behavior = ReturnBehavior::MayReturn;
    }
  }

  const auto find_function = [&](GuestAddress address) -> DiscoveredFunction* {
    const auto it = std::lower_bound(functions.begin(), functions.end(), address,
                                     [](const DiscoveredFunction& fn, GuestAddress value) {
                                       return fn.guest_start < value;
                                     });
    return it != functions.end() && it->guest_start == address ? &*it : nullptr;
  };

  // Monotone fixed point: Unknown may become MayReturn or NoReturn, and once
  // proven it never moves back. This is deliberately about terminal tail
  // edges only; ordinary linked calls are not assumed to terminate their
  // caller without path-sensitive CFG proof.
  const auto max_iterations = std::max<std::size_t>(1u, functions.size() + 1u);
  for (std::size_t iteration = 0; iteration < max_iterations; ++iteration) {
    bool changed = false;
    ++stats.iterations;
    for (auto& function : functions) {
      if (function.return_behavior != ReturnBehavior::Unknown) continue;

      bool has_external_terminal_exit = false;
      bool all_targets_resolved = true;
      bool all_no_return = true;
      bool any_may_return = false;
      for (const auto& branch : function.branches) {
        if (branch.linked || !branch.terminal || branch.conditional ||
            owns_address(function, branch.target))
          continue;
        has_external_terminal_exit = true;
        const auto* target = find_function(branch.target);
        if (!target) {
          all_targets_resolved = false;
          all_no_return = false;
          continue;
        }
        if (target->return_behavior == ReturnBehavior::MayReturn) any_may_return = true;
        if (target->return_behavior != ReturnBehavior::NoReturn) all_no_return = false;
        if (target->return_behavior == ReturnBehavior::Unknown) all_targets_resolved = false;
      }

      if (any_may_return) {
        function.return_behavior = ReturnBehavior::MayReturn;
        changed = true;
      } else if (has_external_terminal_exit && all_targets_resolved && all_no_return) {
        function.return_behavior = ReturnBehavior::NoReturn;
        changed = true;
      }
    }
    if (!changed) break;
  }

  for (const auto& function : functions) {
    if (function.return_behavior == ReturnBehavior::NoReturn) ++stats.no_return;
    else if (function.return_behavior == ReturnBehavior::MayReturn) ++stats.may_return;
  }
  return stats;
}

}  // namespace xenon::recomp::detail
