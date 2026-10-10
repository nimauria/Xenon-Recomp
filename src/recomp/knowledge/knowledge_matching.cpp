#include <algorithm>

#include "recomp/analysis/analysis_phases.hpp"
#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

// Gen 9 universal knowledge matching. Compute the final normalized identity
// only after graph provenance/callers are reconciled, then score every
// independent dimension. A KnowledgeMatch source may corroborate confidence
// but is never considered semantic-boundary evidence. Knowledge-only anchor
// seeds which fail this stronger check are removed before overlap ownership
// can be perturbed by a false short-block collision.
KnowledgeMatchStatistics apply_knowledge_matching(AnalysisReport& report, const DriverOptions& effective,
                                                  const std::string& current_effective_image_hash) {
  KnowledgeMatchStatistics statistics;
  auto& knowledge_functions_fingerprinted = statistics.functions_fingerprinted;
  auto& knowledge_matches_considered = statistics.matches_considered;
  auto& knowledge_matches_accepted = statistics.matches_accepted;
  auto& knowledge_cross_revision_matches = statistics.cross_revision_matches;
  for (auto& function : report.functions) {
    if (!function.compiled || function.native_replacement) continue;
    function.fingerprint = fingerprint_function(report.image, function);
    if (!function.fingerprint.valid()) continue;
    ++knowledge_functions_fingerprinted;
    const auto matches = match_knowledge(function.fingerprint, effective.knowledge_records,
                                         current_effective_image_hash,
                                         effective.knowledge_match_min_score);
    knowledge_matches_considered += effective.knowledge_records.size();
    for (const auto& match : matches) {
      const auto& record = effective.knowledge_records[match.record_index];
      function.knowledge_matches.push_back(
          {function.guest_start, record.id, record.label, record.family, record.kind,
           match.score, match.cross_revision, true});
      ++knowledge_matches_accepted;
      if (match.cross_revision) ++knowledge_cross_revision_matches;
    }
    if (!matches.empty()) add_source(function, DiscoverySource::KnowledgeMatch);
  }
  report.functions.erase(
      std::remove_if(report.functions.begin(), report.functions.end(), [&](const auto& function) {
        if (!function.compiled) return false;
        const bool knowledge_only = !function.sources.empty() &&
            std::all_of(function.sources.begin(), function.sources.end(), [](DiscoverySource source) {
              return source == DiscoverySource::KnowledgeMatch ||
                     source == DiscoverySource::PrologueHeuristic;
            }) &&
            std::find(function.sources.begin(), function.sources.end(),
                      DiscoverySource::KnowledgeMatch) != function.sources.end();
        if (!knowledge_only || !function.knowledge_matches.empty()) return false;
        report.warnings.push_back(
            "knowledge anchor candidate at 0x" + hex_string(function.guest_start) +
            " rejected because the completed function fingerprint did not match the knowledge record");
        return true;
      }),
      report.functions.end());
  for (auto& function : report.functions) {
    function.authority = authority_for_sources(function.sources);
    function.confidence = confidence_for_sources(function.sources);
  }
  return statistics;
}

}  // namespace xenon::recomp::detail
