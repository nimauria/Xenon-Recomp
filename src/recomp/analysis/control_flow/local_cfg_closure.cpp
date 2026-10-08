#include <algorithm>
#include <cstdint>

#include "recomp/analysis/control_flow/candidate_scan.hpp"
#include "recomp/driver/driver_support.hpp"
#include "xenon/recomp/ppc_value_analysis.hpp"

namespace xenon::recomp::detail {

bool CandidateScan::independently_proven_function_start(GuestAddress target) const {
  if (const auto seed = ctx.seeds.find(target); seed != ctx.seeds.end() &&
      source_is_semantic_boundary(seed->second))
    return true;
  for (const auto& metadata : ctx.image.function_metadata)
    if (metadata.valid && metadata.begin == target) return true;
  if (ctx.hint_set_v2)
    for (const auto& function_hint : ctx.hint_set_v2->functions)
      if (function_hint.address == target) return true;
  return false;
}

bool CandidateScan::has_prologue_evidence(GuestAddress target) const {
  const auto* target_section = executable_section(ctx.range_index, target);
  if (!target_section || target < target_section->virtual_address ||
      static_cast<std::uint64_t>(target - target_section->virtual_address) + 4u >
          target_section->bytes.size())
    return false;
  const auto target_offset = static_cast<std::size_t>(target - target_section->virtual_address);
  const auto target_instruction = decoder.decode(target, be32(target_section->bytes, target_offset));
  return looks_like_function_prologue(target_instruction);
}

bool CandidateScan::has_independent_discovery_evidence(GuestAddress target) const {
  if (independently_proven_function_start(target)) return true;
  if (direct_call_targets.contains(target)) return true;
  // A callable XEX-native import thunk is independently, structurally
  // confirmed by the XEX's own import metadata (ctx.image.imports) - never
  // a guess. Without this, a thunk reached ONLY by a non-linked branch/
  // tail-call (no `bl` anywhere providing DirectCall evidence) would never
  // get promoted to its own candidate by promote_terminal_target() below,
  // and worse, queue_local_target()/the inferred-local-range materializer
  // above could try to inline its placeholder bytes as local code inside
  // the BRANCHING function's own body - the same "decode loader metadata
  // as PPC" bug analyze_function_candidate()'s import-thunk short-circuit
  // already prevents for direct candidates, reached here through a
  // different path. Real AC6 repro: xboxkrnl.exe ordinal 197's thunk at
  // 0x823d00cc is reached only by a lone non-linked branch at 0x821f4120.
  if (find_callable_import_thunk(ctx.image, target)) return true;
  const auto evidence = ctx.discovered_evidence.find(target);
  if (evidence == ctx.discovered_evidence.end()) return false;
  return std::any_of(evidence->second.begin(), evidence->second.end(), [](DiscoverySource source) {
    return source == DiscoverySource::DirectCall ||
           source == DiscoverySource::ResolvedIndirectCall ||
           source == DiscoverySource::EntryPoint || source == DiscoverySource::Export ||
           source == DiscoverySource::UnwindMetadata || source == DiscoverySource::ModuleHint ||
           source == DiscoverySource::TlsCallback;
  });
}

void close_local_control_flow(CandidateScan& scan) {
  const auto start = scan.start;
  const auto& ctx = scan.ctx;
  auto& result = scan.result;
  const auto& decoder = scan.decoder;
  auto& function = scan.function;
  auto& words = scan.words;
  auto& local_boundaries = scan.local_boundaries;
  auto& pending_direct_branch_targets = scan.pending_direct_branch_targets;
  const auto& pending_chunk_branch_targets = scan.pending_chunk_branch_targets;
  auto& direct_call_targets = scan.direct_call_targets;
  const auto& child_ranges = scan.child_ranges;
  const auto& explicit_function_end = scan.explicit_function_end;
  auto& inferred_local_ranges = scan.inferred_local_ranges;
  const auto has_independent_discovery_evidence = [&scan](GuestAddress target) {
    return scan.has_independent_discovery_evidence(target);
  };

  // A non-linking conditional branch always has a live fallthrough path,
  // so its direct target is intra-procedural control flow unless another
  // function/chunk already has independent ownership evidence.  The linear
  // primary scan above can legitimately stop before such a target (for
  // example at an unwind-range end or on a terminal instruction in the
  // fallthrough arm).  Materialize those out-of-line reachable blocks as
  // sparse ranges of this same logical function instead of manufacturing a
  // synthetic top-level function or leaving an executable edge unowned.
  //
  // Forward unconditional non-link branches participate in this closure too:
  // crossing a tentative extent is not sufficient evidence of a tail call. A
  // destination remains external only when independent semantic evidence proves
  // another function boundary (or an explicit FunctionHint end forbids growth).
  // Backward targets before this function's start remain conservative external
  // candidates because absorbing them would merge already-preceding code.
  const auto inside_materialized_extent = [&](GuestAddress target) {
    if (target >= function.guest_start && target < function.guest_end) return true;
    for (const auto& [range_start, range_end] : child_ranges)
      if (target >= range_start && target < range_end) return true;
    for (const auto& range : inferred_local_ranges) {
      const auto range_end = static_cast<std::uint64_t>(range.start) +
                             static_cast<std::uint64_t>(range.words.size()) * 4u;
      if (target >= range.start && static_cast<std::uint64_t>(target) < range_end) return true;
    }
    return false;
  };

  // An explicit end/size on this function's own FunctionHint is a hard
  // module-authored contract.  Generic CFG closure may cross a soft XEX
  // unwind extent, but it must never silently violate an explicit module
  // range.  A title that needs out-of-line code past such an end should
  // describe it with FunctionChunk metadata.
  const auto crosses_explicit_function_end = [&](GuestAddress target) {
    return explicit_function_end && target >= *explicit_function_end;
  };

  std::set<GuestAddress> local_worklist;
  const auto queue_local_target = [&](GuestAddress target) {
    if (!ExecutableRangeIndex::is_aligned_ppc_address(target)) return;
    if (!ctx.range_index.is_executable_address(target)) return;
    if (inside_materialized_extent(target)) return;
    if (crosses_explicit_function_end(target)) return;
    if (has_independent_discovery_evidence(target)) return;
    local_worklist.insert(target);
  };

  // Reachability closes before semantic function extents are sealed. In
  // particular, a forward non-linked branch is NOT a tail call merely
  // because it crosses the current tentative range end (Xenia explicitly
  // avoids that aggressive heuristic). Known indirect branch/switch targets
  // are treated the same way. Only independently proven function boundaries
  // remain external. Backward targets before this function start are left
  // external unless stronger evidence says otherwise.
  for (const auto& branch : function.branches) {
    if (branch.linked || branch.target < function.guest_start) continue;
    queue_local_target(branch.target);
  }
  for (const auto& [target, terminal] : pending_chunk_branch_targets) {
    (void)terminal;
    if (target >= function.guest_start) queue_local_target(target);
  }

  constexpr std::size_t kMaxInferredLocalWords = 4096u;
  std::size_t inferred_local_word_count = 0u;
  while (!local_worklist.empty()) {
    const auto block_start = *local_worklist.begin();
    local_worklist.erase(local_worklist.begin());
    if (inside_materialized_extent(block_start)) continue;
    if (crosses_explicit_function_end(block_start)) continue;
    if (has_independent_discovery_evidence(block_start)) continue;

    const auto* block_section = executable_section(ctx.range_index, block_start);
    if (!block_section || block_start < block_section->virtual_address ||
        static_cast<std::uint64_t>(block_start - block_section->virtual_address) + 4u >
            block_section->bytes.size())
      continue;

    InferredLocalRange inferred{};
    inferred.start = block_start;
    const auto block_offset = static_cast<std::size_t>(block_start - block_section->virtual_address);
    const auto block_max_words = std::min<std::size_t>(
        (block_section->bytes.size() - block_offset) / 4u,
        kMaxInferredLocalWords - inferred_local_word_count);
    value_analysis::PpcValueTracker inferred_value_tracker(ctx.image);
    std::vector<cpu::DecodedInstruction> inferred_value_history;
    inferred_value_history.reserve(std::min<std::size_t>(block_max_words, 96u));

    for (std::size_t index = 0; index < block_max_words; ++index) {
      const auto address = static_cast<GuestAddress>(block_start + index * 4u);
      if (index != 0u) {
        if (inside_materialized_extent(address) || crosses_explicit_function_end(address) ||
            has_independent_discovery_evidence(address))
          break;
        if (hinted_non_code(ctx, address)) {
          result.warnings.push_back(
              "inferred local CFG range stopped at hinted non-code region 0x" + hex_string(address));
          break;
        }
      }

      const auto word = be32(block_section->bytes, block_offset + index * 4u);
      if (index != 0u) {
        if (const auto* pattern = instruction_pattern_match(ctx, address, word)) {
          ++result.instruction_pattern_matches;
          result.warnings.push_back(
              "inferred local CFG range stopped at instruction-pattern match 0x" +
              hex_string(address) +
              (pattern->reason.empty() ? "" : " (" + pattern->reason + ")"));
          break;
        }
      }

      const auto instruction = decoder.decode(address, word);
      if (!instruction.valid()) {
        const std::string classification = classify_decode_failure(word);
        result.unresolved.push_back(
            {address, word, classification,
             std::string(classification == "invalid-ppc" ? "invalid PPC in inferred local CFG range at 0x"
                                                          : "unsupported PPC encoding in inferred local CFG range at 0x") +
                 hex_string(address)});
        break;
      }

      inferred.words.push_back(word);
      ++inferred_local_word_count;

      if (instruction.info->group == cpu::InstructionGroup::Branch) {
        const bool terminal_branch = is_terminal(instruction);
        if (instruction.mnemonic() == "bclrx" && !instruction.lk() && terminal_branch)
          function.has_explicit_return = true;
        if (instruction.lk() &&
            (instruction.info->format == cpu::InstructionFormat::I ||
             instruction.info->format == cpu::InstructionFormat::B)) {
          const auto target = instruction.direct_branch_target();
          function.calls.push_back(target);
          function.branches.push_back({address, target, false, false, true, false, true});
          direct_call_targets.insert(target);
          result.discovered.push_back({target, DiscoverySource::DirectCall});
        } else if (!instruction.lk() &&
                   (instruction.info->format == cpu::InstructionFormat::I ||
                    instruction.info->format == cpu::InstructionFormat::B)) {
          const auto target = instruction.direct_branch_target();
          function.branch_references.push_back(target);
          function.branches.push_back({address, target, terminal_branch, false, false,
                                         is_conditional_control_transfer(instruction), !terminal_branch});
          local_boundaries.insert(target);
          pending_direct_branch_targets.push_back({target, terminal_branch});
          if (target >= function.guest_start) queue_local_target(target);
        } else if (instruction.info->format == cpu::InstructionFormat::XL &&
                   !(instruction.mnemonic() == "bclrx" && !instruction.lk())) {
          const auto resolution = value_analysis::resolve_indirect_flow(
              instruction, inferred_value_tracker, inferred_value_history, ctx.image);
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
                direct_call_targets.insert(target);
                result.discovered.push_back({target, DiscoverySource::ResolvedIndirectCall});
              } else {
                function.branch_references.push_back(target);
                function.branches.push_back({address, target, terminal_branch, true, false,
                                             is_conditional_control_transfer(instruction),
                                             !terminal_branch});
                local_boundaries.insert(target);
                result.discovered.push_back({target, DiscoverySource::ResolvedIndirectBranch});
                if (target >= function.guest_start) queue_local_target(target);
              }
            }
          } else {
            result.unresolved.push_back(
                {address, 0u, instruction.lk() ? "indirect-call" : "indirect-branch",
                 "target depends on runtime register state in inferred local CFG range"});
          }
        }

        if (terminal_branch) break;
      }

