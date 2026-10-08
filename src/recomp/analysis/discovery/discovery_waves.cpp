#include <sstream>

#include "recomp/analysis/analysis_phases.hpp"

namespace xenon::recomp::detail {

void DiscoveryWaveEngine::process_frontier() {
  const auto& ctx = ctx_;
  const auto& pool = pool_;
  auto& report = report_;
  auto& discovered_evidence = discovered_evidence_;
  const auto& progress = progress_;
  while (!frontier.empty()) {
    // Phase D (repeated per wave): canonicalize (chunk-owner redirect, Part
    // 14) and validate every newly discovered candidate, then dedupe against
    // everything ever claimed in a previous wave - deterministically
    // address-sorted (std::set) so a wave's processing order, and therefore
    // every unresolved/warning entry it produces, never depends on
    // completion order between workers or between wave/non-wave (jobs=1) runs.
    std::set<GuestAddress> wave_set;
    for (const auto raw : frontier) {
      bool cyclic = false;
      const auto canonical = canonicalize_candidate(raw, ctx.hint_set_v2, cyclic);
      if (cyclic) {
        report.warnings.push_back(
            "chunk parent-redirect did not converge for candidate 0x" +
            [&] { std::ostringstream s; s << std::hex << raw; return s.str(); }() +
            " (possible cyclic FunctionChunk hint data)");
      }
      if (claimed.contains(canonical)) continue;
      if (!ExecutableRangeIndex::is_aligned_ppc_address(canonical)) {
        if (claimed.insert(canonical).second) {
          report.unresolved.push_back({canonical, canonical, "candidate-unaligned",
                                       "candidate address is not 4-byte aligned; cannot be a PPC "
                                       "instruction"});
          ++rejected_unaligned;
        }
        continue;
      }
      wave_set.insert(canonical);
    }
    frontier.clear();
    if (wave_set.empty()) break;
    for (const auto address : wave_set) claimed.insert(address);

    const std::vector<GuestAddress> wave(wave_set.begin(), wave_set.end());
    std::vector<FunctionAnalysisResult> results(wave.size());
    const auto run_one = [&](std::size_t i) { results[i] = analyze_function_candidate(wave[i], ctx); };
    pool.parallel_for(wave.size(), run_one);

    // Phase G (Part 2): deterministic merge - strictly in `wave`'s
    // address-sorted order, identical for jobs=1 and jobs=N.
    for (auto& result : results) {
      ++functions_analyzed;
      instruction_pattern_match_count += result.instruction_pattern_matches;
      resolved_indirect_via_dataflow += result.resolved_indirect_via_dataflow;
      resolved_indirect_via_backward_slice += result.resolved_indirect_via_backward_slice;
      resolved_indirect_via_readonly_table += result.resolved_indirect_via_readonly_table;
      resolved_indirect_via_jump_table += result.resolved_indirect_via_jump_table;
      for (auto& item : result.unresolved) report.unresolved.push_back(std::move(item));
      for (auto& warning : result.warnings) report.warnings.push_back(std::move(warning));
      if (result.outcome == FunctionAnalysisResult::Outcome::Accepted)
        report.functions.push_back(std::move(result.function));
      for (const auto& [discovered_address, discovered_source] : result.discovered) {
        frontier.push_back(discovered_address);
        auto& evidence = discovered_evidence[discovered_address];
        if (std::find(evidence.begin(), evidence.end(), discovered_source) == evidence.end())
          evidence.push_back(discovered_source);
      }
    }

    ++wave_count;
    if (progress) {
      std::ostringstream message;
      message << "[Analysis] wave " << wave_count << ": " << functions_analyzed << " analyzed, "
              << report.functions.size() << " accepted, " << frontier.size() << " newly discovered";
      progress(message.str());
    }
  }
}

}  // namespace xenon::recomp::detail
