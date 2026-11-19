#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace xenon::gpu {

// When bytes a resolve wrote reach Xbox physical RAM on a backend that keeps
// a device-side mirror of it (Direct3D 12, Vulkan). The mirror always holds
// them, so textures sampled from a resolve never depend on this.
enum class ResolveReadback : std::uint8_t {
  // Every resolve is copied back, and the command processor waits for those
  // copies before the guest can observe the GPU passing it (a fence, an
  // interrupt, a memory poll). Exact Xbox behaviour; the default.
  Always,
  // Only when Xenon itself reads the range (index buffers, captures,
  // scan-out, explicit make_guest_memory_cpu_visible()). For a title known
  // never to read resolve output with its CPU - declared per revision by its
  // module (rendering.json "memory.resolveReadback"). Ace Combat 6 copies
  // ~1 GB a second back otherwise, and its reference port runs with resolve
  // readback disabled.
  OnRequest,
};

[[nodiscard]] constexpr std::optional<ResolveReadback> parse_resolve_readback(
    std::string_view name) noexcept {
  if (name == "always") return ResolveReadback::Always;
  if (name == "onRequest") return ResolveReadback::OnRequest;
  return std::nullopt;
}

[[nodiscard]] constexpr const char* resolve_readback_name(ResolveReadback policy) noexcept {
  return policy == ResolveReadback::OnRequest ? "onRequest" : "always";
}

}  // namespace xenon::gpu
