#pragma once

// Whole-image phases of load_and_analyze() (driver/analysis_pipeline.cpp):
// seed collection, the deterministic discovery-wave engine, recovery passes,
// ownership/entry reconciliation and diagnostics. Private to xenon_recomp.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "recomp/analysis/analysis_internal.hpp"
#include "xenon/recomp/worker_pool.hpp"

namespace xenon::recomp::detail {

// True when a V2 data/ignored/invalid-instruction region (anything but
// CodeOverride) covers `address`. A null hint set blocks nothing.
bool in_non_code_hint_region(const analysis::AnalysisHintSetV2* hint_set_v2, GuestAddress address);

// --- Seeds (analysis/discovery/seed_collection.cpp) ------------------------
//
// Initial discovery is evidence-accumulating rather than last-writer-wins.
// A real title entry can simultaneously be an export, unwind start, module
// hint, pointer-table target, etc.; dropping earlier evidence made authority
// and confidence depend on insertion order and allowed weak adaptive sources
// to accidentally downgrade strong semantic boundaries. `seeds` retains one
// primary source for legacy/canonicalization checks while `seed_evidence`
// preserves the complete fact set consumed by per-function analysis.
struct SeedSet {
  std::map<GuestAddress, DiscoverySource> sources;
  std::map<GuestAddress, std::vector<DiscoverySource>> evidence;
  std::map<GuestAddress, std::string> known_names;

  void add(GuestAddress address, DiscoverySource source);
  void erase(GuestAddress address);
  void erase_range(GuestAddress begin, GuestAddress end);
};

struct SeedStatistics {
  std::size_t pointer_tables_discovered{};
  std::size_t pointer_table_targets_discovered{};
  std::size_t adaptive_observations_consumed{};
  std::size_t adaptive_observations_rejected_revision{};
  std::size_t adaptive_observations_rejected_invalid{};
  std::size_t knowledge_seed_candidates{};
};

// Entry point, exports, module hints, unwind metadata, TLS callback, schema V2
// hints, static pointer tables, adaptive runtime observations and knowledge
// anchors - in that order.
void collect_seeds(const DriverOptions& effective, AnalysisReport& report,
                   const ExecutableRangeIndex& range_index,
                   const std::string& current_effective_image_hash, SeedSet& seed_set,
                   SeedStatistics& statistics);

// --- Discovery waves (analysis/discovery/discovery_waves.cpp) --------------
// Deterministic wave engine: workers analyze one wave of claimed candidates
// in parallel and return local results; only the calling thread merges them,
// in address order, between waves. jobs=1 and jobs=N share this path.
class DiscoveryWaveEngine {
 public:
  DiscoveryWaveEngine(const AnalysisContext& ctx, const WorkerPool& pool,
                      std::map<GuestAddress, std::vector<DiscoverySource>>& discovered_evidence,
                      AnalysisReport& report, const std::function<void(const std::string&)>& progress)
      : ctx_(ctx),
        pool_(pool),
        discovered_evidence_(discovered_evidence),
        report_(report),
        progress_(progress) {}

  // Claims, analyzes and merges `frontier` wave by wave until no new
  // candidate is discovered.
  void process_frontier();

  [[nodiscard]] AnalysisReport& report() noexcept { return report_; }
  [[nodiscard]] std::map<GuestAddress, std::vector<DiscoverySource>>& discovered_evidence() noexcept {
    return discovered_evidence_;
  }

  std::vector<GuestAddress> frontier;
  std::set<GuestAddress> claimed;  // every address ever handed to per-candidate analysis (Part 5/22:
                                   // no global function registry is ever mutated by a worker - this
                                   // set, and `report` itself, are only ever touched by this thread,
                                   // between waves)
  std::size_t functions_analyzed = 0;
  std::size_t instruction_pattern_match_count = 0;
  std::size_t wave_count = 0;
  std::size_t rejected_unaligned = 0;
  std::size_t resolved_indirect_via_dataflow = 0;
  std::size_t resolved_indirect_via_backward_slice = 0;
  std::size_t resolved_indirect_via_readonly_table = 0;
  std::size_t resolved_indirect_via_jump_table = 0;

