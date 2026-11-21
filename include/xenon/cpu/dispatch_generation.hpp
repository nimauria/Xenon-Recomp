#pragma once

#include <atomic>
#include <cstdint>

namespace xenon::cpu {

// The dispatch generation: a counter bumped whenever what an
// ExecutionContext's compiled_lookup would return for some target can change
// (the executable code cache invalidating or replacing code, call tracing
// being reconfigured, a native extension being loaded or unloaded). Generated
// call sites cache the native function an indirect target resolved to and
// trust it only while the generation they saw has not moved (see
// aot::lookup_cached). The counter belongs to the host process, not to a
// generated module: contexts the host creates point at it through
// ExecutionContext::dispatch_generation, and a context without one is never
// cached for.
[[nodiscard]] const std::atomic<std::uint64_t>& dispatch_generation() noexcept;

// Invalidates every call-site cache. Returns the new generation.
std::uint64_t bump_dispatch_generation() noexcept;

}  // namespace xenon::cpu
