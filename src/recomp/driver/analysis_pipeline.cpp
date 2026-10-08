#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp {

// Whole-image static analysis. Each phase lives with its subsystem:
//   inputs          this file (image, providers, hint set, CPU coverage)
//   seeds           analysis/discovery/seed_collection.cpp
//   discovery       analysis/discovery/discovery_waves.cpp (per-candidate
//                   work: analysis/control_flow/candidate_analysis.cpp)
//   recovery        analysis/discovery/recovery.cpp
//   ownership       analysis/ownership/, knowledge/knowledge_matching.cpp
//   return behavior analysis/control_flow/return_inference.cpp
//   diagnostics     analysis/diagnostics/analysis_diagnostics.cpp
bool load_and_analyze(const DriverOptions& options, AnalysisReport& report, std::string& error) {
  using namespace detail;
  const auto analysis_start = std::chrono::steady_clock::now();
  if (options.progress) options.progress("[Xenon Recomp] Loading executable...");
  std::vector<ModuleHint> hints;
  if (!load_hints(options, hints, error)) return false;
  DriverOptions effective = options;
  effective.hints = std::move(hints);
  if (options.pre_parsed_image.has_value()) {
    report.image = *options.pre_parsed_image;
  } else {
    std::ifstream file(options.input, std::ios::binary);
    if (!file) { error = "unable to open input XEX: " + options.input.string(); return false; }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    if (!xbox::parse_xex_image(bytes, report.image, &error)) return false;
  }
  for (const auto* provider : effective.module_providers) {
    if (provider == nullptr) continue;
    if (!provider->provide(report.image, effective.hints, error)) return false;
  }
  report.configuration_hash = hash_config(effective);

  // CPU coverage audit: decode every executable word and ask the real native
  // backend and the real dynamic-fallback interpreter whether each distinct
  // instruction is supported. A gap is surfaced as a warning here, in the text
  // report and in the JSON report, so a missing instruction is found by analysis
  // rather than by the title crashing on it (Ace Combat 6 ended its first run on
  // a `rldicl` the fallback lacked).
  if (options.progress) options.progress("[Xenon Recomp] Auditing CPU instruction coverage...");
  report.cpu_coverage = audit_cpu_coverage(report.image);
  if (!report.cpu_coverage.gaps.empty()) {
    std::string summary = "CPU coverage gap: " + std::to_string(report.cpu_coverage.aot_gap_count()) +
                          " instruction kind(s) unsupported by the native backend, " +
                          std::to_string(report.cpu_coverage.fallback_gap_count()) +
                          " by the dynamic fallback:";
    std::size_t listed = 0;
    for (const auto& gap : report.cpu_coverage.gaps) {
      if (listed++ == 8u) {
        summary += " ...";
        break;
      }
      summary += " " + gap.mnemonic + "(x" + std::to_string(gap.count) + ")";
    }
    report.warnings.push_back(std::move(summary));
  }

  // Analysis Hint Schema V2 (Part 1/2): resolve, validate, and scope-check
  // before anything below consumes it. hint_provider_v2 (the production
  // path - see ModuleHintProviderV2) takes precedence over a directly
  // supplied hint_set_v2 (tests/manual invocation); a provider that cannot
  // supply a hint set for this exact executable revision is a hard failure,
  // never a silent "run without hints" or "use the nearest-looking
  // revision" (Part 2.5).
  const auto effective_identity = xbox::compute_effective_identity(report.image);
  if (options.hint_provider_v2) {
    analysis::AnalysisHintSetV2 resolved{};
    if (!options.hint_provider_v2->provide(effective_identity, resolved, error)) return false;
    report.hint_set_v2 = std::move(resolved);
  } else if (options.hint_set_v2) {
    report.hint_set_v2 = options.hint_set_v2;
  }
  if (report.hint_set_v2) {
    std::vector<std::string> schema_errors;
    if (!analysis::validate_hint_set(*report.hint_set_v2, schema_errors)) {
      error = "analysis hint set failed schema validation:";
      for (const auto& schema_error : schema_errors) error += " " + schema_error + ";";
      return false;
    }
    if (!analysis::hint_set_matches_identity(*report.hint_set_v2, effective_identity)) {
      error =
          "analysis hint set is scoped to a different executable revision than the one being "
          "analyzed (module '" +
          report.hint_set_v2->module_name + "')";
      return false;
    }
  }
  const auto& hint_set_v2 = report.hint_set_v2;

  // Phase B (Part 2): immutable executable-memory description, built once
  // and shared read-only by every analysis worker below.
  const ExecutableRangeIndex range_index(report.image);

  SeedSet seed_set;
  SeedStatistics seed_statistics;
  const auto current_effective_image_hash =
      xbox::format_effective_image_hash(xbox::compute_effective_image_hash(report.image));
  collect_seeds(effective, report, range_index, current_effective_image_hash, seed_set, seed_statistics);
  const auto& seeds = seed_set.sources;
  const auto& seed_evidence = seed_set.evidence;
  const auto& known_names = seed_set.known_names;

  // Register-range RuntimeHelperKinds (Part 1.5) declare only their family's
  // lowest variant address in the raw hint list; expand once up front so
  // every actually-callable variant address is available for both the
  // discovery short-circuit below and the diagnostics counters.
  const std::vector<analysis::RuntimeHelper> expanded_runtime_helpers =
      hint_set_v2 ? analysis::expand_runtime_helpers(hint_set_v2->runtime_helpers)
                  : std::vector<analysis::RuntimeHelper>{};

  if (effective.progress) effective.progress("[Xenon Recomp] Collecting function candidates...");

  // Phase D/E (Part 2): validate the initial seed set and freeze it as the
  // first discovery-wave frontier. A seed outside any executable section is
  // rejected right here (never reaches per-candidate analysis at all) -
  // matches the previous serial algorithm's pre-loop filter exactly.
  std::size_t rejected_nonexec = 0;
  std::vector<GuestAddress> frontier;
  frontier.reserve(seeds.size());
  for (const auto& [address, source] : seeds) {
    if (range_index.is_executable_address(address)) {
      frontier.push_back(address);
    } else {
      report.unresolved.push_back({address, address, "seed", "target is not in an executable section"});
      ++rejected_nonexec;
    }
  }
  if (effective.progress)
    effective.progress("[Xenon Recomp] Candidate functions: " + std::to_string(frontier.size()));

  // Part 1 (Recomp Analysis V3): accumulates every DiscoverySource ever
  // attached to a runtime-discovered candidate address, across all waves.
  // Mutated only by this (single) calling thread, strictly between waves -
  // never while a wave's WorkerPool::parallel_for() call is in flight - so
  // sharing it into AnalysisContext by const reference is safe (matches
  // `seeds`/`known_names`'s existing contract exactly).
  std::map<GuestAddress, std::vector<DiscoverySource>> discovered_evidence = seed_evidence;

  const AnalysisContext ctx{report.image,
                            range_index,
                            effective.hints,
                            hint_set_v2 ? &*hint_set_v2 : nullptr,
                            seeds,
                            known_names,
                            expanded_runtime_helpers,
                            effective.allow_partial,
                            effective.graph_cache.empty() ? effective.output / ".graph" : effective.graph_cache,
                            effective.graph_versions,
                            report.configuration_hash,
                            discovered_evidence};

  // Phase F (Part 2/3/4): parallel per-function analysis, driven in
  // deterministic discovery waves (Part 5) so that compiling one function
  // (which can discover new call/branch targets) never requires a worker to
  // mutate the canonical function registry directly - each wave's workers
  // return purely local results (FunctionAnalysisResult), and only the
  // single calling thread ever merges them into `report`, in a fixed
  // address-sorted order independent of which worker finished which item
  // first. jobs=1 and jobs=N run through this exact same code path (no
  // separate serial algorithm to keep in sync), so their output is
  // byte-identical by construction, not by careful parallel bug-for-bug
  // matching of two implementations.
  const auto worker_count = resolve_worker_count(effective.analysis_jobs);
  if (effective.progress)
    effective.progress("[Xenon Recomp] Analysis workers: " + std::to_string(worker_count));
  // WorkerPool itself degrades to pure inline execution (zero threading
  // overhead) when worker_count == 1, so constructing and using it
  // unconditionally - rather than branching around it for the jobs=1 case -
  // is both simpler and already optimal.
  const WorkerPool pool(worker_count);

  DiscoveryWaveEngine engine(ctx, pool, discovered_evidence, report, effective.progress);
  engine.frontier = std::move(frontier);
  engine.process_frontier();

  const analysis::AnalysisHintSetV2* const hint_set_ptr = hint_set_v2 ? &*hint_set_v2 : nullptr;
  const std::size_t orphan_entries_recovered =
      effective.recover_multi_source_orphans
          ? recover_multi_source_orphans(engine, range_index, hint_set_ptr)
          : 0u;
  const std::size_t gap_functions_recovered =
      effective.recover_unowned_gaps ? recover_unowned_gaps(engine, hint_set_ptr) : 0u;

  if (effective.progress) effective.progress("[Analysis] complete");


  std::sort(report.functions.begin(), report.functions.end(),
            [](const auto& a, const auto& b) { return a.guest_start < b.guest_start; });
  const auto codegen_duplicate_addresses_merged =
      deduplicate_functions_by_address(report.functions, report.warnings);
  reconcile_call_graph_provenance(report);

  const auto knowledge_statistics =
      apply_knowledge_matching(report, effective, current_effective_image_hash);

  auto absorption = absorb_weak_functions(report);

  // Gen 6: return/no-return behavior is solved after discovery/ownership has
  // stabilized so worker-wave timing cannot affect the result. Explicit
  // NoReturn metadata seeds the lattice; normal returns and terminal tail
  // calls then converge monotonically to a fixed point.
  const auto return_inference = infer_return_behaviors(report.functions, report.warnings);

  build_guest_entry_map(report, std::move(absorption.entries), effective, expanded_runtime_helpers);
  diagnose_function_ownership(report, range_index);
  deduplicate_unresolved(report);

  AnalysisRunStatistics statistics{};
  statistics.analysis_start = analysis_start;
  statistics.worker_count = worker_count;
  statistics.candidate_functions_total = engine.claimed.size();
  statistics.rejected_nonexec = rejected_nonexec;
  statistics.rejected_unaligned = engine.rejected_unaligned;
  statistics.wave_count = engine.wave_count;
  statistics.functions_analyzed = engine.functions_analyzed;
  statistics.instruction_pattern_match_count = engine.instruction_pattern_match_count;
  statistics.resolved_indirect_via_dataflow = engine.resolved_indirect_via_dataflow;
  statistics.resolved_indirect_via_backward_slice = engine.resolved_indirect_via_backward_slice;
  statistics.resolved_indirect_via_readonly_table = engine.resolved_indirect_via_readonly_table;
  statistics.resolved_indirect_via_jump_table = engine.resolved_indirect_via_jump_table;
  statistics.codegen_duplicate_addresses_merged = codegen_duplicate_addresses_merged;
  statistics.weak_functions_absorbed = absorption.absorbed;
  statistics.orphan_entries_recovered = orphan_entries_recovered;
  statistics.gap_functions_recovered = gap_functions_recovered;
  statistics.knowledge_records_loaded = effective.knowledge_records.size();
  statistics.return_inference = return_inference;
  statistics.seeds = seed_statistics;
  statistics.knowledge = knowledge_statistics;
  populate_analysis_diagnostics(report, statistics);

  return true;
}

}  // namespace xenon::recomp
