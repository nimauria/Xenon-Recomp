#include <algorithm>

#include "recomp/analysis/analysis_phases.hpp"
#include "xenon/recomp/native_replacements.hpp"

namespace xenon::recomp::detail {

void populate_analysis_diagnostics(AnalysisReport& report, const AnalysisRunStatistics& statistics) {
  const auto analysis_start = statistics.analysis_start;
  const auto worker_count = statistics.worker_count;
  const auto candidate_functions_total = statistics.candidate_functions_total;
  const auto rejected_nonexec = statistics.rejected_nonexec;
  const auto rejected_unaligned = statistics.rejected_unaligned;
  const auto wave_count = statistics.wave_count;
  const auto functions_analyzed = statistics.functions_analyzed;
  const auto instruction_pattern_match_count = statistics.instruction_pattern_match_count;
  const auto resolved_indirect_via_dataflow = statistics.resolved_indirect_via_dataflow;
  const auto resolved_indirect_via_backward_slice = statistics.resolved_indirect_via_backward_slice;
  const auto resolved_indirect_via_readonly_table = statistics.resolved_indirect_via_readonly_table;
  const auto resolved_indirect_via_jump_table = statistics.resolved_indirect_via_jump_table;
  const auto codegen_duplicate_addresses_merged = statistics.codegen_duplicate_addresses_merged;
  const auto weak_functions_absorbed = statistics.weak_functions_absorbed;
  const auto orphan_entries_recovered = statistics.orphan_entries_recovered;
  const auto gap_functions_recovered = statistics.gap_functions_recovered;
  const auto knowledge_records_loaded = statistics.knowledge_records_loaded;
  const auto& return_inference = statistics.return_inference;
  const auto pointer_tables_discovered = statistics.seeds.pointer_tables_discovered;
  const auto pointer_table_targets_discovered = statistics.seeds.pointer_table_targets_discovered;
  const auto adaptive_observations_consumed = statistics.seeds.adaptive_observations_consumed;
  const auto adaptive_observations_rejected_revision = statistics.seeds.adaptive_observations_rejected_revision;
  const auto adaptive_observations_rejected_invalid = statistics.seeds.adaptive_observations_rejected_invalid;
  const auto knowledge_seed_candidates = statistics.seeds.knowledge_seed_candidates;
  const auto knowledge_functions_fingerprinted = statistics.knowledge.functions_fingerprinted;
  const auto knowledge_matches_considered = statistics.knowledge.matches_considered;
  const auto knowledge_matches_accepted = statistics.knowledge.matches_accepted;
  const auto knowledge_cross_revision_matches = statistics.knowledge.cross_revision_matches;

  // Analysis diagnostics (Part 1.12/6.2) - computed once, after every
  // function/unresolved entry above is final.
  auto& diagnostics = report.diagnostics;
  diagnostics = {};
  for (const auto& function : report.functions) {
    if (function.native_replacement) {
      ++diagnostics.native_replacements_applied;
      continue;
    }
    if (function.import_thunk) {
      // Deliberately uncompiled (see analyze_function_candidate()'s
      // import-thunk short-circuit) - not an analysis error, so this skips
      // the hinted/auto_discovered and analysis_errors accounting below
      // exactly like the native_replacement case above.
      ++diagnostics.import_thunks_recognized;
      continue;
    }
    if (source_contains(function, DiscoverySource::ModuleHint)) ++diagnostics.hinted_functions;
    else ++diagnostics.auto_discovered_functions;
    if (!function.compiled) ++diagnostics.analysis_errors;
    if (source_contains(function, DiscoverySource::TlsCallback)) ++diagnostics.candidates_from_tls_callbacks;
    // Part 5 (resolved-indirect reconciliation): distinct-function count,
    // directly comparable against resolved_indirect_via_dataflow/
    // _via_jump_table below (see AnalysisDiagnostics's doc comment).
    if (source_contains(function, DiscoverySource::ResolvedIndirect) ||
        source_contains(function, DiscoverySource::ResolvedIndirectCall) ||
        source_contains(function, DiscoverySource::ResolvedIndirectBranch))
      ++diagnostics.functions_with_resolved_indirect_provenance;
  }
  if (report.hint_set_v2) {
    diagnostics.manual_chunks = report.hint_set_v2->chunks.size();
    diagnostics.switch_tables_resolved = report.hint_set_v2->switches.size();
    diagnostics.known_indirect_calls = report.hint_set_v2->indirect_calls.size();
    diagnostics.known_indirect_branches = report.hint_set_v2->indirect_branches.size();
    diagnostics.data_or_ignored_regions = report.hint_set_v2->regions.size();
    for (const auto& replacement : report.hint_set_v2->native_replacements)
      if (native_replacements::entry_for(replacement.kind) == nullptr)
        ++diagnostics.native_replacements_unsupported;
    for (const auto& helper : report.hint_set_v2->runtime_helpers) {
      if (!analysis::runtime_helper_kind_is_register_range(helper.kind)) continue;
      if (analysis::runtime_helper_kind_is_save(helper.kind)) ++diagnostics.register_save_helpers;
      else ++diagnostics.register_restore_helpers;
    }
    diagnostics.instruction_patterns_loaded = report.hint_set_v2->instruction_patterns.size();
  }
  diagnostics.instruction_pattern_matches = instruction_pattern_match_count;
  for (const auto& item : report.unresolved) {
    if (item.kind == "indirect-call" || item.kind == "indirect-branch")
      ++diagnostics.unresolved_indirect_sites;
    if (item.kind == "indirect-call") ++diagnostics.unresolved_indirect_calls;
    if (item.kind == "indirect-branch") ++diagnostics.unresolved_indirect_branches;
    if (item.kind == "invalid-ppc") ++diagnostics.invalid_ppc_sites;
  }
  if (report.hint_set_v2) {
    for (const auto& call : report.hint_set_v2->indirect_calls)
      if (!call.targets.empty()) ++diagnostics.resolved_indirect_calls;
    for (const auto& branch : report.hint_set_v2->indirect_branches)
      if (!branch.targets.empty()) ++diagnostics.resolved_indirect_branches;
    for (const auto& table : report.hint_set_v2->switches)
      if (!table.explicit_targets.empty() || (table.table_address && table.entry_count))
        ++diagnostics.resolved_indirect_branches;
  }

  // Performance/parallelism diagnostics (Part 19) - the wave engine's local
  // counters, folded in here (after the `diagnostics = {}` reset above)
  // rather than written directly during the loop, exactly like
  // `instruction_pattern_match_count` already was before this pass.
  diagnostics.codegen_duplicate_addresses_merged = codegen_duplicate_addresses_merged;
  diagnostics.candidate_functions_total = candidate_functions_total;
  diagnostics.function_candidates_rejected_nonexec = rejected_nonexec;
  diagnostics.function_candidates_rejected_unaligned = rejected_unaligned;
  diagnostics.analysis_waves = wave_count;
  diagnostics.analysis_workers = worker_count;
  diagnostics.functions_analyzed = functions_analyzed;
  diagnostics.functions_compiled =
      static_cast<std::size_t>(std::count_if(report.functions.begin(), report.functions.end(),
                                             [](const auto& function) { return function.compiled; }));
  diagnostics.resolved_indirect_via_dataflow = resolved_indirect_via_dataflow;
  diagnostics.resolved_indirect_via_backward_slice = resolved_indirect_via_backward_slice;
  diagnostics.resolved_indirect_via_readonly_table = resolved_indirect_via_readonly_table;
  diagnostics.resolved_indirect_via_jump_table = resolved_indirect_via_jump_table;
  diagnostics.return_fixed_point_iterations = return_inference.iterations;
  diagnostics.inferred_no_return_functions = return_inference.no_return;
  diagnostics.inferred_may_return_functions = return_inference.may_return;
  diagnostics.pointer_tables_discovered = pointer_tables_discovered;
  diagnostics.pointer_table_targets_discovered = pointer_table_targets_discovered;
  diagnostics.adaptive_observations_consumed = adaptive_observations_consumed;
  diagnostics.adaptive_observations_rejected_revision = adaptive_observations_rejected_revision;
  diagnostics.adaptive_observations_rejected_invalid = adaptive_observations_rejected_invalid;
  diagnostics.alternate_entries_materialized = static_cast<std::size_t>(std::count_if(
      report.entries.begin(), report.entries.end(),
      [](const auto& entry) { return entry.kind == GuestEntryKind::AlternateBlock; }));
  diagnostics.weak_functions_absorbed = weak_functions_absorbed;
  diagnostics.orphan_entries_recovered = orphan_entries_recovered;
  diagnostics.gap_functions_recovered = gap_functions_recovered;
  diagnostics.knowledge_records_loaded = knowledge_records_loaded;
  diagnostics.knowledge_functions_fingerprinted = knowledge_functions_fingerprinted;
  diagnostics.knowledge_matches_considered = knowledge_matches_considered;
  diagnostics.knowledge_matches_accepted = knowledge_matches_accepted;
  diagnostics.knowledge_cross_revision_matches = knowledge_cross_revision_matches;
  diagnostics.knowledge_seed_candidates = knowledge_seed_candidates;
  for (const auto& item : report.unresolved) {
    if (item.kind == "unsupported-ppc") ++diagnostics.unsupported_ppc_sites;
    if (item.kind == "unsupported-vmx") ++diagnostics.unsupported_vmx_sites;
  }
  diagnostics.analysis_duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - analysis_start)
          .count();
}

}  // namespace xenon::recomp::detail
