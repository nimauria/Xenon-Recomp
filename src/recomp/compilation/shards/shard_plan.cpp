#include "xenon/recomp/shard_plan.hpp"

#include <algorithm>

namespace xenon::recomp {

bool shard_anchor(std::uint32_t address) noexcept {
  // splitmix64's finaliser: every address bit reaches the selected bits.
  std::uint64_t x = address;
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x % 12u == 0u;
}

std::vector<std::size_t> plan_shard_starts(std::span<const std::uint32_t> addresses,
                                           std::span<const std::size_t> sizes,
                                           std::size_t budget_bytes, std::size_t header_bytes) {
  std::vector<std::size_t> starts;
  const auto count = (std::min)(addresses.size(), sizes.size());
  const auto budget = (std::max)(budget_bytes, std::size_t{1});
  const auto anchored_from = budget / 2u;
  std::size_t current = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const bool open = !starts.empty();
    const bool hard = open && current + sizes[index] > budget;
    const bool soft = open && current >= anchored_from && shard_anchor(addresses[index]);
    if (!open || hard || soft) {
      starts.push_back(index);
      current = header_bytes;
    }
    current += sizes[index];
  }
  return starts;
}

}  // namespace xenon::recomp
