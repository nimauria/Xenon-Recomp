#include <algorithm>
#include <span>
#include <sstream>

#include "recomp/analysis/control_flow/candidate_scan.hpp"
#include "recomp/driver/driver_support.hpp"
#include "xenon/recomp/native_replacements.hpp"

namespace xenon::recomp::detail {
namespace {

// Candidates that are never decoded: Xenon-implemented runtime helpers,
// native replacements and callable XEX import thunks. Returns true when
// `result` is final.
bool try_short_circuit_candidate(GuestAddress start, const AnalysisContext& ctx,
                                 FunctionAnalysisResult& result) {
  // Runtime-helper short-circuit (Part 1.5/1.9): these guest addresses are
  // deliberately executed by Xenon's own generic runtime helper
  // implementations - discovery must not also decode/compile the original
  // guest helper body.
  const auto helper_it =
      std::find_if(ctx.expanded_runtime_helpers.begin(), ctx.expanded_runtime_helpers.end(),
                   [start](const auto& helper) { return helper.address == start; });
  if (helper_it != ctx.expanded_runtime_helpers.end()) {
    result.outcome = FunctionAnalysisResult::Outcome::Skipped;
    return true;
  }

  // Native replacement short-circuit (Part 1.10/1.11).
  if (ctx.hint_set_v2) {
    const auto replacement_it =
        std::find_if(ctx.hint_set_v2->native_replacements.begin(), ctx.hint_set_v2->native_replacements.end(),
                     [start](const auto& r) { return r.guest_address == start; });
    if (replacement_it != ctx.hint_set_v2->native_replacements.end()) {
      if (native_replacements::entry_for(replacement_it->kind) != nullptr) {
        DiscoveredFunction function{};
        function.guest_start = start;
        function.guest_end = start;
        function.name = !replacement_it->source_name.empty()
                            ? sanitize_cpp_name(replacement_it->source_name, start)
                            : cpp_name(start);
        add_source(function, DiscoverySource::ModuleHint);
        function.native_replacement = replacement_it->kind;
        function.compiled = true;
        function.confidence = 100;
        function.authority = FunctionAuthority::ModuleHint;
        function.ranges = {start, start};
        result.function = std::move(function);
        result.outcome = FunctionAnalysisResult::Outcome::Accepted;
        return true;
      }
      result.warnings.push_back(
          "native replacement '" + replacement_it->source_name + "' (" +
          std::string(analysis::native_replacement_kind_name(replacement_it->kind)) + ") at 0x" +
          [&] { std::ostringstream s; s << std::hex << start; return s.str(); }() +
          " is a recognized identity with no available Xenon implementation yet; analyzing the "
          "guest bytes normally instead");
      // Falls through to normal discovery/compilation below.
    }
  }

  // Import-thunk short-circuit: a candidate landing exactly on an XEX-native
  // import record's callable guest_thunk address is loader-owned placeholder
  // data (see find_callable_import_thunk()'s comment above), never guest PPC
  // bytes - decoding it is a static-analysis false positive
  // ("unsupported"/"invalid PPC encoding" for what is really an unresolved
  // import metadata word), and in the worst case a coincidentally-valid
  // decode could register a bogus compiled function that shadows the
  // correct runtime import dispatch. Xenon's runtime (XenonSession::call(),
  // src/core/session/execution/runtime_services.cpp) already matches call targets against
  // XexImage::imports[].guest_thunk before falling back to compiled code, so
  // this candidate deliberately stays uncompiled (`compiled = false`): no
  // lookup_compiled() case gets emitted for it, and the existing
  // lookup_compiled-miss -> try_dynamic_fallback-miss -> runtime.call()
  // chain reaches that correct dispatch unshadowed.
  if (const auto* import = find_callable_import_thunk(ctx.image, start)) {
    DiscoveredFunction function{};
    function.guest_start = start;
    function.guest_end = start;
    function.name = sanitize_cpp_name(
        !import->symbol.empty() ? import->symbol : (import->module + "_" + std::to_string(import->ordinal)), start);
    add_source(function, DiscoverySource::ModuleHint);
    function.import_thunk = DiscoveredFunction::ImportThunkBinding{import->module, import->symbol, import->ordinal};
    function.compiled = false;
    function.confidence = 100;
    function.authority = FunctionAuthority::ModuleHint;
    function.ranges = {start, start};
    result.function = std::move(function);
    result.outcome = FunctionAnalysisResult::Outcome::Accepted;
    return true;
  }

  return false;
}

// Validates the candidate lies in executable section bytes, seeds its
// identity/provenance and computes the soft (unwind/hint) and hard (explicit
// FunctionHint end/size) limits of its primary range.
bool establish_scan_bounds(CandidateScan& scan) {
  const auto start = scan.start;
  const auto& ctx = scan.ctx;
  auto& result = scan.result;
  const auto* section = executable_section(ctx.range_index, start);
  if (!section || start < section->virtual_address ||
      static_cast<std::uint64_t>(start - section->virtual_address) + 4 > section->bytes.size()) {
    result.unresolved.push_back({start, start, "function", "function start is outside section bytes"});
    result.outcome = FunctionAnalysisResult::Outcome::Rejected;
    return false;
  }
  scan.section = section;

  auto& function = scan.function;
  function.guest_start = start;
  function.name = ctx.known_names.contains(start) ? sanitize_cpp_name(ctx.known_names.at(start), start)
                                                   : cpp_name(start);
  // Part 1 (Recomp Analysis V3): attach every piece of evidence that led
  // here - an original seed's source, plus every DiscoverySource any
  // discoverer(s) tagged this address with across earlier waves. A
  // candidate reached only through decode-time discovery with no evidence
  // recorded at all (should not normally happen - every discovery call site
  // tags a source) falls back to DirectBranch, matching the previous
  // algorithm's behavior for that edge case.
  if (ctx.seeds.contains(start)) add_source(function, ctx.seeds.at(start));
  if (const auto evidence = ctx.discovered_evidence.find(start); evidence != ctx.discovered_evidence.end())
    for (const auto source : evidence->second) add_source(function, source);
  if (function.sources.empty()) add_source(function, DiscoverySource::DirectBranch);
  scan.offset = static_cast<std::size_t>(start - section->virtual_address);
  scan.max_words = std::min<std::size_t>((section->bytes.size() - scan.offset) / 4, 4096);
  auto& metadata_end = scan.metadata_end;
  auto& explicit_function_end = scan.explicit_function_end;
  for (const auto& metadata : ctx.image.function_metadata) {
    if (metadata.valid && metadata.begin == start) {
      metadata_end = metadata.end;
      add_source(function, DiscoverySource::UnwindMetadata);
      break;
    }
  }
  for (const auto& hint : ctx.hints)
    for (const auto boundary : hint.function_boundaries)
      if (boundary > start && (!metadata_end || boundary < *metadata_end)) metadata_end = boundary;
  if (ctx.hint_set_v2) {
    // This function's own explicit end/size (Part 1.4) takes precedence
    // over anything inferred from other hints' addresses below.
    for (const auto& function_hint : ctx.hint_set_v2->functions) {
      if (function_hint.address != start) continue;
      if (function_hint.end) {
        metadata_end = *function_hint.end;
        explicit_function_end = *function_hint.end;
      } else if (function_hint.size) {
        metadata_end = start + *function_hint.size;
        explicit_function_end = start + *function_hint.size;
      }
      if (analysis::has_flag(function_hint.flags, analysis::FunctionFlags::NoReturn)) {
        function.return_behavior = ReturnBehavior::NoReturn;
        function.return_behavior_explicit = true;
      }
      break;
    }
    const auto consider_boundary = [&](GuestAddress boundary) {
      if (boundary > start && (!metadata_end || boundary < *metadata_end)) metadata_end = boundary;
    };
    for (const auto& function_hint : ctx.hint_set_v2->functions) consider_boundary(function_hint.address);
    for (const auto& chunk : ctx.hint_set_v2->chunks) consider_boundary(chunk.start);
    for (const auto& replacement : ctx.hint_set_v2->native_replacements)
      consider_boundary(replacement.guest_address);
  }
  return true;
}

// Compiles the assembled ranges through the content-addressed compilation
// graph and publishes the accepted (or rejected) function.
void finalize_candidate(CandidateScan& scan) {
  const auto start = scan.start;
  const auto& ctx = scan.ctx;
  auto& result = scan.result;
  auto& function = scan.function;
  const auto& words = scan.words;
  const auto& chunk_words = scan.chunk_words;
  const auto& child_ranges = scan.child_ranges;
  const auto& inferred_local_ranges = scan.inferred_local_ranges;
  const auto& local_boundaries = scan.local_boundaries;
  if (words.empty() || !function.error.empty()) {
    if (!ctx.allow_partial) {
      result.outcome = FunctionAnalysisResult::Outcome::Rejected;
      return;
    }
    // allow_partial: fall through and still accept this (uncompiled)
    // function, matching the previous algorithm exactly.
  } else {
    std::vector<cpu::FunctionCodeRange> compile_ranges;
    compile_ranges.reserve(1u + child_ranges.size() + inferred_local_ranges.size());
    compile_ranges.push_back({start, words});
    for (std::size_t i = 0; i < child_ranges.size(); ++i)
      compile_ranges.push_back({child_ranges[i].first, chunk_words[i]});
    for (const auto& range : inferred_local_ranges)
      compile_ranges.push_back({range.start, range.words});

    auto region = graph::compile_region(graph::Store(ctx.graph_cache), ctx.graph_versions, start, compile_ranges);
    function.compilation_nodes = std::move(region.nodes);
    function.ir_cache_hit = region.ir_hit;
    const auto& compile_result = region.compiled;
    if (!compile_result.ok) {
      // NOTE: a compile failure (as opposed to a decode failure above) is
      // always recorded as an uncompiled DiscoveredFunction entry
      // regardless of allow_partial - matches the previous algorithm, which
      // only gated the decode-failure/empty-words case on allow_partial.
      function.error = compile_result.error;
      result.unresolved.push_back(
          {compile_result.error_address, compile_result.error_word, "compile", compile_result.error});
    } else {
      function.ir = compile_result.function;
      function.compiled = true;
      // Part 11 (Recomp Analysis V3): confidence is evidence-based
      // (confidence_for_sources()), not the single arbitrary
      // internal-branch-boundary check this used to be alone. That signal
      // is kept as a secondary modifier - a function whose own scan crossed
      // an address that was ALSO independently discovered as a branch
      // target suggests possible boundary ambiguity worth a small penalty,
      // regardless of how strong the function's own provenance is.
      function.confidence = confidence_for_sources(function.sources);
      function.authority = authority_for_sources(function.sources);
      if (!local_boundaries.empty())
        function.confidence = function.confidence > 10u ? function.confidence - 10u : 0u;
      auto source_hash = hash_bytes(std::as_bytes(std::span(words)), ctx.configuration_hash);
      for (std::size_t i = 0; i < child_ranges.size(); ++i) {
        source_hash = hash_bytes(std::as_bytes(std::span(&child_ranges[i].first, 1)), source_hash);
        source_hash = hash_bytes(std::as_bytes(std::span(chunk_words[i])), source_hash);
      }
      for (const auto& range : inferred_local_ranges) {
        source_hash = hash_bytes(std::as_bytes(std::span(&range.start, 1)), source_hash);
        source_hash = hash_bytes(std::as_bytes(std::span(range.words)), source_hash);
      }
      function.source_hash = source_hash;
    }
  }

  if (function.has_explicit_return && !function.return_behavior_explicit)
    function.return_behavior = ReturnBehavior::MayReturn;
  if (function.confidence == 0u) function.confidence = confidence_for_sources(function.sources);
  function.authority = authority_for_sources(function.sources);
  result.function = std::move(function);
  result.outcome = FunctionAnalysisResult::Outcome::Accepted;
}

}  // namespace

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
FunctionAnalysisResult analyze_function_candidate(GuestAddress start, const AnalysisContext& ctx) {
  FunctionAnalysisResult result;
  if (try_short_circuit_candidate(start, ctx, result)) return result;

  CandidateScan scan(start, ctx, result);
  if (!establish_scan_bounds(scan)) return result;
  scan_primary_range(scan);
  ingest_function_chunks(scan);
  close_local_control_flow(scan);
  promote_tail_call_targets(scan);
  finalize_candidate(scan);
  return result;
}

}  // namespace xenon::recomp::detail
