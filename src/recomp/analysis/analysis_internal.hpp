#pragma once

// Shared vocabulary of the recompilation driver's static analysis: candidate
// classification, provenance/ownership predicates, and the immutable context
// and worker-local result of analyzing one candidate function. Private to
// xenon_recomp.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xenon/cpu/decoder.hpp"
#include "xenon/recomp/driver.hpp"

namespace xenon::recomp::detail {
using cpu::GuestAddress;

// --- Executable image ranges (analysis/executable_ranges.cpp) --------------
const xbox::XexSection* executable_section(const ExecutableRangeIndex& index, GuestAddress address);

// --- Discovery provenance (analysis/discovery/discovery_sources.cpp) -------
bool source_contains(const DiscoveredFunction& function, DiscoverySource source);
void add_source(DiscoveredFunction& function, DiscoverySource source);
void add_source(std::vector<DiscoverySource>& sources, DiscoverySource source);
[[nodiscard]] bool source_is_semantic_boundary(DiscoverySource source) noexcept;
[[nodiscard]] bool has_semantic_boundary_evidence(const DiscoveredFunction& function) noexcept;
[[nodiscard]] FunctionAuthority authority_for_sources(
    const std::vector<DiscoverySource>& sources) noexcept;

// --- Address ownership (analysis/ownership/function_ownership.cpp) ---------
[[nodiscard]] bool owns_address(const DiscoveredFunction& function, GuestAddress address);
[[nodiscard]] bool has_block_entry(const DiscoveredFunction& function, GuestAddress address);

// --- Instruction classification -------------------------------------------
// (analysis/discovery/instruction_classification.cpp)
bool is_terminal(const cpu::DecodedInstruction& instruction);
bool is_conditional_control_transfer(const cpu::DecodedInstruction& instruction);
const char* classify_decode_failure(std::uint32_t word) noexcept;
[[nodiscard]] bool looks_like_function_prologue(const cpu::DecodedInstruction& instruction) noexcept;

// --- Indirect targets -------------------------------------------------------
// Switch-table hint resolution (analysis/control_flow/switch_tables.cpp).
std::vector<std::uint32_t> resolve_switch_targets(const analysis::SwitchTableHint& table,
                                                   const ExecutableRangeIndex& index,
                                                   std::vector<std::string>& warnings);

// Static vtable/function-pointer recovery
// (analysis/discovery/pointer_tables.cpp).
struct PointerTableScanResult {
  std::size_t tables{};
  std::set<GuestAddress> targets;
};

PointerTableScanResult scan_static_pointer_tables(
    const xbox::XexImage& image, const ExecutableRangeIndex& range_index,
    const std::map<GuestAddress, DiscoverySource>& known_starts);

// --- Per-candidate analysis context ---------------------------------------
// Immutable, read-only-shared inputs every per-candidate analysis worker
// needs (Part 2/4 of the Recomp Analysis V2 pass). Built once by
// load_and_analyze() before any wave runs; never mutated afterward, so
// sharing a single instance (by const reference) across worker threads is
// safe.
struct AnalysisContext {
  const xbox::XexImage& image;
  const ExecutableRangeIndex& range_index;
  const std::vector<ModuleHint>& hints;
  const analysis::AnalysisHintSetV2* hint_set_v2;  // may be null
  const std::map<GuestAddress, DiscoverySource>& seeds;
  const std::map<GuestAddress, std::string>& known_names;
  const std::vector<analysis::RuntimeHelper>& expanded_runtime_helpers;
  bool allow_partial;
  std::filesystem::path graph_cache;
  graph::Versions graph_versions;
  std::uint64_t configuration_hash;
  // Part 1 (Recomp Analysis V3): every DiscoverySource ever attached to a
  // runtime-discovered (non-seed) candidate address, accumulated by the
  // single calling thread between waves (see load_and_analyze()) and only
  // ever read - never written - by workers during a wave, exactly like
  // `seeds` above. Lets a candidate discovered via, say, both a direct call
  // from one function and a validated tail call from another carry BOTH
  // pieces of evidence once it is finally claimed and analyzed.
  const std::map<GuestAddress, std::vector<DiscoverySource>>& discovered_evidence;
};

// Candidate filtering and canonicalization
// (analysis/discovery/candidate_filters.cpp).
bool hinted_non_code(const AnalysisContext& ctx, GuestAddress address);
const analysis::InstructionPatternHint* instruction_pattern_match(const AnalysisContext& ctx,
                                                                   GuestAddress address,
                                                                   std::uint32_t word);
bool has_independent_function_hint(const analysis::AnalysisHintSetV2* hint_set_v2, GuestAddress address);
GuestAddress canonicalize_candidate(GuestAddress start, const analysis::AnalysisHintSetV2* hint_set_v2,
                                    bool& cyclic);
const xbox::XexImport* find_callable_import_thunk(const xbox::XexImage& image, GuestAddress address);

// Worker-local output of analyzing exactly one candidate address (Part 2's
// FunctionAnalysisResult). Contains no reference to shared state - a wave's
// results are merged into AnalysisReport by the single calling thread only,
// in deterministic address order (see load_and_analyze()).
struct FunctionAnalysisResult {
  enum class Outcome {
    Rejected,  // never becomes a DiscoveredFunction (matches the old algorithm's `continue`)
    Skipped,   // a runtime-helper address; deliberately not analyzed at all
    Accepted,  // becomes exactly one DiscoveredFunction entry (compiled or not)
  };
  Outcome outcome{Outcome::Rejected};
  DiscoveredFunction function;
  // Raw (not yet canonicalized/deduped) new candidates, each tagged with WHY
  // this analysis pass believes it is a function start (Part 1 of the
  // Recomp Analysis V3 pass) - carried through so the eventual candidate's
  // own `sources`/confidence reflect real provenance instead of a generic
  // fallback the moment it is claimed in a later wave.
  std::vector<std::pair<GuestAddress, DiscoverySource>> discovered;
  std::vector<UnresolvedReference> unresolved;
  std::vector<std::string> warnings;
  std::size_t instruction_pattern_matches{};
  std::size_t resolved_indirect_via_dataflow{};    // Gen 6 aggregate generic value-analysis hits
  std::size_t resolved_indirect_via_backward_slice{};
  std::size_t resolved_indirect_via_readonly_table{};
  std::size_t resolved_indirect_via_jump_table{};  // Part 8: switch/jump-table recovery hits
};

// Analyzes exactly one already-canonicalized, already-claimed candidate
// address: decode scan, FunctionChunk (Part 14) integration, and static
// compilation. Reads only `ctx` (immutable/shared) and `start`; every output
// is local to the returned result. Safe to call concurrently for different
// `start` values from multiple threads (cpu::Decoder and
// cpu::StaticFunctionCompiler are both stateless - see worker_pool.hpp's
// design note and docs/recomp/RECOMP_ANALYSIS_V2.md's reentrancy audit).
//
// This is a faithful extraction of the previous serial algorithm's per-
// address loop body: the chunk-owner redirect and "already discovered"
// checks that used to sit at the top of that loop are deliberately NOT
// here - both are now handled by the caller before a candidate is ever
// claimed (canonicalize_candidate() and the wave engine's `claimed` set
// respectively), which is what makes calling this function safely
// parallelizable across a whole wave's candidates.
// (analysis/control_flow/candidate_analysis.cpp)
FunctionAnalysisResult analyze_function_candidate(GuestAddress start, const AnalysisContext& ctx);

}  // namespace xenon::recomp::detail
