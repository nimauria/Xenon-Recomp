#include <algorithm>
#include <sstream>

#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp {
namespace {
using detail::hash_name;
using detail::json_escape;
using detail::kAnalysisEngineRevision;
using detail::kAnalysisReportSchemaVersion;
}  // namespace

std::string format_report_json(const AnalysisReport& report) {
  std::ostringstream out;
  const auto& diagnostics = report.diagnostics;
  out << "{\n  \"report_schema_version\": " << kAnalysisReportSchemaVersion
      << ",\n  \"analysis_engine_revision\": " << kAnalysisEngineRevision
      << ",\n  \"entry_point\": " << report.image.entry_point
      << ",\n  \"configuration_hash\": " << report.configuration_hash;
  const auto json_compiled_regions = static_cast<std::size_t>(std::count_if(
      report.functions.begin(), report.functions.end(),
      [](const auto& fn) { return fn.compiled && !fn.native_replacement; }));
  const auto json_alternate_entries = static_cast<std::size_t>(std::count_if(
      report.entries.begin(), report.entries.end(),
      [](const auto& entry) { return entry.kind == GuestEntryKind::AlternateBlock; }));
  out << ",\n  \"summary\": {\"semantic_functions\": " << report.functions.size()
      << ", \"compiled_regions\": " << json_compiled_regions
      << ", \"guest_entries\": " << report.entries.size()
      << ", \"alternate_entries\": " << json_alternate_entries
      << ", \"unresolved\": " << report.unresolved.size() << "}"
      << ",\n  \"diagnostics\": {"
      << "\"auto_discovered_functions\": " << diagnostics.auto_discovered_functions
      << ", \"hinted_functions\": " << diagnostics.hinted_functions
      << ", \"manual_chunks\": " << diagnostics.manual_chunks
      << ", \"switch_tables_resolved\": " << diagnostics.switch_tables_resolved
      << ", \"known_indirect_calls\": " << diagnostics.known_indirect_calls
      << ", \"known_indirect_branches\": " << diagnostics.known_indirect_branches
      << ", \"unresolved_indirect_sites\": " << diagnostics.unresolved_indirect_sites
      << ", \"native_replacements_applied\": " << diagnostics.native_replacements_applied
      << ", \"native_replacements_unsupported\": " << diagnostics.native_replacements_unsupported
      << ", \"import_thunks_recognized\": " << diagnostics.import_thunks_recognized
      << ", \"data_or_ignored_regions\": " << diagnostics.data_or_ignored_regions
      << ", \"analysis_errors\": " << diagnostics.analysis_errors
      << ", \"register_save_helpers\": " << diagnostics.register_save_helpers
      << ", \"register_restore_helpers\": " << diagnostics.register_restore_helpers
      << ", \"instruction_patterns_loaded\": " << diagnostics.instruction_patterns_loaded
      << ", \"instruction_pattern_matches\": " << diagnostics.instruction_pattern_matches
      // Performance/parallelism diagnostics (Part 19) - purely additive
      // fields; a consumer that only reads the counters above is unaffected.
      << ", \"candidate_functions_total\": " << diagnostics.candidate_functions_total
      << ", \"function_candidates_rejected_nonexec\": " << diagnostics.function_candidates_rejected_nonexec
      << ", \"function_candidates_rejected_unaligned\": " << diagnostics.function_candidates_rejected_unaligned
      << ", \"analysis_waves\": " << diagnostics.analysis_waves
      << ", \"analysis_workers\": " << diagnostics.analysis_workers
      << ", \"functions_analyzed\": " << diagnostics.functions_analyzed
      << ", \"functions_compiled\": " << diagnostics.functions_compiled
      << ", \"invalid_ppc_sites\": " << diagnostics.invalid_ppc_sites
      << ", \"unresolved_indirect_calls\": " << diagnostics.unresolved_indirect_calls
      << ", \"unresolved_indirect_branches\": " << diagnostics.unresolved_indirect_branches
      << ", \"resolved_indirect_calls\": " << diagnostics.resolved_indirect_calls
      << ", \"resolved_indirect_branches\": " << diagnostics.resolved_indirect_branches
      << ", \"analysis_duration_ms\": " << diagnostics.analysis_duration_ms
      << ", \"codegen_duration_ms\": " << diagnostics.codegen_duration_ms
      // Recomp Analysis V3 additions - purely additive.
      << ", \"resolved_indirect_via_dataflow\": " << diagnostics.resolved_indirect_via_dataflow
      << ", \"resolved_indirect_via_backward_slice\": " << diagnostics.resolved_indirect_via_backward_slice
      << ", \"resolved_indirect_via_readonly_table\": " << diagnostics.resolved_indirect_via_readonly_table
      << ", \"return_fixed_point_iterations\": " << diagnostics.return_fixed_point_iterations
      << ", \"inferred_no_return_functions\": " << diagnostics.inferred_no_return_functions
      << ", \"inferred_may_return_functions\": " << diagnostics.inferred_may_return_functions
      << ", \"candidates_from_tls_callbacks\": " << diagnostics.candidates_from_tls_callbacks
      << ", \"unsupported_ppc_sites\": " << diagnostics.unsupported_ppc_sites
      << ", \"unsupported_vmx_sites\": " << diagnostics.unsupported_vmx_sites
      // Tail-call over-discovery fix additions - purely additive; see
      // AnalysisDiagnostics's doc comments for how these three reconcile.
      << ", \"resolved_indirect_via_jump_table\": " << diagnostics.resolved_indirect_via_jump_table
      << ", \"functions_with_resolved_indirect_provenance\": "
      << diagnostics.functions_with_resolved_indirect_provenance
      // Codegen ownership/dedup diagnostics (generated-code deduplication /
      // shard ownership fix) - purely additive.
      << ", \"codegen_duplicate_addresses_merged\": " << diagnostics.codegen_duplicate_addresses_merged
      << ", \"codegen_input_functions\": " << diagnostics.codegen_input_functions
      << ", \"codegen_unique_functions\": " << diagnostics.codegen_unique_functions
      << ", \"codegen_duplicate_symbols_rejected\": " << diagnostics.codegen_duplicate_symbols_rejected
      << ", \"codegen_shards\": " << diagnostics.codegen_shards
      << ", \"codegen_max_functions_per_shard\": " << diagnostics.codegen_max_functions_per_shard
      << ", \"codegen_max_shard_bytes\": " << diagnostics.codegen_max_shard_bytes
      << ", \"pointer_tables_discovered\": " << diagnostics.pointer_tables_discovered
      << ", \"pointer_table_targets_discovered\": " << diagnostics.pointer_table_targets_discovered
      << ", \"adaptive_observations_consumed\": " << diagnostics.adaptive_observations_consumed
      << ", \"alternate_entries_materialized\": " << diagnostics.alternate_entries_materialized
      << ", \"weak_functions_absorbed\": " << diagnostics.weak_functions_absorbed
      << ", \"orphan_entries_recovered\": " << diagnostics.orphan_entries_recovered
      << ", \"gap_functions_recovered\": " << diagnostics.gap_functions_recovered
      << ", \"adaptive_observations_rejected_revision\": " << diagnostics.adaptive_observations_rejected_revision
      << ", \"adaptive_observations_rejected_invalid\": " << diagnostics.adaptive_observations_rejected_invalid
      << ", \"entry_integrity_checks\": " << diagnostics.entry_integrity_checks
      << ", \"entry_integrity_failures\": " << diagnostics.entry_integrity_failures
      << ", \"knowledge_records_loaded\": " << diagnostics.knowledge_records_loaded
      << ", \"knowledge_functions_fingerprinted\": " << diagnostics.knowledge_functions_fingerprinted
      << ", \"knowledge_matches_considered\": " << diagnostics.knowledge_matches_considered
      << ", \"knowledge_matches_accepted\": " << diagnostics.knowledge_matches_accepted
      << ", \"knowledge_cross_revision_matches\": " << diagnostics.knowledge_cross_revision_matches
      << ", \"knowledge_seed_candidates\": " << diagnostics.knowledge_seed_candidates
      << "},\n  \"functions\": [\n";
  for (std::size_t i = 0; i < report.functions.size(); ++i) {
    const auto& function = report.functions[i];
    out << "    {\"start\": " << function.guest_start
        << ", \"end\": " << function.guest_end
        << ", \"name\": \"" << json_escape(function.name)
        << "\", \"confidence\": " << function.confidence
        << ", \"authority\": \"" << function_authority_name(function.authority) << "\""
        << ", \"return_behavior\": \"" << return_behavior_name(function.return_behavior) << "\""
        << ", \"return_behavior_explicit\": " << (function.return_behavior_explicit ? "true" : "false")
        << ", \"has_explicit_return\": " << (function.has_explicit_return ? "true" : "false")
        << ", \"compiled\": " << (function.compiled ? "true" : "false");
    // Provenance (Part 9): every DiscoverySource that contributed to this
    // function being found - lets a real-title investigation answer "why
    // does Xenon think this address is a function" without re-running
    // analysis under a debugger.
    out << ", \"sources\": [";
    for (std::size_t s = 0; s < function.sources.size(); ++s) {
      out << "\"" << discovery_source_name(function.sources[s]) << "\"";
      if (s + 1 != function.sources.size()) out << ", ";
    }
    out << "]";
    // Exact outbound control-flow provenance. Unlike the legacy
    // branch_references summary this records the instruction site and whether
    // the edge is terminal/indirect, which is essential when diagnosing
    // ownership repair and deciding whether a learned target is a block entry
    // or a semantic callable boundary.
    out << ", \"branches\": [";
    for (std::size_t b = 0; b < function.branches.size(); ++b) {
      const auto& branch = function.branches[b];
      out << "{\"site\": " << branch.site
          << ", \"target\": " << branch.target
          << ", \"terminal\": " << (branch.terminal ? "true" : "false")
          << ", \"indirect\": " << (branch.indirect ? "true" : "false")
          << ", \"linked\": " << (branch.linked ? "true" : "false")
          << ", \"conditional\": " << (branch.conditional ? "true" : "false")
          << ", \"fallthrough\": " << (branch.fallthrough ? "true" : "false") << "}";
      if (b + 1 != function.branches.size()) out << ", ";
    }
    out << "]";
    if (function.fingerprint.valid()) {
      const auto& fp = function.fingerprint;
      out << ", \"fingerprint\": {\"version\": " << fp.version
          << ", \"instruction_shape\": \"" << hash_name(fp.instruction_shape_hash) << "\""
          << ", \"entry_anchor\": \"" << hash_name(fp.entry_anchor_hash) << "\""
          << ", \"cfg_shape\": \"" << hash_name(fp.cfg_shape_hash) << "\""
          << ", \"constant_shape\": \"" << hash_name(fp.constant_shape_hash) << "\""
          << ", \"call_neighborhood\": \"" << hash_name(fp.call_neighborhood_hash) << "\""
          << ", \"instruction_count\": " << fp.instruction_count
          << ", \"entry_anchor_instructions\": " << fp.entry_anchor_instructions
          << ", \"block_count\": " << fp.block_count
          << ", \"external_call_count\": " << fp.external_call_count
          << ", \"byte_size\": " << fp.byte_size << "}";
    }
    out << ", \"knowledge_matches\": [";
    for (std::size_t k = 0; k < function.knowledge_matches.size(); ++k) {
      const auto& match = function.knowledge_matches[k];
      out << "{\"id\": \"" << json_escape(match.record_id)
          << "\", \"label\": \"" << json_escape(match.label)
          << "\", \"family\": \"" << json_escape(match.family)
          << "\", \"kind\": \"" << knowledge_kind_name(match.kind)
          << "\", \"score\": " << match.score
          << ", \"cross_revision\": " << (match.cross_revision ? "true" : "false")
          << ", \"accepted\": " << (match.accepted ? "true" : "false") << "}";
      if (k + 1 != function.knowledge_matches.size()) out << ", ";
    }
    out << "]";
    if (function.native_replacement)
      out << ", \"native_replacement\": \""
          << analysis::native_replacement_kind_name(*function.native_replacement) << "\"";
    if (function.import_thunk)
      out << ", \"import_thunk\": {\"module\": \"" << json_escape(function.import_thunk->module)
          << "\", \"symbol\": \"" << json_escape(function.import_thunk->symbol)
          << "\", \"ordinal\": " << function.import_thunk->ordinal << "}";
    if (!function.error.empty()) out << ", \"error\": \"" << json_escape(function.error) << "\"";
    out << "}";
    if (i + 1 != report.functions.size()) out << ',';
    out << '\n';
  }
  out << "  ],\n  \"entries\": [\n";
  for (std::size_t i = 0; i < report.entries.size(); ++i) {
    const auto& entry = report.entries[i];
    out << "    {\"address\": " << entry.address
        << ", \"owner\": " << entry.owner_function
        << ", \"block\": " << entry.block
        << ", \"kind\": \"" << guest_entry_kind_name(entry.kind) << "\""
        << ", \"confidence\": " << entry.confidence
        << ", \"sources\": [";
    for (std::size_t source = 0; source < entry.sources.size(); ++source) {
      out << "\"" << discovery_source_name(entry.sources[source]) << "\"";
      if (source + 1 != entry.sources.size()) out << ", ";
    }
    out << "]}";
    if (i + 1 != report.entries.size()) out << ',';
    out << '\n';
  }
  out << "  ],\n  \"unresolved\": [\n";
  for (std::size_t i = 0; i < report.unresolved.size(); ++i) {
    const auto& item = report.unresolved[i];
    out << "    {\"address\": " << item.address << ", \"target\": " << item.target
        << ", \"kind\": \"" << json_escape(item.kind) << "\", \"detail\": \""
        << json_escape(item.detail) << "\"}";
    if (i + 1 != report.unresolved.size()) out << ',';
    out << '\n';
  }
  out << "  ],\n  \"cpu_coverage\": {\"instructions_decoded\": " << report.cpu_coverage.instructions_decoded
      << ", \"distinct_mnemonics\": " << report.cpu_coverage.distinct_mnemonics
      << ", \"undecodable_words\": " << report.cpu_coverage.undecodable_words
      << ", \"aot_gaps\": " << report.cpu_coverage.aot_gap_count()
      << ", \"fallback_gaps\": " << report.cpu_coverage.fallback_gap_count() << ", \"gaps\": [";
  for (std::size_t i = 0; i < report.cpu_coverage.gaps.size(); ++i) {
    const auto& gap = report.cpu_coverage.gaps[i];
    out << (i == 0 ? "" : ", ") << "{\"mnemonic\": \"" << json_escape(gap.mnemonic)
        << "\", \"count\": " << gap.count << ", \"first_address\": " << gap.first_address
        << ", \"example_word\": " << gap.example_word
        << ", \"aot_supported\": " << (gap.aot_supported ? "true" : "false")
        << ", \"fallback_supported\": " << (gap.fallback_supported ? "true" : "false") << "}";
  }
  out << "]}\n}\n";
  return out.str();
}

}  // namespace xenon::recomp
