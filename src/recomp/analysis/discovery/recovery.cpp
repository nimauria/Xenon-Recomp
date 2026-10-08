#include <algorithm>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

// Dead-Rising-style dropped-edge recovery, generalized: once ordinary CFG
// closure reaches a fixed point, an executable target still lacking an owner
// is promoted only when at least two distinct already-discovered functions
// reference it. One branch alone is not enough to manufacture a function.
// Re-run the same deterministic wave engine to a bounded fixpoint; any weak
// overlapping candidate created here is eligible for absorption below.
std::size_t recover_multi_source_orphans(DiscoveryWaveEngine& engine,
                                         const ExecutableRangeIndex& range_index,
                                         const analysis::AnalysisHintSetV2* hint_set_v2) {
  auto& report = engine.report();
  auto& discovered_evidence = engine.discovered_evidence();
  const auto& claimed = engine.claimed;
  auto& frontier = engine.frontier;
  const auto blocked_by_non_code_hint = [hint_set_v2](GuestAddress address) {
    return in_non_code_hint_region(hint_set_v2, address);
  };
  const auto process_frontier = [&engine] { engine.process_frontier(); };
  std::size_t orphan_entries_recovered = 0;
  for (unsigned recovery_round = 0; recovery_round < 4u; ++recovery_round) {
    std::map<GuestAddress, std::set<GuestAddress>> incoming;
    for (const auto& function : report.functions) {
      for (const auto& branch : function.branches) {
        if (branch.linked) continue;
        if (!range_index.is_executable_address(branch.target) ||
            !ExecutableRangeIndex::is_aligned_ppc_address(branch.target))
          continue;
        incoming[branch.target].insert(function.guest_start);
      }
    }

    std::vector<GuestAddress> recovered;
    for (const auto& [target, owners] : incoming) {
      if (owners.size() < 2u || claimed.contains(target)) continue;
      const bool already_owned = std::any_of(
          report.functions.begin(), report.functions.end(),
          [target](const auto& function) { return owns_address(function, target); });
      if (already_owned || blocked_by_non_code_hint(target)) continue;
      auto& evidence = discovered_evidence[target];
      add_source(evidence, DiscoverySource::GapRecovery);
      recovered.push_back(target);
    }
    if (recovered.empty()) break;
    orphan_entries_recovered += recovered.size();
    frontier.insert(frontier.end(), recovered.begin(), recovered.end());
    process_frontier();
  }
  return orphan_entries_recovered;
}

