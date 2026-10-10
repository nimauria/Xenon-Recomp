#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <limits>
#include <string_view>

#include "xenon/recomp/driver.hpp"

namespace xenon::recomp {

bool load_adaptive_observations(const std::filesystem::path& path,
                                std::vector<AdaptiveObservation>& observations,
                                std::string& error) {
  std::ifstream input(path);
  if (!input) {
    error = "unable to open adaptive observation trace '" + path.string() + "'";
    return false;
  }

  const auto skip_space = [](std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  };
  const auto find_value = [&](std::string_view text, std::string_view key,
                              std::size_t& pos) -> bool {
    const std::string needle = "\"" + std::string(key) + "\"";
    pos = text.find(needle);
    if (pos == std::string_view::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return false;
    ++pos;
    skip_space(text, pos);
    return pos < text.size();
  };
  const auto parse_uint = [&](std::string_view text, std::string_view key,
                              bool required, std::uint32_t& value) -> bool {
    std::size_t pos = 0;
    if (!find_value(text, key, pos)) return !required;
    std::size_t end = pos;
    if (end + 2u <= text.size() && text.substr(end, 2u) == "0x") {
      end += 2u;
      while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) ++end;
    } else {
      while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
    }
    if (end == pos || (text.substr(pos, 2u) == "0x" && end == pos + 2u)) return false;
    try {
      const auto token = std::string(text.substr(pos, end - pos));
      const auto parsed = std::stoull(token, nullptr, 0);
      if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
      value = static_cast<std::uint32_t>(parsed);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  const auto parse_string = [&](std::string_view text, std::string_view key,
                                std::string& value) -> bool {
    std::size_t pos = 0;
    if (!find_value(text, key, pos) || text[pos] != '"') return false;
    const auto end = text.find('"', pos + 1u);
    if (end == std::string_view::npos) return false;
    value.assign(text.substr(pos + 1u, end - pos - 1u));
    return true;
  };

  constexpr std::size_t kMaxAdaptiveObservationLines = 1'000'000u;
  constexpr std::size_t kMaxAdaptiveObservationLineBytes = 64u * 1024u;
  std::vector<AdaptiveObservation> loaded;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line_number > kMaxAdaptiveObservationLines) {
      error = "adaptive observation trace exceeds safety limit of " +
              std::to_string(kMaxAdaptiveObservationLines) + " lines";
      return false;
    }
    if (line.size() > kMaxAdaptiveObservationLineBytes) {
      error = "adaptive observation line exceeds 64 KiB at " + path.string() + ":" +
              std::to_string(line_number);
      return false;
    }
    if (line_number == 1u && line.size() >= 3u &&
        static_cast<unsigned char>(line[0]) == 0xEFu &&
        static_cast<unsigned char>(line[1]) == 0xBBu &&
        static_cast<unsigned char>(line[2]) == 0xBFu)
      line.erase(0u, 3u);
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '#') continue;

    AdaptiveObservation observation{};
    std::uint32_t hits = 1u;
    std::uint32_t owner_hint = 0u;
    std::string kind;
    if (!parse_uint(line, "address", true, observation.address) ||
        !parse_uint(line, "site", false, observation.site) ||
        !parse_uint(line, "hits", false, hits) ||
        !parse_string(line, "kind", kind)) {
      error = "invalid adaptive observation at " + path.string() + ":" +
              std::to_string(line_number);
      return false;
    }
    if (parse_uint(line, "owner", false, owner_hint) &&
        line.find("\"owner\"") != std::string::npos)
      observation.owner_hint = owner_hint;
    if (line.find("\"imageHash\"") != std::string::npos) {
      if (!parse_string(line, "imageHash", observation.image_hash)) {
        error = "invalid imageHash in adaptive observation at " + path.string() + ":" +
                std::to_string(line_number);
        return false;
      }
      std::transform(observation.image_hash.begin(), observation.image_hash.end(),
                     observation.image_hash.begin(), [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                     });
      if (observation.image_hash.size() != 40u ||
          !std::all_of(observation.image_hash.begin(), observation.image_hash.end(),
                       [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
        error = "imageHash must be a 40-character SHA-1 hex string at " +
                path.string() + ":" + std::to_string(line_number);
        return false;
      }
    }

    if (kind == "executed-entry")
      observation.kind = AdaptiveObservationKind::ExecutedEntry;
    else if (kind == "indirect-call-target" || kind == "call")
      observation.kind = AdaptiveObservationKind::IndirectCallTarget;
    else if (kind == "indirect-branch-target" || kind == "branch")
      observation.kind = AdaptiveObservationKind::IndirectBranchTarget;
    else {
      error = "unknown adaptive observation kind '" + kind + "' at " +
              path.string() + ":" + std::to_string(line_number);
      return false;
    }
    observation.hits = std::max<std::uint32_t>(1u, hits);
    loaded.push_back(observation);
  }

  // Aggregate repeat misses from long runtime traces. Runtime writes are
  // append-only for crash durability, so repeated dynamic targets are normal.
  observations.insert(observations.end(), loaded.begin(), loaded.end());
  std::sort(observations.begin(), observations.end(), [](const auto& a, const auto& b) {
    if (a.address != b.address) return a.address < b.address;
    if (a.site != b.site) return a.site < b.site;
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.owner_hint != b.owner_hint) return a.owner_hint < b.owner_hint;
    return a.image_hash < b.image_hash;
  });
  std::vector<AdaptiveObservation> aggregated;
  aggregated.reserve(observations.size());
  for (const auto& item : observations) {
    if (!aggregated.empty()) {
      auto& previous = aggregated.back();
      if (previous.address == item.address && previous.site == item.site &&
          previous.kind == item.kind && previous.owner_hint == item.owner_hint &&
          previous.image_hash == item.image_hash) {
        const auto total = static_cast<std::uint64_t>(previous.hits) + item.hits;
        previous.hits = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            total, std::numeric_limits<std::uint32_t>::max()));
        continue;
      }
    }
    aggregated.push_back(item);
  }
  observations = std::move(aggregated);
  return true;
}

