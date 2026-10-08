#include <algorithm>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

const DiscoveredFunction* find_strongest_owner(const std::vector<DiscoveredFunction>& functions,
                                               GuestAddress target) {
  const DiscoveredFunction* best = nullptr;
  for (const auto& candidate : functions) {
    if (!candidate.compiled || candidate.native_replacement || !owns_address(candidate, target)) continue;
    if (!best || static_cast<unsigned>(candidate.authority) > static_cast<unsigned>(best->authority) ||
        (candidate.authority == best->authority && candidate.confidence > best->confidence))
      best = &candidate;
  }
  return best;
}

void build_guest_entry_map(AnalysisReport& report, std::vector<GuestEntryPoint> absorbed_entries,
                           const DriverOptions& effective,
                           const std::vector<analysis::RuntimeHelper>& expanded_runtime_helpers) {
  // Build the complete guest-entry map. The same helper merges evidence when
  // several mechanisms independently identify the same entry. A canonical
  // function start outranks an alternate entry; alternate entries never
  // overwrite semantic function identity.
  report.entries.clear();
  const auto entry_kind_rank = [](GuestEntryKind kind) {
    switch (kind) {
      case GuestEntryKind::NativeReplacement: return 4;
      case GuestEntryKind::Function: return 3;
      case GuestEntryKind::RuntimeHelper: return 2;
      case GuestEntryKind::ImportThunk: return 2;
      case GuestEntryKind::AlternateBlock: return 1;
    }
    return 0;
  };
  const auto add_entry = [&](GuestEntryPoint entry) {
    auto it = std::find_if(report.entries.begin(), report.entries.end(),
                           [&](const auto& existing) { return existing.address == entry.address; });
    if (it == report.entries.end()) {
      report.entries.push_back(std::move(entry));
      return;
    }
    for (const auto source : entry.sources) add_source(it->sources, source);
    it->confidence = std::max(it->confidence, entry.confidence);
    if (entry_kind_rank(entry.kind) > entry_kind_rank(it->kind)) {
      it->kind = entry.kind;
      it->owner_function = entry.owner_function;
      it->block = entry.block;
    }
  };

  for (const auto& function : report.functions) {
    if (function.import_thunk) {
      // Deliberately uncompiled (see analyze_function_candidate()'s
      // import-thunk short-circuit) but still a legitimate dispatch target:
      // publish it as an entry so entry-integrity validation recognizes a
      // `bl` OR plain-branch/tail-call edge landing here as valid, instead
      // of reporting a hole - resolution itself still happens purely at
      // runtime via XenonSession::call()'s guest_thunk match, never via a
      // codegen entry.
      GuestEntryPoint entry{};
      entry.address = function.guest_start;
      entry.owner_function = function.guest_start;
      entry.block = function.guest_start;
      entry.kind = GuestEntryKind::ImportThunk;
      entry.sources = function.sources;
      entry.confidence = function.confidence;
      add_entry(std::move(entry));
      continue;
    }
    if (!function.compiled) continue;
    GuestEntryPoint entry{};
    entry.address = function.guest_start;
    entry.owner_function = function.guest_start;
    entry.block = function.guest_start;
    entry.kind = function.native_replacement ? GuestEntryKind::NativeReplacement
                                             : GuestEntryKind::Function;
    entry.sources = function.sources;
    entry.confidence = function.confidence;
    add_entry(std::move(entry));
  }
  for (auto& entry : absorbed_entries) add_entry(std::move(entry));

  const auto find_owner = [&report](GuestAddress target) {
    return find_strongest_owner(report.functions, target);
  };

  // Any branch entering another function's interior becomes a first-class
  // alternate dispatch entry if that exact address is a materialized block.
  for (const auto& source : report.functions) {
    for (const auto& branch : source.branches) {
      if (branch.linked) continue;
      const auto* owner = find_owner(branch.target);
      if (!owner || branch.target == owner->guest_start || source.guest_start == owner->guest_start)
        continue;
      if (!has_block_entry(*owner, branch.target)) continue;
      GuestEntryPoint entry{};
      entry.address = branch.target;
      entry.owner_function = owner->guest_start;
      entry.block = branch.target;
      entry.kind = GuestEntryKind::AlternateBlock;
      entry.sources = {branch.indirect ? DiscoverySource::ResolvedIndirectBranch
                                       : DiscoverySource::DirectBranch};
      entry.confidence = confidence_for_sources(entry.sources);
      add_entry(std::move(entry));
    }
  }

  // Runtime feedback may name an interior block even when no static edge did.
  // Owner hints are advisory only and are accepted solely when the claimed
  // owner truly contains a materialized block at the observed address.
  for (const auto& observation : effective.adaptive_observations) {
    const DiscoveredFunction* owner = nullptr;
    if (observation.owner_hint) {
      const auto hinted = std::find_if(report.functions.begin(), report.functions.end(), [&](const auto& fn) {
        return fn.guest_start == *observation.owner_hint && owns_address(fn, observation.address) &&
               has_block_entry(fn, observation.address);
      });
      if (hinted != report.functions.end()) owner = &*hinted;
    }
    if (!owner) owner = find_owner(observation.address);
    if (!owner || observation.address == owner->guest_start || !has_block_entry(*owner, observation.address))
      continue;
    GuestEntryPoint entry{};
    entry.address = observation.address;
    entry.owner_function = owner->guest_start;
    entry.block = observation.address;
    entry.kind = GuestEntryKind::AlternateBlock;
    entry.sources = {DiscoverySource::RuntimeObservation};
    entry.confidence = discovery_source_base_confidence(DiscoverySource::RuntimeObservation);
    add_entry(std::move(entry));
  }

  for (const auto& helper : expanded_runtime_helpers) {
    GuestEntryPoint entry{};
    entry.address = helper.address;
    entry.owner_function = helper.address;
    entry.block = helper.address;
    entry.kind = GuestEntryKind::RuntimeHelper;
    entry.sources = {DiscoverySource::ModuleHint};
    entry.confidence = 100u;
    add_entry(std::move(entry));
  }
  std::sort(report.entries.begin(), report.entries.end(),
            [](const auto& a, const auto& b) { return a.address < b.address; });
}