      inferred_value_history.push_back(instruction);
      if (inferred_value_history.size() > 96u)
        inferred_value_history.erase(inferred_value_history.begin(),
                                     inferred_value_history.begin() + 32);
      inferred_value_tracker.step(instruction);

      if (inferred_local_word_count >= kMaxInferredLocalWords) break;
    }

    if (!inferred.words.empty()) inferred_local_ranges.push_back(std::move(inferred));
    if (inferred_local_word_count >= kMaxInferredLocalWords) {
      result.warnings.push_back(
          "inferred local CFG closure reached its 4096-instruction safety bound for function 0x" +
          hex_string(start));
      break;
    }
  }

  // If CFG closure recovered code beginning exactly at the primary
  // range's tentative exclusive end, it is not genuinely discontinuous.
  // Fold that adjacent block (and any subsequently adjacent recovered block)
  // back into the primary range.  Besides producing a cleaner ownership
  // model, this prevents analysis.json from continuing to advertise the old
  // broken shape `direct target == function.end` after that target has been
  // proven to be reachable continuation of the same function.
  for (;;) {
    const auto adjacent = std::find_if(
        inferred_local_ranges.begin(), inferred_local_ranges.end(),
        [&](const InferredLocalRange& range) { return range.start == function.guest_end; });
    if (adjacent == inferred_local_ranges.end()) break;
    words.insert(words.end(), adjacent->words.begin(), adjacent->words.end());
    function.guest_end = static_cast<GuestAddress>(
        static_cast<std::uint64_t>(function.guest_end) +
        static_cast<std::uint64_t>(adjacent->words.size()) * 4u);
    inferred_local_ranges.erase(adjacent);
  }

  function.ranges = {function.guest_start, function.guest_end};
  for (const auto& [range_start, range_end] : child_ranges) {
    function.ranges.push_back(range_start);
    function.ranges.push_back(range_end);
  }
  for (const auto& range : inferred_local_ranges) {
    function.ranges.push_back(range.start);
    function.ranges.push_back(static_cast<GuestAddress>(
        static_cast<std::uint64_t>(range.start) +
        static_cast<std::uint64_t>(range.words.size()) * 4u));
  }
}

