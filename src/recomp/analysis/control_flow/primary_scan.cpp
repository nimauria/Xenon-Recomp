#include <algorithm>
#include <sstream>

#include "recomp/analysis/control_flow/candidate_scan.hpp"
#include "recomp/driver/driver_support.hpp"
#include "xenon/recomp/ppc_value_analysis.hpp"

namespace xenon::recomp::detail {

// Linear decode of the candidate's primary range: records calls, branches and
// indirect-flow resolutions, and stops at a terminal branch, a hinted
// non-code region, an instruction-pattern match, an undecodable word, or the
// metadata/hint extent established by establish_scan_bounds().
void scan_primary_range(CandidateScan& scan) {
  const auto start = scan.start;
  const auto& ctx = scan.ctx;
  auto& result = scan.result;
  const auto& decoder = scan.decoder;
  auto& function = scan.function;
  auto& words = scan.words;
  const auto* section = scan.section;
  const auto offset = scan.offset;
  const auto max_words = scan.max_words;
  const auto& metadata_end = scan.metadata_end;
  auto& local_boundaries = scan.local_boundaries;
  auto& pending_direct_branch_targets = scan.pending_direct_branch_targets;
  auto& direct_call_targets = scan.direct_call_targets;
  // Gen 6: cheap forward abstract PPC value state. Unlike the previous
  // all-or-nothing optional<uint32_t> tracker, this preserves unrelated GPR
  // facts across harmless scheduling and can fold static non-writable
  // function-pointer/vtable loads. A bounded backward slicer is invoked only
  // at an indirect site when this fast state cannot resolve it.
  value_analysis::PpcValueTracker value_tracker(ctx.image);
  std::vector<cpu::DecodedInstruction> value_history;
  value_history.reserve(std::min<std::size_t>(max_words, 256u));
  // Part 8 (Recomp Analysis V3): a coarse "was there a bounds check
  // recently" signal for the jump-table-recovery heuristic below - real
  // compiler-generated switch dispatch almost always compares the index
  // against a bound shortly before the indirect branch; an indirect branch
  // with no compare anywhere nearby is weaker evidence that recovered
  // "targets" are a real jump table rather than coincidentally
  // pointer-shaped data.
  std::size_t instructions_since_compare = 1000;
  for (std::size_t index = 0; index < max_words; ++index) {
    const auto address = static_cast<GuestAddress>(start + index * 4u);
    if (index != 0 && hinted_non_code(ctx, address)) {
      result.warnings.push_back("function scan stopped at hinted non-code region 0x" +
                                [&] { std::ostringstream s; s << std::hex << address; return s.str(); }());
      function.guest_end = address;
      break;
    }
    const auto word = be32(section->bytes, offset + index * 4);
    if (index != 0) {
      if (const auto* pattern = instruction_pattern_match(ctx, address, word)) {
        ++result.instruction_pattern_matches;
        result.warnings.push_back(
            "function scan stopped at instruction-pattern match 0x" +
            [&] { std::ostringstream s; s << std::hex << address; return s.str(); }() +
            (pattern->reason.empty() ? "" : " (" + pattern->reason + ")"));
        function.guest_end = address;
        break;
      }
    }
    const auto instruction = decoder.decode(address, word);
    if (!instruction.valid()) {
      const std::string classification = classify_decode_failure(word);
      function.error = (classification == "invalid-ppc" ? "invalid PPC at 0x" : "unsupported PPC encoding at 0x") +
                       [&] { std::ostringstream s; s << std::hex << address; return s.str(); }();
      result.unresolved.push_back({address, word, classification, function.error});
      break;
    }
    words.push_back(instruction.word);
    // Part 4: corroborating evidence only - a recognized prologue opening at
    // this candidate's very first instruction adds PrologueHeuristic to its
    // sources, never creates a new candidate by itself.
    if (index == 0 && looks_like_function_prologue(instruction)) add_source(function, DiscoverySource::PrologueHeuristic);
    if (instructions_since_compare < 1000) ++instructions_since_compare;
    if (instruction.mnemonic().rfind("cmp", 0) == 0) instructions_since_compare = 0;
    const bool terminal_branch = is_terminal(instruction);
    if (instruction.mnemonic() == "bclrx" && !instruction.lk() && terminal_branch)
      function.has_explicit_return = true;
    if (instruction.info->group == cpu::InstructionGroup::Branch) {
      if (instruction.lk() && (instruction.info->format == cpu::InstructionFormat::I ||
                               instruction.info->format == cpu::InstructionFormat::B)) {
        const auto target = instruction.direct_branch_target();
        function.calls.push_back(target);
        function.branches.push_back({address, target, false, false, true, false, true});
        direct_call_targets.insert(target);
        result.discovered.push_back({target, DiscoverySource::DirectCall});
      } else if (!instruction.lk() && (instruction.info->format == cpu::InstructionFormat::I ||
                                        instruction.info->format == cpu::InstructionFormat::B)) {
        const auto target = instruction.direct_branch_target();
        function.branch_references.push_back(target);
        function.branches.push_back({address, target, terminal_branch, false, false,
                                         is_conditional_control_transfer(instruction), !terminal_branch});
        local_boundaries.insert(target);
        pending_direct_branch_targets.push_back({target, terminal_branch});
      } else if (instruction.info->format == cpu::InstructionFormat::XL &&
                !(instruction.mnemonic() == "bclrx" && !instruction.lk())) {
        // Schema V2 known-indirect-site resolution (Part 1.7/1.11): a site
        // the module's own analysis metadata already investigated is
        // reported and treated distinctly from one nothing ever looked at,
        // and any known/decoded targets are seeded exactly like a direct
        // branch/call.
        //
        // An unconditional, non-linked bclrx ("blr") is excluded: it is an
        // ordinary function return, not an indirect branch needing
        // resolution.
        bool resolved_by_hint = false;
        if (ctx.hint_set_v2) {
          if (instruction.lk()) {
            const auto call_it =
                std::find_if(ctx.hint_set_v2->indirect_calls.begin(), ctx.hint_set_v2->indirect_calls.end(),
                             [address](const auto& c) { return c.callsite == address; });
            if (call_it != ctx.hint_set_v2->indirect_calls.end()) {
              resolved_by_hint = true;
              for (const auto target : call_it->targets) {
                function.calls.push_back(target);
                function.branches.push_back({address, target, false, true, true,
                                             is_conditional_control_transfer(instruction), true});
                result.discovered.push_back({target, DiscoverySource::ResolvedIndirectCall});
              }
              if (call_it->targets.empty()) {
                result.unresolved.push_back(
                    {address, 0, "indirect-call-acknowledged",
                     "site flagged as indirect by module analysis metadata; no static targets known"});
              }
            }
          } else {
            const auto branch_it =
                std::find_if(ctx.hint_set_v2->indirect_branches.begin(), ctx.hint_set_v2->indirect_branches.end(),
                             [address](const auto& b) { return b.site == address; });
            const auto table_it =
                std::find_if(ctx.hint_set_v2->switches.begin(), ctx.hint_set_v2->switches.end(),
                             [address](const auto& t) { return t.site == address; });
            std::vector<GuestAddress> known_targets;
            if (branch_it != ctx.hint_set_v2->indirect_branches.end()) known_targets = branch_it->targets;
            else if (table_it != ctx.hint_set_v2->switches.end())
              known_targets = resolve_switch_targets(*table_it, ctx.range_index, result.warnings);
            if (branch_it != ctx.hint_set_v2->indirect_branches.end() ||
                table_it != ctx.hint_set_v2->switches.end()) {
              resolved_by_hint = true;
              for (const auto target : known_targets) {
                function.branch_references.push_back(target);
                function.branches.push_back({address, target, is_terminal(instruction), true, false,
                                         is_conditional_control_transfer(instruction),
                                         !is_terminal(instruction)});
                local_boundaries.insert(target);
                result.discovered.push_back({target, DiscoverySource::ControlFlowHint});
              }
              if (known_targets.empty()) {
                result.unresolved.push_back(
                    {address, 0, "indirect-branch-acknowledged",
                     "site flagged as indirect by module analysis metadata; no resolvable targets"});
              }
            }
          }
        }
        if (!resolved_by_hint) {
          // Gen 6 tiered indirect-flow resolution. The cheap forward value
          // state is always tried first. Only unresolved sites pay for the
          // bounded backward slicer, which can walk through unrelated
          // scheduling and fold non-writable function-pointer/vtable loads.
          const auto indirect_resolution = value_analysis::resolve_indirect_flow(
              instruction, value_tracker, value_history, ctx.image);
          const bool resolved_by_dataflow = !indirect_resolution.targets.empty();
          if (resolved_by_dataflow) {
            ++result.resolved_indirect_via_dataflow;
            if (indirect_resolution.kind == value_analysis::IndirectResolverKind::BackwardSlice)
              ++result.resolved_indirect_via_backward_slice;
            if (indirect_resolution.kind == value_analysis::IndirectResolverKind::ReadOnlyTable)
              ++result.resolved_indirect_via_readonly_table;
            for (const auto target : indirect_resolution.targets) {
              if (instruction.lk()) {
                function.calls.push_back(target);
                function.branches.push_back({address, target, false, true, true,
                                             is_conditional_control_transfer(instruction), true});
              } else {
                function.branch_references.push_back(target);
                function.branches.push_back({address, target, is_terminal(instruction), true, false,
                                             is_conditional_control_transfer(instruction),
                                             !is_terminal(instruction)});
                local_boundaries.insert(target);
              }
              result.discovered.push_back(
                  {target, instruction.lk() ? DiscoverySource::ResolvedIndirectCall
                                            : DiscoverySource::ResolvedIndirectBranch});
            }
            if (indirect_resolution.kind == value_analysis::IndirectResolverKind::BackwardSlice ||
                indirect_resolution.kind == value_analysis::IndirectResolverKind::ReadOnlyTable) {
              std::string detail = "resolved indirect " +
                  std::string(value_analysis::indirect_resolver_name(indirect_resolution.kind)) +
                  " at 0x" + hex_string(address);
              if (indirect_resolution.table_address)
                detail += " via static table 0x" + hex_string(*indirect_resolution.table_address);
              result.warnings.push_back(std::move(detail));
            }
          }
          if (!resolved_by_dataflow) {
            result.unresolved.push_back({address, 0, instruction.lk() ? "indirect-call" : "indirect-branch",
                                         "target depends on runtime register state"});
            // Part 8 (Recomp Analysis V3): only attempt jump-table recovery
            // when a bounds check appeared recently - real compiler switch
            // dispatch compares the index against a bound shortly before
            // the indirect branch; without that corroborating evidence
            // nearby, recovered "targets" are indistinguishable from
            // coincidentally pointer-shaped data, so none are reported
            // (stricter than treating every unresolved indirect branch as a
            // possible table - Part 8's "reject arbitrary data that merely
            // resembles pointers").
            if (!instruction.lk() && instructions_since_compare <= 8u) {
              const auto table_start = offset + index * 4u + 4u;
              const auto table_end = std::min(section->bytes.size(), table_start + 256u);
              for (auto table = table_start; table + 4u <= table_end; table += 4u) {
                const auto target = be32(section->bytes, table);
                if (executable_section(ctx.range_index, target)) {
                  function.branch_references.push_back(target);
                  function.branches.push_back({address, target, is_terminal(instruction), true, false,
                                         is_conditional_control_transfer(instruction),
                                         !is_terminal(instruction)});
                  result.discovered.push_back({target, DiscoverySource::ResolvedIndirectBranch});
                  ++result.resolved_indirect_via_jump_table;
                  result.warnings.push_back(
                      "recovered possible jump-table target 0x" +
                      [&] { std::ostringstream s; s << std::hex << target; return s.str(); }());
                }
              }
            }
          }
        }
      }
      if (terminal_branch) {
        function.guest_end = address + 4;
        break;
      }
    }
    // Gen 6 state for the NEXT instruction. Keep a bounded prior-instruction
    // history for the expensive resolver; the history itself is cheap and the
    // recursive slice is never run unless the forward state failed.
    value_history.push_back(instruction);
    if (value_history.size() > 96u) value_history.erase(value_history.begin(), value_history.begin() + 32u);
    value_tracker.step(instruction);
    function.guest_end = address + 4;
    if (metadata_end && address + 4u >= *metadata_end) {
      function.guest_end = address + 4u;
      break;
    }
  }
}

}  // namespace xenon::recomp::detail
