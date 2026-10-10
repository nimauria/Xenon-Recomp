#include <algorithm>

#include "recomp/analysis/analysis_internal.hpp"

namespace xenon::recomp::detail {

bool hinted_non_code(const AnalysisContext& ctx, GuestAddress address) {
  for (const auto& hint : ctx.hints)
    if (std::find(hint.data_regions.begin(), hint.data_regions.end(), address) != hint.data_regions.end() ||
        std::find(hint.ignored_regions.begin(), hint.ignored_regions.end(), address) !=
            hint.ignored_regions.end())
      return true;
  if (ctx.hint_set_v2)
    for (const auto& region : ctx.hint_set_v2->regions)
      if (region.kind != analysis::RegionKind::CodeOverride && address >= region.start &&
          address < region.end)
        return true;
  return false;
}

const analysis::InstructionPatternHint* instruction_pattern_match(const AnalysisContext& ctx,
                                                                   GuestAddress address,
                                                                   std::uint32_t word) {
  if (!ctx.hint_set_v2) return nullptr;
  for (const auto& pattern : ctx.hint_set_v2->instruction_patterns) {
    if ((word & pattern.mask) != (pattern.value & pattern.mask)) continue;
    if (pattern.scope_start && pattern.scope_end &&
        (address < *pattern.scope_start || address >= *pattern.scope_end))
      continue;
    return &pattern;
  }
  return nullptr;
}

// Part 2 (generated-code deduplication / shard ownership fix): true iff
// `address` has its own explicit, parentless FunctionHint - i.e. hint data
// itself declares this address an independent top-level function, regardless
// of any FunctionChunk that also happens to claim it as a child range.
// Shared by canonicalize_candidate() below (decides which address a raw
// discovery actually gets analyzed/compiled under) and
// analyze_function_candidate()'s FunctionChunk-ingestion loop (decides which
// chunks a parent may stitch into its own compiled body) so the two
// decisions can never disagree with each other - disagreement between them
// is exactly what used to let the same guest bytes be compiled twice, once
// under the parent's canonical identity and once under the independent
// function's own.
bool has_independent_function_hint(const analysis::AnalysisHintSetV2* hint_set_v2, GuestAddress address) {
  if (!hint_set_v2) return false;
  return std::any_of(hint_set_v2->functions.begin(), hint_set_v2->functions.end(),
                     [address](const auto& hint) {
                       return hint.address == address && !hint.parent_function.has_value();
                     });
}

// Resolves a raw candidate address to the address that should actually be
// claimed/analyzed (Part 2 Phase D / Part 14): a FunctionChunk's child range
// is owned by its declared parent (unless that exact address is ALSO an
// independent top-level FunctionHint), so any discovery that lands inside
// one redirects to the parent instead of creating a second, unrelated host
// function - exactly the behavior the previous serial algorithm applied
// each time it popped an address off its worklist, just performed here
// before an address is ever claimed for a wave rather than inside the
// per-function analysis itself. Bounded against a cyclic/self-referential
// hint set (Part 5 "avoid infinite discovery loops"); `cyclic` is set true
// if the bound is hit, so the caller can surface it as a visible warning
// rather than silently truncating the redirect.
GuestAddress canonicalize_candidate(GuestAddress start, const analysis::AnalysisHintSetV2* hint_set_v2,
                                    bool& cyclic) {
  cyclic = false;
  if (!hint_set_v2) return start;
  for (int guard = 0; guard < 256; ++guard) {
    const auto owning_chunk =
        std::find_if(hint_set_v2->chunks.begin(), hint_set_v2->chunks.end(),
                     [start](const auto& chunk) { return start >= chunk.start && start < chunk.end; });
    if (owning_chunk == hint_set_v2->chunks.end()) return start;
    if (has_independent_function_hint(hint_set_v2, start)) return start;
    if (owning_chunk->parent_function == start) return start;
    start = owning_chunk->parent_function;
  }
  cyclic = true;
  return start;
}

// Xbox 360 import binding (Part 1/1.11 extension): an XEX-native import
// record's callable guest_thunk address is loader-owned placeholder data
// (ordinal/attributes/record-type - see xex_loader.cpp's
// parse_native_import_libraries()), not guest PPC bytes. Only
// XexImportKind::FunctionThunk/PeFunction records are actual callable
// dispatch targets (XexImport::callable()); a type-0/FunctionAddress slot is
// metadata bound to a variable, never a branch target. Shared by
// analyze_function_candidate()'s decode short-circuit below and
// load_and_analyze()'s adaptive-observation/knowledge-seed filtering.
const xbox::XexImport* find_callable_import_thunk(const xbox::XexImage& image, GuestAddress address) {
  for (const auto& import : image.imports) {
    if (import.callable() && import.guest_thunk == address) return &import;
  }
  return nullptr;
}

}  // namespace xenon::recomp::detail