std::uint64_t adaptive_observation_fingerprint(
    std::span<const AdaptiveObservation> observations) noexcept {
  if (observations.empty()) return 0u;
  struct Fact {
    std::uint32_t address{};
    std::uint32_t site{};
    AdaptiveObservationKind kind{AdaptiveObservationKind::ExecutedEntry};
    std::optional<std::uint32_t> owner_hint;
    std::string image_hash;
  };
  std::vector<Fact> facts;
  facts.reserve(observations.size());
  for (const auto& observation : observations)
    facts.push_back({observation.address, observation.site, observation.kind,
                     observation.owner_hint, observation.image_hash});
  std::sort(facts.begin(), facts.end(), [](const Fact& a, const Fact& b) {
    if (a.address != b.address) return a.address < b.address;
    if (a.site != b.site) return a.site < b.site;
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.owner_hint != b.owner_hint) return a.owner_hint < b.owner_hint;
    return a.image_hash < b.image_hash;
  });
  facts.erase(std::unique(facts.begin(), facts.end(), [](const Fact& a, const Fact& b) {
                return a.address == b.address && a.site == b.site && a.kind == b.kind &&
                       a.owner_hint == b.owner_hint && a.image_hash == b.image_hash;
              }),
              facts.end());

  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](auto value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
  };
  const std::uint64_t count = facts.size();
  mix(count);
  for (const auto& fact : facts) {
    mix(fact.address);
    mix(fact.site);
    const auto kind = static_cast<std::uint8_t>(fact.kind);
    mix(kind);
    const std::uint8_t has_owner = fact.owner_hint.has_value() ? 1u : 0u;
    mix(has_owner);
    if (fact.owner_hint) mix(*fact.owner_hint);
    for (const auto ch : fact.image_hash) mix(static_cast<std::uint8_t>(ch));
    mix(static_cast<std::uint8_t>(0u));
  }
  return hash;
}

}  // namespace xenon::recomp