void diagnose_function_ownership(AnalysisReport& report, const ExecutableRangeIndex& range_index) {
  const auto find_owner = [&report](GuestAddress target) {
    return find_strongest_owner(report.functions, target);
  };

  // Remaining overlap warnings are now genuinely competing semantic owners,
  // not weak alternate-entry candidates that Xenon already reconciled.
  for (const auto& function : report.functions) {
    for (const auto& other : report.functions) {
      if (&function == &other) continue;
      if (function.guest_start < other.guest_end && other.guest_start < function.guest_end) {
        report.warnings.push_back("overlapping strong functions at 0x" + hex_string(function.guest_start) +
                                  " and 0x" + hex_string(other.guest_start));
        break;
      }
    }
  }

  // Diagnose direct/known-indirect executable edges from their ACTUAL branch
  // instruction site. An owned local target is fine. A cross-owner interior
  // target must have an alternate entry; otherwise codegen would have no legal
  // NativeCompiledEntry for the transfer and analysis must say so precisely.
  for (const auto& function : report.functions) {
    for (const auto& branch : function.branches) {
      if (branch.linked) continue;
      if (!executable_section(range_index, branch.target)) {
        report.unresolved.push_back(
            {branch.site, branch.target, "branch-target", "target is not executable"});
        continue;
      }
      const auto* owner = find_owner(branch.target);
      if (!owner) {
        report.unresolved.push_back(
            {branch.site, branch.target, "branch-into-unknown-code",
             "executable target has no discovered function/chunk owner"});
        continue;
      }
      if (owner->guest_start == function.guest_start || branch.target == owner->guest_start) continue;
      const bool dispatchable = std::any_of(report.entries.begin(), report.entries.end(), [&](const auto& entry) {
        return entry.address == branch.target && entry.owner_function == owner->guest_start;
      });
      if (!dispatchable) {
        report.unresolved.push_back(
            {branch.site, branch.target, "branch-into-nonentry-block",
             "target is owned by function 0x" + hex_string(owner->guest_start) +
                 " but is not a materialized alternate entry block"});
      }
    }
  }
}

void deduplicate_unresolved(AnalysisReport& report) {
  // Part 4 (over-discovery fix): deduplicate final unresolved diagnostics by
  // (kind, address, target, detail) identity. The same physical site can
  // legitimately surface more than once while it is being produced above -
  // e.g. the exact same unresolved branch target referenced by several
  // different branch instructions within one function's `branch_references`
  // - but that is repeated emission of the SAME finding, not multiple
  // distinct findings, and must not inflate the top-level counters computed
  // below. A sort+unique on the full identity tuple (rather than an
  // unordered hash set) keeps this deterministic and identical between
  // jobs=1 and jobs=N, matching every other pass in this function.
  std::sort(report.unresolved.begin(), report.unresolved.end(),
           [](const UnresolvedReference& a, const UnresolvedReference& b) {
             if (a.kind != b.kind) return a.kind < b.kind;
             if (a.address != b.address) return a.address < b.address;
             if (a.target != b.target) return a.target < b.target;
             return a.detail < b.detail;
           });
  report.unresolved.erase(
      std::unique(report.unresolved.begin(), report.unresolved.end(),
                 [](const UnresolvedReference& a, const UnresolvedReference& b) {
                   return a.kind == b.kind && a.address == b.address && a.target == b.target &&
                          a.detail == b.detail;
                 }),
      report.unresolved.end());
}

}  // namespace xenon::recomp::detail
