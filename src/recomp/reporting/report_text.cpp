#include <algorithm>
#include <sstream>

#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp {
namespace {
using detail::hash_name;
}  // namespace

std::string format_report(const AnalysisReport& report) {
  std::ostringstream out;
  const auto compiled_regions = static_cast<std::size_t>(std::count_if(
      report.functions.begin(), report.functions.end(),
      [](const auto& fn) { return fn.compiled && !fn.native_replacement; }));
  const auto alternate_entries = static_cast<std::size_t>(std::count_if(
      report.entries.begin(), report.entries.end(),
      [](const auto& entry) { return entry.kind == GuestEntryKind::AlternateBlock; }));
  out << "entry: 0x" << std::hex << report.image.entry_point
      << "\nsemantic functions: " << std::dec << report.functions.size()
      << "\ncompiled regions: " << compiled_regions
      << "\nguest entries: " << report.entries.size()
      << "\nalternate entries: " << alternate_entries
      << "\nunresolved: " << report.unresolved.size() << "\n";
  const auto& diagnostics = report.diagnostics;
  out << "diagnostics: auto_discovered=" << diagnostics.auto_discovered_functions
      << " hinted=" << diagnostics.hinted_functions << " manual_chunks=" << diagnostics.manual_chunks
      << " switch_tables=" << diagnostics.switch_tables_resolved
      << " known_indirect_calls=" << diagnostics.known_indirect_calls
      << " known_indirect_branches=" << diagnostics.known_indirect_branches
      << " unresolved_indirect_sites=" << diagnostics.unresolved_indirect_sites
      << " native_replacements_applied=" << diagnostics.native_replacements_applied
      << " native_replacements_unsupported=" << diagnostics.native_replacements_unsupported
      << " import_thunks_recognized=" << diagnostics.import_thunks_recognized
      << " data_or_ignored_regions=" << diagnostics.data_or_ignored_regions
      << " pointer_tables=" << diagnostics.pointer_tables_discovered
      << " pointer_targets=" << diagnostics.pointer_table_targets_discovered
      << " adaptive_observations=" << diagnostics.adaptive_observations_consumed
      << " adaptive_rejected_revision=" << diagnostics.adaptive_observations_rejected_revision
      << " adaptive_rejected_invalid=" << diagnostics.adaptive_observations_rejected_invalid
      << " alternate_entries=" << diagnostics.alternate_entries_materialized
      << " weak_absorbed=" << diagnostics.weak_functions_absorbed
      << " orphan_recovered=" << diagnostics.orphan_entries_recovered
      << " gap_functions=" << diagnostics.gap_functions_recovered
      << " value_slice_hits=" << diagnostics.resolved_indirect_via_backward_slice
      << " readonly_table_hits=" << diagnostics.resolved_indirect_via_readonly_table
      << " return_fp_iterations=" << diagnostics.return_fixed_point_iterations
      << " no_return=" << diagnostics.inferred_no_return_functions
      << " may_return=" << diagnostics.inferred_may_return_functions
      << " entry_integrity_checks=" << diagnostics.entry_integrity_checks
      << " entry_integrity_failures=" << diagnostics.entry_integrity_failures
      << " knowledge_records=" << diagnostics.knowledge_records_loaded
      << " knowledge_fingerprinted=" << diagnostics.knowledge_functions_fingerprinted
      << " knowledge_matches=" << diagnostics.knowledge_matches_accepted
      << " knowledge_cross_revision=" << diagnostics.knowledge_cross_revision_matches
      << " knowledge_seed_candidates=" << diagnostics.knowledge_seed_candidates
      << " analysis_errors=" << diagnostics.analysis_errors << "\n";
  out << "runtime helpers: register-save helpers: " << diagnostics.register_save_helpers
      << " register-restore helpers: " << diagnostics.register_restore_helpers << "\n";
  out << "instruction pattern rules: loaded: " << diagnostics.instruction_patterns_loaded
      << " matches: " << diagnostics.instruction_pattern_matches << "\n";
  out << "codegen: input=" << diagnostics.codegen_input_functions
      << " unique=" << diagnostics.codegen_unique_functions
      << " duplicate_addresses_merged=" << diagnostics.codegen_duplicate_addresses_merged
      << " duplicate_symbols_rejected=" << diagnostics.codegen_duplicate_symbols_rejected
      << " shards=" << diagnostics.codegen_shards
      << " max_functions_per_shard=" << diagnostics.codegen_max_functions_per_shard
      << " max_shard_bytes=" << diagnostics.codegen_max_shard_bytes << "\n";
  for (const auto& function : report.functions) {
    out << "0x" << std::hex << function.guest_start << "-0x" << function.guest_end << " "
        << function.name << " confidence=" << std::dec << function.confidence
        << " authority=" << function_authority_name(function.authority)
        << " returns=" << return_behavior_name(function.return_behavior)
        << " status=" << (function.compiled ? "compiled" : "unresolved");
    if (function.fingerprint.valid())
      out << " fp=" << hash_name(function.fingerprint.instruction_shape_hash);
    if (!function.knowledge_matches.empty()) {
      const auto& match = function.knowledge_matches.front();
      out << " knowledge=" << (match.label.empty() ? match.record_id : match.label)
          << " score=" << match.score
          << (match.cross_revision ? " cross-revision" : "");
    }
    out << "\n";
  }
  for (const auto& entry : report.entries) {
    out << "entry 0x" << std::hex << entry.address << " -> owner=0x" << entry.owner_function
        << " block=0x" << entry.block << " kind=" << guest_entry_kind_name(entry.kind)
        << " confidence=" << std::dec << entry.confidence << " sources=";
    for (std::size_t i = 0; i < entry.sources.size(); ++i) {
      if (i != 0) out << ',';
      out << discovery_source_name(entry.sources[i]);
    }
    out << "\n";
  }
  out << format_cpu_coverage(report.cpu_coverage);
  for (const auto& warning : report.warnings)
    out << "warning: " << warning << "\n";
  for (const auto& item : report.unresolved)
    out << "warning 0x" << std::hex << item.address << " " << item.kind << ": " << item.detail << "\n";
  return out.str();
}

std::string format_ir(const DiscoveredFunction& function) {
  std::ostringstream out;
  out << "function " << function.name << " @ 0x" << std::hex << function.guest_start << "\n";
  for (const auto& block : function.ir.blocks)
    out << "  block 0x" << std::hex << block.guest_address << "-0x" << block.end_address
        << " instructions=" << std::dec << block.instructions.size() << "\n";
  for (const auto& block : function.ir.blocks)
    for (const auto& instruction : block.instructions)
      out << "    0x" << std::hex << instruction.guest_address
          << " word=0x" << instruction.guest_word
          << " op=" << std::dec << static_cast<unsigned>(instruction.op)
          << " result=" << instruction.result << "\n";
  return out.str();
}

}  // namespace xenon::recomp
