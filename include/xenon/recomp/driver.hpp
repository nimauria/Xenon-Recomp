#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "xenon/cpu/ir.hpp"
#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/knowledge_base.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

// Sorted, indexed description of a XexImage's executable ranges (Part 7 of
// the Recomp Analysis V2 pass) - replaces repeated linear scans over
// `image.sections` (xbox::XexImage::sections is a flat, unsorted vector) in
// every hot per-candidate/per-address check the analyzer performs. Built
// once per analysis run from the immutable image, then queried read-only
// (safe to share across analysis worker threads).
class ExecutableRangeIndex {
 public:
  explicit ExecutableRangeIndex(const xbox::XexImage& image);

  // The containing section for `address` if one exists (executable or not),
  // else nullptr.
  [[nodiscard]] const xbox::XexSection* containing_section(std::uint32_t address) const noexcept;
  // True iff `address` falls inside a section marked executable.
  [[nodiscard]] bool is_executable_address(std::uint32_t address) const noexcept;
  // True iff `address` falls inside any known section (executable or not) -
  // i.e. the address is part of the mapped image at all.
  [[nodiscard]] bool is_mapped_address(std::uint32_t address) const noexcept;
  // True iff `address` satisfies PPC's mandatory 4-byte instruction
  // alignment. A function/branch candidate failing this can never be real
  // code and should be rejected before any decode is attempted.
  [[nodiscard]] static bool is_aligned_ppc_address(std::uint32_t address) noexcept;

 private:
  struct Entry {
    std::uint32_t begin;
    std::uint32_t end;  // exclusive
    const xbox::XexSection* section;
  };
  std::vector<Entry> entries_;  // sorted by `begin`, non-overlapping per input section
};



enum class DiscoverySource : std::uint8_t {
  EntryPoint,
  Export,
  DirectCall,
  DirectBranch,
  UnwindMetadata,
  ModuleHint,
  // Recomp Analysis V3 (discovery-quality pass) additions - see
  // docs/recomp/RECOMP_ANALYSIS_V3.md for the full provenance/confidence model.
  TlsCallback,         // XEX TLS directory callback address (Part 2) - a real,
                       // generic loader-exposed entry point, not title-specific.
  ResolvedIndirect,    // legacy/coarse indirect-target provenance retained for
                       // report compatibility. New analysis records the more
                       // precise call/branch variants below; this source alone
                       // is NOT proof of a semantic function boundary.
  ResolvedIndirectCall,   // statically resolved linked/indirect call target.
                          // Strong callable evidence and eligible to anchor a
                          // semantic function boundary.
  ResolvedIndirectBranch, // statically resolved non-linked indirect branch or
                          // switch target. Dispatchable entry evidence only;
                          // never self-promotes an internal block to a function.
  ControlFlowHint,        // module-provided branch/switch target. The hint proves
                          // the edge/entry, not that the target begins a distinct
                          // semantic function.
  ValidatedTailCall,   // a non-linked branch whose target's control-flow
                       // shape corroborates it being a genuine tail call to a
                       // separate function rather than an internal jump
                       // (Part 4/10).
  PrologueHeuristic,   // recognized compiler prologue/epilogue pattern (Part
                       // 4) - supporting evidence only; Xenon never creates a
                       // function candidate from this alone.
  PointerTable,        // target recovered from a generic static function-pointer/vtable
                       // run in non-executable image data. Entry evidence, not by itself
                       // authoritative proof of a semantic function boundary.
  RuntimeObservation,  // executable guest entry observed by a prior runtime/trace pass.
                       // Strong evidence that the address must be dispatchable, but not
                       // necessarily that it begins a distinct semantic function.
  GapRecovery,         // orphan executable branch target recovered after graph closure
  KnowledgeMatch,      // Gen 9 normalized cross-revision fingerprint corroboration.
                       // Never a semantic boundary on its own.
                       // because multiple independent owners reference it. Low authority;
                       // eligible for absorption into a stronger overlapping owner.
};