 private:
  const AnalysisContext& ctx_;
  const WorkerPool& pool_;
  std::map<GuestAddress, std::vector<DiscoverySource>>& discovered_evidence_;
  AnalysisReport& report_;
  const std::function<void(const std::string&)>& progress_;
};

// --- Recovery passes (analysis/discovery/recovery.cpp) ---------------------
// Each returns the number of new candidates it handed to the wave engine.
std::size_t recover_multi_source_orphans(DiscoveryWaveEngine& engine,
                                         const ExecutableRangeIndex& range_index,
                                         const analysis::AnalysisHintSetV2* hint_set_v2);
std::size_t recover_unowned_gaps(DiscoveryWaveEngine& engine,
                                 const analysis::AnalysisHintSetV2* hint_set_v2);

// --- Ownership reconciliation (analysis/ownership/function_ownership.cpp) --
std::size_t deduplicate_functions_by_address(std::vector<DiscoveredFunction>& functions,
                                             std::vector<std::string>& warnings);
void reconcile_call_graph_provenance(AnalysisReport& report);

struct WeakFunctionAbsorption {
  std::vector<GuestEntryPoint> entries;
  std::size_t absorbed{};
};
WeakFunctionAbsorption absorb_weak_functions(AnalysisReport& report);

// --- Knowledge matching (knowledge/knowledge_matching.cpp) -----------------
struct KnowledgeMatchStatistics {
  std::size_t functions_fingerprinted{};
  std::size_t matches_considered{};
  std::size_t matches_accepted{};
  std::size_t cross_revision_matches{};
};
KnowledgeMatchStatistics apply_knowledge_matching(AnalysisReport& report, const DriverOptions& effective,
                                                  const std::string& current_effective_image_hash);

// --- Return behavior (analysis/control_flow/return_inference.cpp) ----------
struct ReturnInferenceStats {
  std::size_t iterations{};
  std::size_t no_return{};
  std::size_t may_return{};
};

ReturnInferenceStats infer_return_behaviors(std::vector<DiscoveredFunction>& functions,
                                            std::vector<std::string>& warnings);

// --- Guest entries (analysis/ownership/entry_reconciliation.cpp) -----------
// Strongest compiled (non-replacement) owner of `target`, by authority then
// confidence.
const DiscoveredFunction* find_strongest_owner(const std::vector<DiscoveredFunction>& functions,
                                               GuestAddress target);
void build_guest_entry_map(AnalysisReport& report, std::vector<GuestEntryPoint> absorbed_entries,
                           const DriverOptions& effective,
                           const std::vector<analysis::RuntimeHelper>& expanded_runtime_helpers);
void diagnose_function_ownership(AnalysisReport& report, const ExecutableRangeIndex& range_index);
void deduplicate_unresolved(AnalysisReport& report);

// --- Diagnostics (analysis/diagnostics/analysis_diagnostics.cpp) -----------
struct AnalysisRunStatistics {
  std::chrono::steady_clock::time_point analysis_start{};
  std::size_t worker_count{};
  std::size_t candidate_functions_total{};
  std::size_t rejected_nonexec{};
  std::size_t rejected_unaligned{};
  std::size_t wave_count{};
  std::size_t functions_analyzed{};
  std::size_t instruction_pattern_match_count{};
  std::size_t resolved_indirect_via_dataflow{};
  std::size_t resolved_indirect_via_backward_slice{};
  std::size_t resolved_indirect_via_readonly_table{};
  std::size_t resolved_indirect_via_jump_table{};
  std::size_t codegen_duplicate_addresses_merged{};
  std::size_t weak_functions_absorbed{};
  std::size_t orphan_entries_recovered{};
  std::size_t gap_functions_recovered{};
  std::size_t knowledge_records_loaded{};
  ReturnInferenceStats return_inference{};
  SeedStatistics seeds{};
  KnowledgeMatchStatistics knowledge{};
};
void populate_analysis_diagnostics(AnalysisReport& report, const AnalysisRunStatistics& statistics);

}  // namespace xenon::recomp::detail
