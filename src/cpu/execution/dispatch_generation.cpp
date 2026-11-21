#include "xenon/cpu/dispatch_generation.hpp"

namespace xenon::cpu {

namespace {
std::atomic<std::uint64_t> g_dispatch_generation{1u};
}  // namespace

const std::atomic<std::uint64_t>& dispatch_generation() noexcept { return g_dispatch_generation; }

std::uint64_t bump_dispatch_generation() noexcept {
  return g_dispatch_generation.fetch_add(1u, std::memory_order_acq_rel) + 1u;
}

}  // namespace xenon::cpu
