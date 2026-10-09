#pragma once

// Options for load_and_analyze() and generate_project().

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "xenon/recomp/adaptive_observations.hpp"
#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/compilation_graph.hpp"
#include "xenon/recomp/knowledge_base.hpp"
#include "xenon/recomp/module_hints.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

struct DriverOptions {
  // Empty selects output/.graph; preparation supplies a persistent shared root.
  std::filesystem::path graph_cache;
  graph::Versions graph_versions;
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
  // Byte-size shard budget, an alternative to shard_function_count. When set,
  // shard boundaries are decided by accumulated generated-source size instead
  // of a fixed function count, and shard_function_count is ignored. A single
  // function larger than the budget still gets its own shard rather than
  // being split or producing an empty one. Generated function size varies by
  // orders of magnitude - a handful of functions with large jump-table-driven
  // indirect-branch dispatches can be 100x the size of a typical function -
  // so a fixed function count produces wildly uneven shards (a few
  // multi-hour-to-compile outliers alongside many near-instant ones), while a
  // byte budget keeps every shard's own compile cost roughly bounded and lets
  // the build schedule far more of them in parallel. Unset preserves the
  // existing shard_function_count behavior exactly (no default: a byte
  // budget changes on-disk shard layout, so callers opt in deliberately
  // rather than being silently repartitioned by an upstream default change).
  std::optional<std::size_t> shard_max_bytes;
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

}  // namespace xenon::recomp
