#include <array>
#include <span>

#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp::detail {

std::uint64_t hash_config(const DriverOptions& options) {
  std::uint64_t hash = hash_bytes({});
  hash = hash_bytes(std::as_bytes(std::span(&kAnalysisEngineRevision, 1)), hash);
  for (const auto& hint : options.hints) {
    hash = hash_bytes(std::as_bytes(std::span(hint.name.data(), hint.name.size())), hash);
    for (const auto address : hint.function_boundaries)
      hash = hash_bytes(std::as_bytes(std::span(&address, 1)), hash);
    for (const auto address : hint.data_regions)
      hash = hash_bytes(std::as_bytes(std::span(&address, 1)), hash);
    for (const auto& symbol : hint.known_symbols) {
      hash = hash_bytes(std::as_bytes(std::span(&symbol.address, 1)), hash);
      hash = hash_bytes(std::as_bytes(std::span(symbol.name.data(), symbol.name.size())), hash);
    }
    for (const auto& hook : hint.special_hooks)
      hash = hash_bytes(std::as_bytes(std::span(hook.data(), hook.size())), hash);
    for (const auto& patch : hint.patches)
      hash = hash_bytes(std::as_bytes(std::span(patch.data(), patch.size())), hash);
  }
  // Adaptive observations are facts, not an ordered event stream. Hash the
  // canonical sorted/distinct fingerprint so the same learned control-flow
  // knowledge produces the same generated artifact regardless of JSONL order,
  // duplicate records, or accumulated hit counts.
  const auto adaptive_hash = adaptive_observation_fingerprint(options.adaptive_observations);
  hash = hash_bytes(std::as_bytes(std::span(&adaptive_hash, 1)), hash);
  const auto knowledge_hash = knowledge_base_fingerprint(options.knowledge_records);
  hash = hash_bytes(std::as_bytes(std::span(&knowledge_hash, 1)), hash);
  hash = hash_bytes(std::as_bytes(std::span(&options.knowledge_match_min_score, 1)), hash);
  const std::array<std::uint8_t, 3> adaptive_flags = {
      static_cast<std::uint8_t>(options.scan_static_pointer_tables),
      static_cast<std::uint8_t>(options.recover_multi_source_orphans),
      static_cast<std::uint8_t>(options.recover_unowned_gaps)};
  hash = hash_bytes(std::as_bytes(std::span(adaptive_flags)), hash);
  return hash;
}

}  // namespace xenon::recomp::detail
