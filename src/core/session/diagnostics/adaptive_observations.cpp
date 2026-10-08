#include <filesystem>
#include <fstream>
#include <mutex>
#include <tuple>

#include "xenon/core/session.hpp"

namespace xenon::core {

void XenonSession::record_compiled_lookup_miss(
    void* observer, cpu::ExecutionContext& context, cpu::GuestAddress target,
    cpu::CompiledLookupKind kind) {
  auto* session = static_cast<XenonSession*>(observer);
  if (!session) return;
  std::lock_guard<std::mutex> observation_lock(session->adaptive_observation_mutex_);
  // Runtime learning is evidence, not an unbounded telemetry sink. A hostile
  // or badly-corrupted target stream must not grow process memory forever.
  // 65k distinct misses is already vastly more than a normal title should
  // need before the next preparation pass incorporates the new entries.
  constexpr std::size_t kMaxAdaptiveObservationFactsPerSession = 65'536u;
  if (session->adaptive_observation_seen_.size() >= kMaxAdaptiveObservationFactsPerSession) return;
  const auto fact = std::make_tuple(target, context.state.cia, kind);
  if (!session->adaptive_observation_seen_.insert(fact).second) return;

  const auto write_observation = [&](const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    // JSONL is intentionally append-only: if execution terminates abruptly we
    // still retain every observation written before the failure. The ingest
    // side deduplicates identical records and accumulates hit counts.
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out) return;
    out << "{\"address\":" << target
        << ",\"site\":" << context.state.cia
        << ",\"kind\":\""
        << (kind == cpu::CompiledLookupKind::Call ? "indirect-call-target"
                                                  : "indirect-branch-target")
        << "\",\"hits\":1";
    if (session->effective_identity_)
      out << ",\"imageHash\":\""
          << xbox::format_effective_image_hash(session->effective_identity_->effective_image_hash)
          << "\"";
    out << "}\n";
  };
  write_observation(session->config_.adaptive_observation_path);
  if (session->config_.adaptive_observation_mirror_path !=
      session->config_.adaptive_observation_path)
    write_observation(session->config_.adaptive_observation_mirror_path);
}


void XenonSession::record_dynamic_fallback_observation(
    const cpu::DynamicFallbackObservation& observation) {
  std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
  constexpr std::size_t kMaxFallbackFactsPerSession = 65'536u;
  if (dynamic_fallback_observation_seen_.size() >= kMaxFallbackFactsPerSession)
    return;
  if (!dynamic_fallback_observation_seen_
           .emplace(observation.entry, observation.block_fingerprint)
           .second)
    return;

  const auto write_observation = [&](const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out) return;
    // `executed-entry` is already consumed by Gen 5/6 analysis. The extra Gen
    // 7 fields are intentionally additive: old readers ignore them, while a
    // later knowledge-base generation can consume fingerprints/reasons.
    out << "{\"address\":" << observation.entry
        << ",\"site\":" << observation.site
        << ",\"kind\":\"executed-entry\",\"hits\":1"
        << ",\"fallback\":true"
        << ",\"exit\":" << observation.exit
        << ",\"instructions\":" << observation.instructions
        << ",\"fingerprint\":" << observation.block_fingerprint
        << ",\"fallbackReason\":\""
        << cpu::dynamic_fallback_stop_reason_name(observation.reason) << "\""
        << ",\"transferKind\":\""
        << (observation.kind == cpu::CompiledLookupKind::Call ? "call" : "branch")
        << "\"";
    if (effective_identity_)
      out << ",\"imageHash\":\""
          << xbox::format_effective_image_hash(
                 effective_identity_->effective_image_hash)
          << "\"";
    out << "}\n";
  };
  write_observation(config_.adaptive_observation_path);
  if (config_.adaptive_observation_mirror_path !=
      config_.adaptive_observation_path)
    write_observation(config_.adaptive_observation_mirror_path);
}

}  // namespace xenon::core