// Part 11: confidence is derived from evidence (DiscoverySource), not an
// arbitrary single-source magic number. Returns the strongest single piece
// of evidence's base confidence, plus a small corroboration bonus when two
// or more independent sources agree on the same candidate (capped at 100).
// Ordering here matches the tiers real Xbox 360 static analysis evidence
// actually has: entry point/explicit module hint/verified unwind metadata
// are as strong as generic evidence gets; a bare heuristic pattern with no
// other corroboration is the weakest signal this scheme ever trusts.
[[nodiscard]] std::uint32_t discovery_source_base_confidence(DiscoverySource source) noexcept;
[[nodiscard]] std::uint32_t confidence_for_sources(const std::vector<DiscoverySource>& sources) noexcept;

struct ModuleHint {
  struct Symbol {
    std::uint32_t address{};
    std::string name;
  };
  std::string name;
  std::vector<std::uint32_t> function_boundaries;
  std::vector<std::uint32_t> data_regions;
  std::vector<std::uint32_t> ignored_regions;
  std::vector<Symbol> known_symbols;
  std::vector<std::string> special_hooks;
  std::vector<std::string> patches;
};

class ModuleHintProvider {
 public:
  virtual ~ModuleHintProvider() = default;
  virtual bool provide(const xbox::XexImage& image,
                       std::vector<ModuleHint>& hints,
                       std::string& error) const = 0;
};

class ModuleCatalog {
 public:
  void register_provider(const ModuleHintProvider& provider);
  [[nodiscard]] const std::vector<const ModuleHintProvider*>& providers() const noexcept {
    return providers_;
  }

 private:
  std::vector<const ModuleHintProvider*> providers_;
};

// Production source of Analysis Hint Schema V2 data (Part 2 of the
// Gracemeria readiness pass) - distinct from the legacy ModuleHintProvider
// above, which stays supported unchanged for existing callers/tests. A real
// implementation (see module_hint_provider.hpp's FileModuleHintProvider)
// reads installed module package data from disk; this interface is what the
// Recomp Driver actually calls.
class ModuleHintProviderV2 {
 public:
  virtual ~ModuleHintProviderV2() = default;

  // Supplies the hint set scoped to `identity` (the executable actually
  // being analyzed - see xbox::compute_effective_identity()). Returns false
  // with `error` set when this provider has no hint set for that exact
  // revision - a hard failure the caller must not paper over with a
  // different revision's data (Part 2.5: "fail clearly", never the
  // nearest-looking revision).
  [[nodiscard]] virtual bool provide(const xbox::XexEffectiveIdentity& identity,
                                     analysis::AnalysisHintSetV2& out_hint_set,
                                     std::string& error) const = 0;
};

struct UnresolvedReference {
  std::uint32_t address{};
  std::uint32_t target{};
  std::string kind;
  std::string detail;
};

// Function identity and dispatchable guest entry identity are deliberately
// separate. Xbox 360 code routinely has shared suffixes, outlined blocks,
// funclets and indirect-entry blocks that are valid control-flow destinations
// without being independent C/C++ functions.
enum class FunctionAuthority : std::uint8_t {
  GapRecovery,
  InferredBranch,
  PointerTable,
  RuntimeObservation,
  ResolvedIndirect,
  DirectCall,
  Metadata,
  ModuleHint,
  EntryPoint,
};

enum class GuestEntryKind : std::uint8_t {
  Function,
  AlternateBlock,
  RuntimeHelper,
  NativeReplacement,
};

// Gen 6 call/return fixed-point result. MayReturn means at least one proven
// normal return path exists; NoReturn means all currently-known terminal exits
// are proven non-returning (or the module explicitly declared NoReturn).
enum class ReturnBehavior : std::uint8_t {
  Unknown,
  MayReturn,
  NoReturn,
};

struct BranchReference {
  std::uint32_t site{};
  std::uint32_t target{};
  // `terminal` means the edge terminates the current basic block without a
  // fallthrough path. It is deliberately separate from `conditional`: a
  // conditional branch ends a block for CFG purposes but is not a semantic
  // tail call. Keeping these facts separate prevents post-analysis
  // reconciliation from re-promoting loop headers / shared suffixes into
  // functions merely because they cross a tentative function extent.
  bool terminal{};
  bool indirect{};
  bool linked{};
  bool conditional{};
  bool fallthrough{};
};

struct GuestEntryPoint {
  std::uint32_t address{};
  std::uint32_t owner_function{};
  std::uint32_t block{};
  GuestEntryKind kind{GuestEntryKind::Function};
  std::vector<DiscoverySource> sources;
  std::uint32_t confidence{};
};