void promote_tail_call_targets(CandidateScan& scan) {
  const auto& ctx = scan.ctx;
  auto& result = scan.result;
  const auto& decoder = scan.decoder;
  const auto& function = scan.function;
  const auto& pending_direct_branch_targets = scan.pending_direct_branch_targets;
  const auto& pending_chunk_branch_targets = scan.pending_chunk_branch_targets;
  const auto has_independent_discovery_evidence = [&scan](GuestAddress target) {
    return scan.has_independent_discovery_evidence(target);
  };
  const auto has_prologue_evidence = [&scan](GuestAddress target) {
    return scan.has_prologue_evidence(target);
  };

  // Tail-call promotion happens only after all definitely intra-procedural
  // conditional targets have been materialized.  A terminal direct branch
  // outside the final owned ranges remains a possible tail call, but it is
  // promoted only when the destination has independent evidence.  A
  // tentative range boundary created by this analysis is never itself such
  // evidence.
  const auto inside_own_extent = [&](GuestAddress target) {
    for (std::size_t i = 0; i + 1u < function.ranges.size(); i += 2u)
      if (target >= function.ranges[i] && target < function.ranges[i + 1u]) return true;
    return false;
  };
  const bool self_confirmed =
      std::any_of(function.sources.begin(), function.sources.end(),
                  [](DiscoverySource source) { return source != DiscoverySource::DirectBranch; });
  // A genuine compiler-emitted tail call commonly targets a shared/outlined
  // block that reuses the CALLER's stack frame and so never has a classic
  // prologue of its own (has_prologue_evidence() rejects it) - and nothing
  // else may ever `bl` it directly either (has_independent_discovery_
  // evidence() rejects it too). The terminal branch instruction itself is
  // still real, structural evidence from actually-compiled code (branches
  // are not placed at arbitrary addresses by accident); this confirms that
  // evidence by requiring the target to decode into a fully valid,
  // terminator-reaching instruction stream - the same structural check
  // load_and_analyze()'s gap-recovery pass already trusts elsewhere, just
  // applied to one specific, already-evidenced control-flow edge instead of
  // a blind per-byte scan of every unclaimed executable range, so it is
  // strictly narrower (and no more permissive) than a check this codebase
  // already relies on. Real AC6 repro: 0x8224c9a0 tail-calls 0x8224c458 (a
  // `cmplwi`-first shared block, not a prologue, with zero other callers
  // anywhere in the title) - entry-integrity validation failed outright
  // ("... leaves owner 0x8224c9a0 without a dispatchable guest entry")
  // because this address was never analyzed at all.
  const auto decodes_to_valid_terminated_block = [&](GuestAddress target) {
    const auto* target_section = executable_section(ctx.range_index, target);
    if (!target_section || target < target_section->virtual_address) return false;
    auto probe_offset = static_cast<std::size_t>(target - target_section->virtual_address);
    constexpr std::size_t kMaxProbeWords = 1024u;
    for (std::size_t probe = 0; probe < kMaxProbeWords; ++probe, probe_offset += 4u) {
      if (probe_offset + 4u > target_section->bytes.size()) return false;
      const auto probe_address = static_cast<GuestAddress>(target + probe * 4u);
      const auto probe_word = be32(target_section->bytes, probe_offset);
      if (probe_word == 0u) return false;
      const auto probe_instruction = decoder.decode(probe_address, probe_word);
      if (!probe_instruction.valid()) return false;
      if (is_terminal(probe_instruction)) return true;
    }
    return false;
  };
  const auto promote_terminal_target = [&](GuestAddress target) {
    if (inside_own_extent(target) || !self_confirmed) return;
    if (!has_independent_discovery_evidence(target) && !has_prologue_evidence(target) &&
        !decodes_to_valid_terminated_block(target))
      return;
    result.discovered.push_back({target, DiscoverySource::DirectBranch});
  };
  for (const auto& [target, terminal] : pending_direct_branch_targets)
    if (terminal) promote_terminal_target(target);
  for (const auto& [target, terminal] : pending_chunk_branch_targets)
    if (terminal) promote_terminal_target(target);
}

}  // namespace xenon::recomp::detail
