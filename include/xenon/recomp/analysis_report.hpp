#pragma once

// The result of load_and_analyze(): the analyzed image, its functions and
// entries, diagnostics, and the text/JSON/IR report formatters.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/cpu_coverage.hpp"
#include "xenon/recomp/function_analysis.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

// Analysis diagnostics (Part 1.12 / 6.2): counts useful for judging whether
// a commercial title's analysis is Gracemeria-ready, beyond the raw
// functions/unresolved lists above.
struct AnalysisDiagnostics {
  std::size_t auto_discovered_functions{};
  std::size_t hinted_functions{};
  std::size_t manual_chunks{};
  std::size_t switch_tables_resolved{};
  std::size_t known_indirect_calls{};
  std::size_t known_indirect_branches{};
  std::size_t unresolved_indirect_sites{};
  std::size_t native_replacements_applied{};
  std::size_t native_replacements_unsupported{};
  // Count of DiscoveredFunction entries recognized as XEX-native import
  // thunks (DiscoveredFunction::import_thunk set) - see
  // analyze_function_candidate()'s import-thunk short-circuit.
  std::size_t import_thunks_recognized{};
  std::size_t data_or_ignored_regions{};
  std::size_t analysis_errors{};
  // Register-range RuntimeHelperKind families (Part 9) - counts of declared
  // (not expanded-per-variant) save/restore helper entries.
  std::size_t register_save_helpers{};
  std::size_t register_restore_helpers{};
  // Instruction-pattern hints (Part 2/9): how many rules were loaded from
  // the hint set, and how many times a rule actually matched during
  // scanning (a rule loaded but never matched is a real, visible signal -
  // e.g. a stale/misconfigured pattern - not silently ignored).
  std::size_t instruction_patterns_loaded{};
  std::size_t instruction_pattern_matches{};

  // Performance/parallelism diagnostics (Part 19/24 of the Recomp Analysis
  // V2 pass) - lets a caller (the CLI, xenon-prepare, a real-title
  // before/after comparison) judge throughput without external profiling.
  std::size_t candidate_functions_total{};       // every address ever handed to per-function analysis
  std::size_t function_candidates_rejected_nonexec{};   // seed/candidate outside any executable section
  std::size_t function_candidates_rejected_unaligned{}; // candidate failing 4-byte PPC alignment
  std::size_t analysis_waves{};                  // discovery waves executed (Part 5)
  std::size_t analysis_workers{};                // worker count actually used for analysis
  std::size_t functions_analyzed{};               // == candidate_functions_total minus short-circuited/rejected
  std::size_t functions_compiled{};                // functions with compiled == true (any kind)
  std::size_t invalid_ppc_sites{};                 // unresolved entries of kind "invalid-ppc"
  std::size_t unresolved_indirect_calls{};         // unresolved entries of kind "indirect-call"
  std::size_t unresolved_indirect_branches{};      // unresolved entries of kind "indirect-branch"
  // Sites the MODULE's own hint metadata declared as indirect and supplied
  // >=1 resolved target for - counts hint-declared sites, not Xenon's own
  // proof. Deliberately independent of DiscoverySource::ResolvedIndirect
  // (which only Xenon's own dataflow/jump-table recovery ever attaches -
  // hint-resolved targets are tagged ModuleHint instead, see
  // analyze_function_candidate()) and of resolved_indirect_via_dataflow/
  // resolved_indirect_via_jump_table below (which count Xenon's own generic
  // proof events, not module declarations). A real title can have any
  // relationship between the three: a hint set with no indirect metadata at
  // all still lets these generic counters be nonzero, and vice versa.
  std::size_t resolved_indirect_calls{};           // KnownIndirectCall/hint sites with >=1 target resolved
  std::size_t resolved_indirect_branches{};        // KnownIndirectBranch/switch sites with >=1 target resolved
  std::int64_t analysis_duration_ms{};
  std::int64_t codegen_duration_ms{};

