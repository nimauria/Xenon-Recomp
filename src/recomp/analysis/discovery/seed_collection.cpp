#include <algorithm>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {
namespace {

bool stronger_seed_source(DiscoverySource lhs, DiscoverySource rhs) {
  const bool lhs_semantic = source_is_semantic_boundary(lhs);
  const bool rhs_semantic = source_is_semantic_boundary(rhs);
  if (lhs_semantic != rhs_semantic) return lhs_semantic;
  const auto lhs_authority = authority_for_sources({lhs});
  const auto rhs_authority = authority_for_sources({rhs});
  if (lhs_authority != rhs_authority)
    return static_cast<unsigned>(lhs_authority) > static_cast<unsigned>(rhs_authority);
  return discovery_source_base_confidence(lhs) > discovery_source_base_confidence(rhs);
}

}  // namespace

bool in_non_code_hint_region(const analysis::AnalysisHintSetV2* hint_set_v2, GuestAddress address) {
  if (!hint_set_v2) return false;
  return std::any_of(hint_set_v2->regions.begin(), hint_set_v2->regions.end(),
                     [address](const auto& region) {
                       return region.kind != analysis::RegionKind::CodeOverride &&
                              address >= region.start && address < region.end;
                     });
}

void SeedSet::add(GuestAddress address, DiscoverySource source) {
  add_source(evidence[address], source);
  const auto it = sources.find(address);
  if (it == sources.end()) {
    sources.emplace(address, source);
  } else if (stronger_seed_source(source, it->second)) {
    it->second = source;
  }
}

void SeedSet::erase(GuestAddress address) {
  sources.erase(address);
  evidence.erase(address);
}

void SeedSet::erase_range(GuestAddress begin, GuestAddress end) {
  for (auto it = sources.lower_bound(begin); it != sources.end() && it->first < end;)
    it = sources.erase(it);
  for (auto it = evidence.lower_bound(begin);
       it != evidence.end() && it->first < end;)
    it = evidence.erase(it);
}