enum class AdaptiveObservationKind : std::uint8_t {
  ExecutedEntry,
  IndirectCallTarget,
  IndirectBranchTarget,
};

// Generic feedback record suitable for future runtime/coverage/network
// learning. It contains facts only: an executable guest address was observed
// as an entry/indirect target. Analysis decides whether that address is a new
// semantic function or an alternate entry into an existing compiled region.
struct AdaptiveObservation {
  std::uint32_t address{};
  std::uint32_t site{};
  AdaptiveObservationKind kind{AdaptiveObservationKind::ExecutedEntry};
  std::uint32_t hits{1};
  std::optional<std::uint32_t> owner_hint;
  // Optional lowercase effective-image SHA-1. Runtime-generated records set
  // this so observations from an old title update/revision cannot poison a
  // new executable that happens to reuse the same guest address. Empty keeps
  // hand-authored/developer traces backwards-compatible.
  std::string image_hash;
};

struct DiscoveredFunction {
  std::uint32_t guest_start{};
  std::uint32_t guest_end{};
  std::vector<std::uint32_t> ranges;
  std::string name;
  std::vector<DiscoverySource> sources;
  std::vector<std::uint32_t> calls;
  std::vector<std::uint32_t> callers;
  std::vector<std::uint32_t> branch_references; // compatibility/summary target list
  std::vector<BranchReference> branches;          // exact source-site control-flow facts
  FunctionAuthority authority{FunctionAuthority::InferredBranch};
  std::uint32_t confidence{};
  std::uint64_t source_hash{};
  bool compiled{};
  std::string error;
  cpu::ir::Function ir;

  // Gen 6 return/no-return analysis. has_explicit_return is a local decode fact
  // (an unconditional bclrx/blr-style return was observed). The fixed-point
  // pass may then propagate MayReturn/NoReturn through terminal tail calls.
  ReturnBehavior return_behavior{ReturnBehavior::Unknown};
  bool return_behavior_explicit{};
  bool has_explicit_return{};

  // Gen 9 universal knowledge-base identity and any confidence-scored
  // matches accepted for this function. These are report/evidence data; a
  // knowledge label never silently replaces a module-provided symbol name.
  FunctionFingerprint fingerprint{};
  std::vector<KnowledgeMatchReport> knowledge_matches;

  // Reserved for a FunctionChunk hint's declared parent address; currently
  // unset by every production path. A FunctionChunk's bytes ARE stitched
  // into the parent's own compiled IR (see driver.cpp's
  // analyze_function_candidate()), so a chunk never gets its own
  // DiscoveredFunction entry to record this on in the first place - the
  // association is recoverable from the parent's own `ranges` instead. See
  // docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md's "known limitations" section.
  std::optional<std::uint32_t> chunk_parent;
  // Set when a NativeReplacement hint (Part 1.10) matched this address and
  // Xenon has a real implementation for it (native_replacements.hpp) -
  // generate_project() emits a call to that implementation instead of
  // compiling guest bytes at this address.
  std::optional<analysis::NativeReplacementKind> native_replacement;
};

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

struct DriverOptions {
  std::filesystem::path input;
  // Automatic game preparation (Part 11): an already-parsed XexImage - e.g.
  // the result of xbox::apply_title_update(), or simply xbox::parse_xex_image()
  // run directly on bytes read out of a mounted disc image/package - to
  // analyze in place of reading and parsing `input` from disk. When set,
  // load_and_analyze() uses this image verbatim and never touches `input` at
  // all, so a caller that already has the exact effective (optionally
  // title-update-patched) executable in memory never needs to serialize it
  // back out to a temporary file just to hand it to the driver. XEX Loader V2
  // and the driver remain completely unaware of where the bytes originally
  // came from (loose file, ISO, package) either way.
  std::optional<xbox::XexImage> pre_parsed_image;
  std::filesystem::path output{"generated"};
  std::filesystem::path hints_file;
  std::vector<ModuleHint> hints;
  std::vector<const ModuleHintProvider*> module_providers;
  // Analysis Hint Schema V2 (Part 1/2). Either supply a ready-made hint set
  // directly (hint_set_v2) or a provider that resolves one from the
  // effective executable identity computed from `input` (hint_provider_v2) -
  // production callers (the CLI, XenonSession-adjacent tooling) use the
  // latter; tests may use either. When both are given, hint_provider_v2's
  // result takes precedence and hint_set_v2 is ignored (a directly-supplied
  // hint set is meant for tests/manual invocation, not production).
  std::optional<analysis::AnalysisHintSetV2> hint_set_v2;
  const ModuleHintProviderV2* hint_provider_v2{nullptr};
  std::size_t shard_function_count{128};
  bool allow_partial{false};

