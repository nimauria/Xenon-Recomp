#pragma once

// Working state of analyze_function_candidate() for exactly one candidate
// address, shared by its phases:
//
//   candidate_analysis.cpp  short-circuits, extent bounds, compilation
//   primary_scan.cpp        linear decode of the primary range
//   function_chunks.cpp     declared FunctionChunk (Part 14) ingestion
//   local_cfg_closure.cpp   out-of-line local CFG closure, tail-call promotion
//
// A scan is created, used and destroyed by one worker thread; it only reads
// the shared AnalysisContext.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "recomp/analysis/analysis_internal.hpp"

namespace xenon::recomp::detail {

// Code recovered by local CFG closure outside the primary range and declared
// chunks (see close_local_control_flow()).
struct InferredLocalRange {
  GuestAddress start{};
  std::vector<std::uint32_t> words;
};

struct CandidateScan {
  CandidateScan(GuestAddress start_address, const AnalysisContext& context,
                FunctionAnalysisResult& analysis_result)
      : start(start_address), ctx(context), result(analysis_result) {}

  GuestAddress start;
  const AnalysisContext& ctx;
  FunctionAnalysisResult& result;
  cpu::Decoder decoder;

  DiscoveredFunction function{};
  const xbox::XexSection* section{};
  std::size_t offset{};
  std::size_t max_words{};
  std::vector<std::uint32_t> words;
  std::optional<GuestAddress> metadata_end;
  std::optional<GuestAddress> explicit_function_end;

  std::set<GuestAddress> local_boundaries;
  // Tail-call over-discovery fix: a plain (non-linked) direct branch's
  // target is NEVER pushed straight to `result.discovered` at the point the
  // branch is decoded - unlike a direct call, an ordinary branch is
  // routinely just an internal basic-block edge (if/else, loop, shared
  // epilogue), and this function's own final extent (and any FunctionChunk
  // ranges merged into it) is not known until the whole scan finishes. Every
  // candidate branch target is buffered here instead and only promoted to an
  // actual new-function candidate once the full extent is known (see the
  // filtering pass right after `function.ranges` is assembled below).
  // `bool` = was this the function's own terminal (no-fallthrough) branch -
  // only a terminal branch can plausibly be a tail call; a branch that
  // leaves a live fallthrough path is definitionally intra-procedural
  // control flow and must never seed a function on its own.
  std::vector<std::pair<GuestAddress, bool>> pending_direct_branch_targets;
  std::vector<std::pair<GuestAddress, bool>> pending_chunk_branch_targets;
  std::set<GuestAddress> direct_call_targets;

  // Every discontinuous FunctionChunk owned by this function: decoded words
  // and [start, end) ranges, index-aligned.
  std::vector<std::vector<std::uint32_t>> chunk_words;
  std::vector<std::pair<GuestAddress, GuestAddress>> child_ranges;
  std::vector<InferredLocalRange> inferred_local_ranges;

  // Ownership evidence used by CFG closure and tail-call promotion
  // (local_cfg_closure.cpp).
  [[nodiscard]] bool independently_proven_function_start(GuestAddress target) const;
  [[nodiscard]] bool has_prologue_evidence(GuestAddress target) const;
  [[nodiscard]] bool has_independent_discovery_evidence(GuestAddress target) const;
};

void scan_primary_range(CandidateScan& scan);
void ingest_function_chunks(CandidateScan& scan);
void close_local_control_flow(CandidateScan& scan);
void promote_tail_call_targets(CandidateScan& scan);

}  // namespace xenon::recomp::detail