void collect_seeds(const DriverOptions& effective, AnalysisReport& report,
                   const ExecutableRangeIndex& range_index,
                   const std::string& current_effective_image_hash, SeedSet& seed_set,
                   SeedStatistics& statistics) {
  const auto& hint_set_v2 = report.hint_set_v2;
  const auto& seeds = seed_set.sources;
  auto& seed_evidence = seed_set.evidence;
  auto& known_names = seed_set.known_names;
  const auto add_seed = [&seed_set](GuestAddress address, DiscoverySource source) {
    seed_set.add(address, source);
  };
  const auto erase_seed = [&seed_set](GuestAddress address) { seed_set.erase(address); };
  const auto erase_seed_range = [&seed_set](GuestAddress begin, GuestAddress end) {
    seed_set.erase_range(begin, end);
  };
  auto& pointer_tables_discovered = statistics.pointer_tables_discovered;
  auto& pointer_table_targets_discovered = statistics.pointer_table_targets_discovered;
  auto& adaptive_observations_consumed = statistics.adaptive_observations_consumed;
  auto& adaptive_observations_rejected_revision = statistics.adaptive_observations_rejected_revision;
  auto& adaptive_observations_rejected_invalid = statistics.adaptive_observations_rejected_invalid;
  auto& knowledge_seed_candidates = statistics.knowledge_seed_candidates;

  add_seed(report.image.entry_point, DiscoverySource::EntryPoint);
  for (const auto& export_entry : report.image.exports)
    add_seed(export_entry.address, DiscoverySource::Export);
  for (const auto& hint : effective.hints)
    for (const auto address : hint.function_boundaries)
      add_seed(address, DiscoverySource::ModuleHint);
  for (const auto& hint : effective.hints)
    for (const auto& symbol : hint.known_symbols) {
      add_seed(symbol.address, DiscoverySource::ModuleHint);
      known_names[symbol.address] = symbol.name;
    }
  for (const auto& hint : effective.hints)
    for (const auto address : hint.data_regions)
      erase_seed(address);
  for (const auto& hint : effective.hints)
    for (const auto address : hint.ignored_regions)
      erase_seed(address);
  for (const auto& metadata : report.image.function_metadata)
    if (metadata.valid) add_seed(metadata.begin, DiscoverySource::UnwindMetadata);
  // Part 2 (Recomp Analysis V3): a XEX TLS directory callback is a real,
  // generic loader-exposed callable address - the loader itself invokes it
  // on thread attach/detach, exactly as reachable as the entry point, just
  // never reached via any direct call/branch a decode-time scan would ever
  // find. No title ever needs a hint to tell Xenon this address is code.
  if (report.image.tls && report.image.tls->callback_address != 0)
    add_seed(report.image.tls->callback_address, DiscoverySource::TlsCallback);

  // Schema V2 seeding (Part 1.11: the driver actually uses this metadata
  // during function discovery, not merely parses/stores it).
  if (hint_set_v2) {
    for (const auto& function_hint : hint_set_v2->functions) {
      add_seed(function_hint.address, DiscoverySource::ModuleHint);
      if (!function_hint.name.empty()) known_names[function_hint.address] = function_hint.name;
    }
    // FunctionChunk ranges are owned by their declared parent and are compiled
    // with that parent below. They are intentionally not seeded as unrelated
    // top-level functions; explicit independent FunctionHint entries still
    // seed normally.
    // Known indirect call/branch targets and explicit/decoded switch targets
    // are reachable entry facts. Calls are semantic-callable evidence; branch
    // and switch targets are intentionally weaker and can later collapse into
    // alternate entries owned by another compiled region.
    for (const auto& call : hint_set_v2->indirect_calls)
      for (const auto target : call.targets) add_seed(target, DiscoverySource::ResolvedIndirectCall);
    for (const auto& branch : hint_set_v2->indirect_branches)
      for (const auto target : branch.targets) add_seed(target, DiscoverySource::ControlFlowHint);
    for (const auto& table : hint_set_v2->switches)
      for (const auto target : resolve_switch_targets(table, range_index, report.warnings))
        add_seed(target, DiscoverySource::ControlFlowHint);
    // Native replacement addresses are real, dispatchable entry points too
    // (Part 1.10/1.11) - seeded so a `bl` to one is discoverable even when
    // the hint set never separately lists it as a FunctionHint.
    for (const auto& replacement : hint_set_v2->native_replacements)
      add_seed(replacement.guest_address, DiscoverySource::ModuleHint);
    // Data/ignored/invalid-instruction regions (Part 1.8) must never seed a
    // function, regardless of source. CodeOverride regions remain eligible.
    for (const auto& region : hint_set_v2->regions) {
      if (region.kind == analysis::RegionKind::CodeOverride) continue;
      erase_seed_range(region.start, region.end);
    }
  }

  // Region + Entry adaptive seeding. These inputs say only that an address is
  // a real executable entry observed/referenced somewhere; they intentionally
  // do not become authoritative semantic function boundaries. If later CFG
  // ownership proves the address is an internal block, post-analysis
  // reconciliation absorbs the weak candidate and keeps only an alternate
  // dispatch entry.
  const auto blocked_by_non_code_hint = [&](GuestAddress address) {
    return in_non_code_hint_region(hint_set_v2 ? &*hint_set_v2 : nullptr, address);
  };
  const auto is_import_thunk = [&](GuestAddress address) {
    return find_callable_import_thunk(report.image, address) != nullptr;
  };

  if (effective.scan_static_pointer_tables) {
    const auto pointer_scan = scan_static_pointer_tables(report.image, range_index, seeds);
    pointer_tables_discovered = pointer_scan.tables;
    for (const auto target : pointer_scan.targets) {
      if (blocked_by_non_code_hint(target)) continue;
      auto& evidence = seed_evidence[target];
      const bool already_had_pointer_evidence =
          std::find(evidence.begin(), evidence.end(), DiscoverySource::PointerTable) != evidence.end();
      add_seed(target, DiscoverySource::PointerTable);
      if (!already_had_pointer_evidence) ++pointer_table_targets_discovered;
    }
  }

  for (const auto& observation : effective.adaptive_observations) {
    if (!observation.image_hash.empty() &&
        observation.image_hash != current_effective_image_hash) {
      ++adaptive_observations_rejected_revision;
      report.warnings.push_back(
          "adaptive observation at 0x" + hex_string(observation.address) +
          " ignored because imageHash belongs to a different executable revision");
      continue;
    }
    if (!ExecutableRangeIndex::is_aligned_ppc_address(observation.address) ||
        !range_index.is_executable_address(observation.address) ||
        blocked_by_non_code_hint(observation.address) ||
        is_import_thunk(observation.address)) {
      ++adaptive_observations_rejected_invalid;
      report.warnings.push_back(
          "adaptive observation at 0x" + hex_string(observation.address) +
          (is_import_thunk(observation.address)
               ? " ignored because the target is an import thunk, not guest function evidence"
               : " ignored because the target is not aligned executable code"));
      continue;
    }
    ++adaptive_observations_consumed;
    add_seed(observation.address, DiscoverySource::RuntimeObservation);
    // A runtime-observed CALL target is stronger than a generic executed or
    // branch entry: the guest actually used the address as a callable ABI
    // boundary. Preserve both facts so authority/confidence can distinguish it
    // from a mere mid-function dispatch entry.
    if (observation.kind == AdaptiveObservationKind::IndirectCallTarget)
      add_seed(observation.address, DiscoverySource::ResolvedIndirectCall);
  }

  // Gen 9 cross-revision candidate nomination. The cheap entry-anchor hash is
  // deliberately only a seed: after full analysis the candidate must also
  // pass the multi-dimensional function matcher or a knowledge-only seed is
  // discarded. This gives moved functions a way back into the frontier
  // without turning a short block hash into semantic truth.
  if (effective.enable_knowledge_seeding && !effective.knowledge_records.empty()) {
    const auto candidates = find_knowledge_seed_candidates(
        report.image, effective.knowledge_records, effective.knowledge_match_min_score);
    for (const auto address : candidates) {
      if (blocked_by_non_code_hint(address) || is_import_thunk(address)) continue;
      const bool already_seeded = seed_evidence.contains(address);
      add_seed(address, DiscoverySource::KnowledgeMatch);
      if (!already_seeded) ++knowledge_seed_candidates;
    }
  }
}

}  // namespace xenon::recomp::detail