  // Recomp Analysis V3 (discovery-quality pass) additions - see
  // docs/recomp/RECOMP_ANALYSIS_V3.md.
  // Xenon's own generic proof of an indirect call/branch target, entirely
  // independent of any module hint - see DiscoverySource::ResolvedIndirect's
  // doc comment (attached by exactly these two mechanisms and no others).
  // Both count RESOLUTION EVENTS during scanning (one per site that
  // resolved), not distinct functions - the same target reached via several
  // resolved sites increments these once per site but only ever adds
  // ResolvedIndirect to that target's `sources` once (add_source() dedupes
  // by kind). `functions_with_resolved_indirect_provenance` below is the
  // directly comparable distinct-function count.
  std::size_t resolved_indirect_via_dataflow{};    // Gen 6: generic PPC value/dataflow resolutions (compatibility aggregate)
  std::size_t resolved_indirect_via_backward_slice{}; // Gen 6: expensive bounded slice used only after fast state failed
  std::size_t resolved_indirect_via_readonly_table{}; // Gen 6: static non-writable table/vtable load resolved a target
  std::size_t resolved_indirect_via_jump_table{};  // Part 8: switch/jump-table recovery hits (generic, no hint)
  std::size_t return_fixed_point_iterations{};      // Gen 6: iterations required for call/return behavior convergence
  std::size_t inferred_no_return_functions{};      // Gen 6: final NoReturn functions (explicit + propagated)
  std::size_t inferred_may_return_functions{};     // Gen 6: final MayReturn functions
  // Distinct discovered functions whose `sources` contains
  // DiscoverySource::ResolvedIndirect - the number to cross-check against
  // resolved_indirect_via_dataflow/_via_jump_table above (always <= their
  // sum, strictly less whenever the same target was reached by more than
  // one resolved site).
  std::size_t functions_with_resolved_indirect_provenance{};
  std::size_t candidates_from_tls_callbacks{};     // Part 2: XEX TLS directory callback addresses seeded
  std::size_t unsupported_ppc_sites{};             // Part 5: decode failures where the primary opcode IS cataloged
                                                    // (a real instruction family Xenon just doesn't implement this
                                                    // encoding of yet) - distinct from invalid_ppc_sites, where the
                                                    // primary opcode has no cataloged entries at all
  std::size_t unsupported_vmx_sites{};             // Part 5: unsupported_ppc_sites narrowed to a Vector-group primary

  // Codegen ownership/dedup diagnostics (generated-code deduplication /
  // shard ownership fix): together these prove the hard invariant "ONE
  // guest callable function address -> ONE canonical generated-function
  // record -> ONE emitted C++ definition" actually held for this run, not
  // merely that codegen completed without crashing.
  std::size_t codegen_duplicate_addresses_merged{};  // report.functions entries merged by guest_start
                                                      // before codegen ever saw them (see
                                                      // load_and_analyze()'s post-sort dedup pass) - the
                                                      // wave engine's `claimed` set and the FunctionChunk/
                                                      // independent-FunctionHint mutual-exclusion fix both
                                                      // prevent this from happening at all; nonzero here
                                                      // means a real would-be duplicate was caught, not a
                                                      // routine event.
  std::size_t codegen_input_functions{};             // functions handed to generate_project()'s codegen
                                                      // stage (compiled, non-native-replacement)
  std::size_t codegen_unique_functions{};            // == codegen_input_functions on success; kept
                                                      // separate so a caller never has to assume the
                                                      // invariant held rather than reading it
  std::size_t codegen_duplicate_symbols_rejected{};  // distinct-address symbol collisions that made
                                                      // generate_project() fail codegen before emitting
                                                      // any conflicting C++ (Part 3/13: never silently
                                                      // renamed/suppressed/worked around)
  std::size_t codegen_shards{};                      // functions/shard_*.cpp files written
  std::size_t codegen_max_functions_per_shard{};     // largest function count in any one shard
  std::size_t codegen_max_shard_bytes{};             // largest generated-source size in any one shard

  // Region + Entry / adaptive-analysis diagnostics.
  std::size_t pointer_tables_discovered{};
  std::size_t pointer_table_targets_discovered{};
  std::size_t adaptive_observations_consumed{};
  std::size_t alternate_entries_materialized{};
  std::size_t weak_functions_absorbed{};
  std::size_t orphan_entries_recovered{};
  std::size_t gap_functions_recovered{};
  std::size_t adaptive_observations_rejected_revision{};
  std::size_t adaptive_observations_rejected_invalid{};
  std::size_t entry_integrity_checks{};
  std::size_t entry_integrity_failures{};

  // Gen 9 universal knowledge-base diagnostics.
  std::size_t knowledge_records_loaded{};
  std::size_t knowledge_functions_fingerprinted{};
  std::size_t knowledge_matches_considered{};
  std::size_t knowledge_matches_accepted{};
  std::size_t knowledge_cross_revision_matches{};
  std::size_t knowledge_seed_candidates{};
};

struct AnalysisReport {
  xbox::XexImage image;
  // Which of this image's instructions the native backend / the dynamic fallback
  // cannot execute (see cpu_coverage.hpp). A fallback gap is a run-ending crash
  // waiting for the title to reach code the recompiler did not discover.
  CpuCoverageReport cpu_coverage;
  std::vector<DiscoveredFunction> functions;
  // Complete dispatch map. Canonical function starts and alternate entry
  // blocks live in one address-indexed model, while semantic function
  // identity remains in `functions`.
  std::vector<GuestEntryPoint> entries;
  std::vector<UnresolvedReference> unresolved;
  std::vector<std::string> warnings;
  std::uint64_t configuration_hash{};
  // Populated whenever a schema V2 hint set was consumed (directly via
  // DriverOptions::hint_set_v2 or through hint_provider_v2) - see
  // load_and_analyze().
  std::optional<analysis::AnalysisHintSetV2> hint_set_v2;
  AnalysisDiagnostics diagnostics;
};

[[nodiscard]] std::string format_report(const AnalysisReport& report);
[[nodiscard]] std::string format_report_json(const AnalysisReport& report);
[[nodiscard]] std::string format_ir(const DiscoveredFunction& function);

}  // namespace xenon::recomp
