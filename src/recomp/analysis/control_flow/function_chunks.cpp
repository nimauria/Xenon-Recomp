#include <algorithm>
#include <sstream>

#include "recomp/analysis/control_flow/candidate_scan.hpp"
#include "recomp/driver/driver_support.hpp"
#include "xenon/recomp/ppc_value_analysis.hpp"

namespace xenon::recomp::detail {

void ingest_function_chunks(CandidateScan& scan) {
  const auto start = scan.start;
  const auto& ctx = scan.ctx;
  auto& result = scan.result;
  const auto& decoder = scan.decoder;
  auto& function = scan.function;
  auto& pending_chunk_branch_targets = scan.pending_chunk_branch_targets;
  auto& direct_call_targets = scan.direct_call_targets;
  auto& chunk_words = scan.chunk_words;
  auto& child_ranges = scan.child_ranges;
  // Read every discontinuous FunctionChunk owned by this parent (Part 14)
  // and make it part of the same compiler input.
  if (ctx.hint_set_v2) {
    const auto owned_count = static_cast<std::size_t>(
        std::count_if(ctx.hint_set_v2->chunks.begin(), ctx.hint_set_v2->chunks.end(),
                      [start](const auto& chunk) { return chunk.parent_function == start; }));
    chunk_words.reserve(owned_count);
    child_ranges.reserve(owned_count);
    for (const auto& chunk : ctx.hint_set_v2->chunks) {
      if (chunk.parent_function != start) continue;
      // Part 6/7 (generated-code deduplication / shard ownership fix): a
      // chunk address that ALSO has its own independent top-level
      // FunctionHint (canonicalize_candidate() deliberately lets that hint
      // win over the chunk redirect - see has_independent_function_hint()'s
      // doc comment) must never ALSO be stitched into this parent's
      // compiled body. Without this guard the exact same guest bytes get
      // compiled twice under two different canonical identities - this
      // parent's name, and the independent function's own name - a real
      // violation of the one-address/one-canonical-function invariant, even
      // though the two resulting C++ symbols differ (so it never showed up
      // as a duplicate-body compile error the way the codegen cache-key
      // collision below did). Excluded, not merged: explicit hint data
      // marking an address independent is authoritative over an unrelated
      // chunk declaration that happens to also claim it.
      if (has_independent_function_hint(ctx.hint_set_v2, chunk.start)) {
        result.warnings.push_back(
            "FunctionChunk at 0x" + hex_string(chunk.start) + " declares parent 0x" + hex_string(start) +
            " but 0x" + hex_string(chunk.start) +
            " also has its own independent FunctionHint; excluded from the parent's merged body so "
            "each guest address compiles under exactly one canonical function");
        continue;
      }
      const auto* chunk_section = executable_section(ctx.range_index, chunk.start);
      const auto chunk_size = static_cast<std::uint64_t>(chunk.end) - chunk.start;
      if (!chunk_section || chunk.end <= chunk.start || (chunk.start & 3u) != 0u ||
          (chunk.end & 3u) != 0u ||
          static_cast<std::uint64_t>(chunk.start - chunk_section->virtual_address) + chunk_size >
              chunk_section->bytes.size()) {
        function.error = "FunctionChunk range is outside executable section bytes";
        result.unresolved.push_back({chunk.start, chunk.end, "function-chunk", function.error});
        break;
      }

      auto& storage = chunk_words.emplace_back();
      storage.reserve(static_cast<std::size_t>(chunk_size / 4u));
      const auto chunk_offset = static_cast<std::size_t>(chunk.start - chunk_section->virtual_address);
      auto chunk_end_used = chunk.end;
      value_analysis::PpcValueTracker chunk_value_tracker(ctx.image);
      std::vector<cpu::DecodedInstruction> chunk_value_history;
      chunk_value_history.reserve(std::min<std::size_t>(chunk_size / 4u, 96u));
      for (GuestAddress address = chunk.start; address < chunk.end; address += 4u) {
        const auto word = be32(chunk_section->bytes, chunk_offset + (address - chunk.start));
        if (address != chunk.start) {
          if (const auto* pattern = instruction_pattern_match(ctx, address, word)) {
            ++result.instruction_pattern_matches;
            result.warnings.push_back(
                "FunctionChunk scan stopped at instruction-pattern match 0x" +
                [&] { std::ostringstream s; s << std::hex << address; return s.str(); }() +
                (pattern->reason.empty() ? "" : " (" + pattern->reason + ")"));
            chunk_end_used = address;
            break;
          }
        }
        const auto instruction = decoder.decode(address, word);
        if (!instruction.valid()) {
          const std::string classification = classify_decode_failure(word);
          function.error = (classification == "invalid-ppc" ? "invalid PPC in FunctionChunk at 0x"
                                                              : "unsupported PPC encoding in FunctionChunk at 0x") +
              [&] { std::ostringstream text; text << std::hex << address; return text.str(); }();
          result.unresolved.push_back({address, word, classification, function.error});
          break;
        }
        storage.push_back(word);
        if (instruction.mnemonic() == "bclrx" && !instruction.lk() && is_terminal(instruction))
          function.has_explicit_return = true;
        if (instruction.info->group == cpu::InstructionGroup::Branch) {
          if (instruction.info->format == cpu::InstructionFormat::I ||
              instruction.info->format == cpu::InstructionFormat::B) {
            const auto target = instruction.direct_branch_target();
            if (instruction.lk()) {
              function.calls.push_back(target);
              function.branches.push_back({address, target, false, false, true, false, true});
              direct_call_targets.insert(target);
              result.discovered.push_back({target, DiscoverySource::DirectCall});
            } else {
              // Same over-discovery fix as the primary scan above: buffered,
              // filtered once this function's complete extent (primary range +
              // every chunk) is known, not pushed unconditionally here.
              function.branch_references.push_back(target);
              function.branches.push_back({address, target, is_terminal(instruction), false, false,
                                           is_conditional_control_transfer(instruction),
                                           !is_terminal(instruction)});
              pending_chunk_branch_targets.push_back({target, is_terminal(instruction)});
            }
          } else if (instruction.info->format == cpu::InstructionFormat::XL &&
                     !(instruction.mnemonic() == "bclrx" && !instruction.lk())) {
            const auto resolution = value_analysis::resolve_indirect_flow(
                instruction, chunk_value_tracker, chunk_value_history, ctx.image);
            if (!resolution.targets.empty()) {
              ++result.resolved_indirect_via_dataflow;
              if (resolution.kind == value_analysis::IndirectResolverKind::BackwardSlice)
                ++result.resolved_indirect_via_backward_slice;
              if (resolution.kind == value_analysis::IndirectResolverKind::ReadOnlyTable)
                ++result.resolved_indirect_via_readonly_table;
              for (const auto target : resolution.targets) {
                if (instruction.lk()) {
                  function.calls.push_back(target);
                  function.branches.push_back({address, target, false, true, true,
                                               is_conditional_control_transfer(instruction), true});
                  result.discovered.push_back({target, DiscoverySource::ResolvedIndirectCall});
                } else {
                  function.branch_references.push_back(target);
                  function.branches.push_back({address, target, is_terminal(instruction), true, false,
                                               is_conditional_control_transfer(instruction),
                                               !is_terminal(instruction)});
                  result.discovered.push_back({target, DiscoverySource::ResolvedIndirectBranch});
                }
              }
            } else {
              result.unresolved.push_back(
                  {address, 0u, instruction.lk() ? "indirect-call" : "indirect-branch",
                   "target depends on runtime register state in FunctionChunk"});
            }
          }
        }

        // FunctionChunk ranges may contain more than one compiler block. A
        // terminal transfer cuts abstract state so facts are never propagated
        // through unreachable bytes merely because the module declared one
        // contiguous chunk range.
        if (is_terminal(instruction)) {
          chunk_value_tracker.reset();
          chunk_value_history.clear();
        } else {
          chunk_value_history.push_back(instruction);
          if (chunk_value_history.size() > 96u)
            chunk_value_history.erase(chunk_value_history.begin(), chunk_value_history.begin() + 32);
          chunk_value_tracker.step(instruction);
        }
      }
      if (!function.error.empty()) break;
      child_ranges.emplace_back(chunk.start, chunk_end_used);
    }
  }
}

}  // namespace xenon::recomp::detail