// Conservative gap fill, generalized from the repair passes used by mature
// title-specific recomp projects. We never blanket-decode every byte in
// .text as a function. A gap candidate must:
//   * be outside every already-owned compiled range,
//   * start at a structural boundary (section start, padding/invalid word,
//     terminator, or the end of already-owned/non-code data),
//   * begin with a recognizable PPC function prologue, and
//   * reach a real terminator before colliding with already-owned code.
// The resulting candidate carries only GapRecovery+PrologueHeuristic (weak
// evidence), so stronger CFG ownership can still absorb it later.
std::size_t recover_unowned_gaps(DiscoveryWaveEngine& engine,
                                 const analysis::AnalysisHintSetV2* hint_set_v2) {
  auto& report = engine.report();
  auto& discovered_evidence = engine.discovered_evidence();
  const auto& claimed = engine.claimed;
  auto& frontier = engine.frontier;
  const auto blocked_by_non_code_hint = [hint_set_v2](GuestAddress address) {
    return in_non_code_hint_region(hint_set_v2, address);
  };
  const auto process_frontier = [&engine] { engine.process_frontier(); };
  std::size_t gap_functions_recovered = 0;
  std::vector<std::pair<GuestAddress, GuestAddress>> owned_ranges;
  for (const auto& function : report.functions) {
    if (!function.compiled || function.native_replacement) continue;
    if (!function.ranges.empty()) {
      for (std::size_t i = 0; i + 1u < function.ranges.size(); i += 2u) {
        if (function.ranges[i + 1u] > function.ranges[i])
          owned_ranges.emplace_back(function.ranges[i], function.ranges[i + 1u]);
      }
    } else if (function.guest_end > function.guest_start) {
      owned_ranges.emplace_back(function.guest_start, function.guest_end);
    }
  }
  std::sort(owned_ranges.begin(), owned_ranges.end());
  std::vector<std::pair<GuestAddress, GuestAddress>> merged_owned;
  for (const auto& range : owned_ranges) {
    if (merged_owned.empty() || range.first > merged_owned.back().second) {
      merged_owned.push_back(range);
    } else {
      merged_owned.back().second = std::max(merged_owned.back().second, range.second);
    }
  }

  // Ranges accepted by this scan are reserved immediately so another
  // prologue inside the same not-yet-analyzed gap cannot be double-seeded.
  std::vector<std::pair<GuestAddress, GuestAddress>> provisional_gap_ranges;
  const auto in_sorted_ranges = [](const auto& ranges, GuestAddress address) {
    const auto it = std::upper_bound(
        ranges.begin(), ranges.end(), address,
        [](GuestAddress value, const auto& range) { return value < range.first; });
    if (it == ranges.begin()) return false;
    const auto& candidate = *std::prev(it);
    return address >= candidate.first && address < candidate.second;
  };
  const auto is_owned_for_gap = [&](GuestAddress address) {
    return in_sorted_ranges(merged_owned, address) ||
           in_sorted_ranges(provisional_gap_ranges, address);
  };

  cpu::Decoder gap_decoder;
  std::vector<GuestAddress> gap_candidates;
  constexpr std::size_t kMaxGapProbeWords = 1024u;
  for (const auto& section : report.image.sections) {
    if (!section.executable || section.bytes.size() < 8u) continue;
    for (std::size_t offset = 0; offset + 4u <= section.bytes.size(); offset += 4u) {
      const auto address = static_cast<GuestAddress>(section.virtual_address + offset);
      if (!ExecutableRangeIndex::is_aligned_ppc_address(address) ||
          claimed.contains(address) || is_owned_for_gap(address) ||
          blocked_by_non_code_hint(address))
        continue;

      const auto word = be32(section.bytes, offset);
      if (word == 0u) continue;
      const auto instruction = gap_decoder.decode(address, word);
      if (!instruction.valid() || !looks_like_function_prologue(instruction)) continue;

      bool structural_boundary = offset == 0u;
      if (!structural_boundary) {
        const auto previous = static_cast<GuestAddress>(address - 4u);
        if (is_owned_for_gap(previous) || blocked_by_non_code_hint(previous)) {
          structural_boundary = true;
        } else {
          const auto previous_word = be32(section.bytes, offset - 4u);
          if (previous_word == 0u) {
            structural_boundary = true;
          } else {
            const auto previous_instruction = gap_decoder.decode(previous, previous_word);
            structural_boundary = !previous_instruction.valid() || is_terminal(previous_instruction);
          }
        }
      }
      if (!structural_boundary) continue;

      bool reached_terminator = false;
      bool collided_with_owner = false;
      GuestAddress provisional_end = address;
      const auto max_words = std::min<std::size_t>(
          kMaxGapProbeWords, (section.bytes.size() - offset) / 4u);
      for (std::size_t probe = 0; probe < max_words; ++probe) {
        const auto probe_address = static_cast<GuestAddress>(address + probe * 4u);
        if (probe != 0u &&
            (is_owned_for_gap(probe_address) || blocked_by_non_code_hint(probe_address))) {
          collided_with_owner = true;
          break;
        }
        const auto probe_word = be32(section.bytes, offset + probe * 4u);
        if (probe_word == 0u) break;
        const auto probe_instruction = gap_decoder.decode(probe_address, probe_word);
        if (!probe_instruction.valid()) break;
        provisional_end = probe_address + 4u;
        if (is_terminal(probe_instruction)) {
          reached_terminator = true;
          break;
        }
      }
      if (!reached_terminator || collided_with_owner || provisional_end <= address) continue;

      auto& evidence = discovered_evidence[address];
      add_source(evidence, DiscoverySource::GapRecovery);
      add_source(evidence, DiscoverySource::PrologueHeuristic);
      gap_candidates.push_back(address);
      provisional_gap_ranges.emplace_back(address, provisional_end);
      std::sort(provisional_gap_ranges.begin(), provisional_gap_ranges.end());
    }
  }

  if (!gap_candidates.empty()) {
    std::sort(gap_candidates.begin(), gap_candidates.end());
    gap_candidates.erase(std::unique(gap_candidates.begin(), gap_candidates.end()),
                         gap_candidates.end());
    gap_functions_recovered += gap_candidates.size();
    frontier.insert(frontier.end(), gap_candidates.begin(), gap_candidates.end());
    process_frontier();
  }
  return gap_functions_recovered;
}

}  // namespace xenon::recomp::detail