  // Worker-count configuration (Part 3/16 of the Recomp Analysis V2 pass).
  // nullopt = auto (resolve_worker_count(), roughly hardware_concurrency-1);
  // 1 = deterministic single-thread mode, kept available for debugging;
  // N = an explicit worker count. `analysis_jobs` governs per-function
  // static analysis (load_and_analyze()); `codegen_jobs` governs generated
  // C++ emission (generate_project()). Both default to auto.
  std::optional<std::size_t> analysis_jobs;
  std::optional<std::size_t> codegen_jobs;

  // Generic adaptive-analysis inputs. These are intentionally title-agnostic
  // execution facts, not hard-coded addresses in Xenon. Pointer-table scanning
  // recovers static vtable/function-pointer entries; runtime observations allow
  // a later execution/coverage pass to feed back targets that static analysis
  // could not prove (for example runtime-built tables).
  std::vector<AdaptiveObservation> adaptive_observations;

  // Gen 9 universal knowledge base. Records are versioned, address-independent
  // fingerprints. Matching is confidence-scored and can corroborate a function
  // across title updates without blindly trusting the old guest address.
  std::vector<KnowledgeRecord> knowledge_records;
  std::uint32_t knowledge_match_min_score{70u};
  bool enable_knowledge_seeding{true};
  bool scan_static_pointer_tables{true};
  bool recover_multi_source_orphans{true};
  // Conservative executable-gap recovery: only seeds an unowned region when
  // it begins at a plausible function prologue on a structural boundary and
  // reaches a real terminator before colliding with already-owned code.
  bool recover_unowned_gaps{true};

  // Progress milestones (Part 18). Invoked synchronously from whichever
  // thread is driving the current phase - load_and_analyze()/
  // generate_project() never call it concurrently from two threads at once
  // (progress is aggregated to a single reporting point, not emitted
  // per-worker), so a caller needs no locking of its own. May be empty
  // (the default): both functions run identically either way.
  std::function<void(const std::string&)> progress;
};

[[nodiscard]] bool load_and_analyze(const DriverOptions& options,
                                    AnalysisReport& report,
                                    std::string& error);
[[nodiscard]] bool generate_project(const DriverOptions& options,
                                    AnalysisReport& report,
                                    std::string& error);
// Loads Xenon's newline-delimited runtime observation trace (the
// adaptive-observations.jsonl emitted by runtime_host). Records are
// deduplicated by address/site/kind/owner-hint and hit counts are accumulated.
// The parser also accepts hexadecimal integer literals to make hand-authored
// developer traces convenient.
[[nodiscard]] bool load_adaptive_observations(
    const std::filesystem::path& path,
    std::vector<AdaptiveObservation>& observations,
    std::string& error);
// Stable fingerprint of the DISTINCT adaptive facts consumed by analysis.
// Hit counts are deliberately excluded: seeing the same target again must not
// invalidate a prepared artifact; learning a new address/site/kind/owner fact must.
[[nodiscard]] std::uint64_t adaptive_observation_fingerprint(
    std::span<const AdaptiveObservation> observations) noexcept;
[[nodiscard]] std::string format_report(const AnalysisReport& report);
[[nodiscard]] std::string format_report_json(const AnalysisReport& report);
[[nodiscard]] std::string format_ir(const DiscoveredFunction& function);
[[nodiscard]] const char* discovery_source_name(DiscoverySource source) noexcept;
[[nodiscard]] const char* function_authority_name(FunctionAuthority authority) noexcept;
[[nodiscard]] const char* guest_entry_kind_name(GuestEntryKind kind) noexcept;
[[nodiscard]] const char* return_behavior_name(ReturnBehavior behavior) noexcept;

}  // namespace xenon::recomp
