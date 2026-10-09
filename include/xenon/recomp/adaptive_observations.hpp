#pragma once

// Runtime feedback records consumed by analysis and their trace loader.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xenon::recomp {

enum class AdaptiveObservationKind : std::uint8_t {
  ExecutedEntry,
  IndirectCallTarget,
  IndirectBranchTarget,
};

// Generic feedback record suitable for future runtime/coverage/network
// learning. It contains facts only: an executable guest address was observed
// as an entry/indirect target. Analysis decides whether that address is a new
// semantic function or an alternate entry into an existing compiled region.
struct AdaptiveObservation {
  std::uint32_t address{};
  std::uint32_t site{};
  AdaptiveObservationKind kind{AdaptiveObservationKind::ExecutedEntry};
  std::uint32_t hits{1};
  std::optional<std::uint32_t> owner_hint;
  // Optional lowercase effective-image SHA-1. Runtime-generated records set
  // this so observations from an old title update/revision cannot poison a
  // new executable that happens to reuse the same guest address. Empty keeps
  // hand-authored/developer traces backwards-compatible.
  std::string image_hash;
};

// Loads Xenon's newline-delimited runtime observation trace (the
// adaptive-observations.jsonl emitted by runtime_host). Records are
// deduplicated by address/site/kind/owner-hint and hit counts are accumulated.
// The parser also accepts hexadecimal integer literals to make hand-authored
// developer traces convenient.
[[nodiscard]] bool load_adaptive_observations(
    const std::filesystem::path& path,
    std::vector<AdaptiveObservation>& observations,
    std::string& error);
// Stable fingerprint of the DISTINCT adaptive facts consumed by analysis.
// Hit counts are deliberately excluded: seeing the same target again must not
// invalidate a prepared artifact; learning a new address/site/kind/owner fact must.
[[nodiscard]] std::uint64_t adaptive_observation_fingerprint(
    std::span<const AdaptiveObservation> observations) noexcept;

}  // namespace xenon::recomp
