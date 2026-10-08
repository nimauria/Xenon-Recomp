#include <algorithm>
#include <optional>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

[[nodiscard]] bool owns_address(const DiscoveredFunction& function, GuestAddress address) {
  if (!function.ranges.empty()) {
    for (std::size_t i = 0; i + 1u < function.ranges.size(); i += 2u)
      if (address >= function.ranges[i] && address < function.ranges[i + 1u]) return true;
    return false;
  }
  return address >= function.guest_start && address < function.guest_end;
}

[[nodiscard]] bool has_block_entry(const DiscoveredFunction& function, GuestAddress address) {
  return std::any_of(function.ir.blocks.begin(), function.ir.blocks.end(),
                     [address](const auto& block) { return block.guest_address == address; });
}

// Part 2 (generated-code deduplication / shard ownership fix): enforces "ONE
// guest callable function address -> ONE canonical DiscoveredFunction
// record" directly on `functions`, as a safety net against any discovery
// path that manages to produce two entries sharing a guest_start. The wave
// engine's `claimed` set (see load_and_analyze()'s wave loop) already
// prevents this for every address-claiming path, and the FunctionChunk/
// independent-FunctionHint mutual-exclusion fix above closes the one path
// that could produce the same bytes under two canonical identities without
// literally duplicating a guest_start - so this should never actually merge
// anything in production. It exists anyway because Part 9's contract is "do
// not silently discard conflicting function definitions" for ANY future
// regression in this area, not just the ones already understood, and because
// downstream passes (registry/codegen emission) must never have to
// separately re-derive this invariant to stay correct. Requires `functions`
// to already be sorted by guest_start. Duplicate entries are merged, not
// dropped: provenance (`sources`) is unioned onto the kept record, and a
// compiled record is always preferred over an uncompiled one.
std::size_t deduplicate_functions_by_address(std::vector<DiscoveredFunction>& functions,
                                             std::vector<std::string>& warnings) {
  std::size_t merged = 0;
  std::vector<DiscoveredFunction> unique_functions;
  unique_functions.reserve(functions.size());
  for (auto& function : functions) {
    if (!unique_functions.empty() && unique_functions.back().guest_start == function.guest_start) {
      auto& kept = unique_functions.back();
      warnings.push_back("duplicate canonical function record for guest address 0x" +
                         hex_string(function.guest_start) + " ('" + kept.name + "' vs '" + function.name +
                         "') merged into one canonical record");
      for (const auto source : function.sources) add_source(kept, source);
      if (!kept.compiled && function.compiled) kept = std::move(function);
      ++merged;
      continue;
    }
    unique_functions.push_back(std::move(function));
  }
  functions = std::move(unique_functions);
  return merged;
}

void reconcile_call_graph_provenance(AnalysisReport& report) {
  // Reconcile graph provenance after all discovery waves. Worker wave timing
  // must not decide whether a target receives DirectCall/ValidatedTailCall.
  for (auto& function : report.functions) {
    for (const auto target : function.calls) {
      for (auto& callee : report.functions) {
        if (callee.guest_start != target) continue;
        if (std::find(callee.callers.begin(), callee.callers.end(), function.guest_start) ==
            callee.callers.end())
          callee.callers.push_back(function.guest_start);
        add_source(callee, DiscoverySource::DirectCall);
      }
    }
    for (const auto& branch : function.branches) {
      if (branch.linked || branch.target == function.guest_start) continue;
      for (auto& callee : report.functions) {
        if (callee.guest_start != branch.target) continue;
        add_source(callee, branch.indirect ? DiscoverySource::ResolvedIndirectBranch
                                          : DiscoverySource::DirectBranch);
        if (!branch.indirect && !branch.linked && branch.terminal && !branch.conditional)
          add_source(callee, DiscoverySource::ValidatedTailCall);
      }
    }
  }
  for (auto& function : report.functions) {
    function.authority = authority_for_sources(function.sources);
    function.confidence = confidence_for_sources(function.sources);
  }
}

// Weak overlapping starts are often alternate entries, outlined blocks or
// loop headers rather than real semantic functions. This is the generic
// equivalent of the pruning/"widen dropped branches" repair passes used by
// title-specific recomp projects: when a weak candidate begins at an actual
// basic-block entry already owned by a stronger compiled function, keep the
// address dispatchable but collapse the duplicate semantic function.
WeakFunctionAbsorption absorb_weak_functions(AnalysisReport& report) {
  WeakFunctionAbsorption absorption;
  auto& absorbed_entries = absorption.entries;
  auto& weak_functions_absorbed = absorption.absorbed;
  std::vector<bool> absorbed(report.functions.size(), false);
  for (std::size_t i = 0; i < report.functions.size(); ++i) {
    auto& victim = report.functions[i];
    if (!victim.compiled || victim.native_replacement || has_semantic_boundary_evidence(victim))
      continue;

    std::optional<std::size_t> best_owner;
    for (std::size_t j = 0; j < report.functions.size(); ++j) {
      if (i == j || absorbed[j]) continue;
      const auto& owner = report.functions[j];
      if (!owner.compiled || owner.native_replacement || !owns_address(owner, victim.guest_start) ||
          !has_block_entry(owner, victim.guest_start))
        continue;
      // Alternate-entry absorption must always land on a stable semantic owner.
      // Never build owner chains (weak function -> weak function -> strong function):
      // they make registry aliases order-dependent and can leave an alias naming an
      // owner that is itself removed later in this pass.
      if (!has_semantic_boundary_evidence(owner)) continue;
      if (!best_owner ||
          static_cast<unsigned>(owner.authority) >
              static_cast<unsigned>(report.functions[*best_owner].authority) ||
          (owner.authority == report.functions[*best_owner].authority &&
           owner.confidence > report.functions[*best_owner].confidence))
        best_owner = j;
    }
    if (!best_owner) continue;

    const auto& owner = report.functions[*best_owner];
    GuestEntryPoint entry{};
    entry.address = victim.guest_start;
    entry.owner_function = owner.guest_start;
    entry.block = victim.guest_start;
    entry.kind = GuestEntryKind::AlternateBlock;
    entry.sources = victim.sources;
    entry.confidence = victim.confidence;
    absorbed_entries.push_back(std::move(entry));
    report.warnings.push_back(
        "absorbed weak function candidate 0x" + hex_string(victim.guest_start) +
        " into canonical function 0x" + hex_string(owner.guest_start) +
        " as an alternate compiled entry");
    absorbed[i] = true;
    ++weak_functions_absorbed;
  }
  if (weak_functions_absorbed != 0u) {
    std::vector<DiscoveredFunction> kept;
    kept.reserve(report.functions.size() - weak_functions_absorbed);
    for (std::size_t i = 0; i < report.functions.size(); ++i)
      if (!absorbed[i]) kept.push_back(std::move(report.functions[i]));
    report.functions = std::move(kept);
  }
  return absorption;
}

}  // namespace xenon::recomp::detail
