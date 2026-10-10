#pragma once

// Private helpers shared by the recompilation driver's analysis, code
// generation and reporting translation units. Nothing here is part of the
// public xenon/recomp API; see include/xenon/recomp/driver.hpp for that.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

namespace xenon::recomp::detail {

// Generated analysis/report contract version. Keep this separate from the
// module hint schema: changing Xenon's inferred ownership/entry semantics must
// invalidate stale generated artifacts without forcing every existing hint
// file to be rewritten.
inline constexpr std::uint32_t kAnalysisReportSchemaVersion = 4u;
inline constexpr std::uint32_t kAnalysisEngineRevision = 9u;

inline std::uint32_t be32(const std::vector<std::byte>& data, std::size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return (std::to_integer<std::uint32_t>(data[offset]) << 24) |
         (std::to_integer<std::uint32_t>(data[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(data[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(data[offset + 3]);
}

inline std::uint64_t hash_bytes(std::span<const std::byte> bytes, std::uint64_t seed = 1469598103934665603ull) {
  auto hash = seed;
  for (const auto byte : bytes) {
    hash ^= std::to_integer<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hash_name(std::uint64_t hash);
std::string hex_string(std::uint32_t value);
std::string json_escape(const std::string& value);
std::string cpp_name(std::uint32_t address);
std::string sanitize_cpp_name(std::string name, std::uint32_t address);

// Configuration identity of an analysis run (hashed into every function's
// source hash, so a changed hint/knowledge/adaptive input invalidates
// generated artifacts). Defined in caching/configuration_identity.cpp.
std::uint64_t hash_config(const DriverOptions& options);

// Legacy line-oriented module hint file (DriverOptions::hints_file) merged
// with DriverOptions::hints. Defined in hints/module_hints.cpp.
bool load_hints(const DriverOptions& options, std::vector<ModuleHint>& hints, std::string& error);

}  // namespace xenon::recomp::detail
