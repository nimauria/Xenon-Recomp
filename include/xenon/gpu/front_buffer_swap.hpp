#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "xenon/gpu/types.hpp"

namespace xenon::gpu {

// A front-buffer flip as VdSwap places it in the command stream. On hardware
// the swap the kernel writes into the title's reserved ring region takes effect
// when the command processor reaches it - after every draw and resolve the
// title queued before it - so scan-out never shows a front buffer its resolve
// has not written yet. Publishing the flip from the CPU call instead, as VdSwap
// once did, let a vsync present the buffer while its frame was still queued:
// stale and half-written frames, and the real one never shown.
//
// The record rides in a type-3 NOP payload. Hardware skips NOP payloads, so the
// ring stays a valid Xenos command stream; Xenon's command processor recognises
// the signature and performs the flip in stream order.
struct FrontBufferSwap {
  std::uint32_t base_address{};  // physical
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t pitch{};
  std::uint8_t format{};  // raw Xenos texture-format code
  bool has_fetch_constant{};
  std::array<std::uint32_t, 6> fetch_constant{};

  friend bool operator==(const FrontBufferSwap&, const FrontBufferSwap&) = default;
};

inline constexpr std::uint32_t kFrontBufferSwapSignature = 0x58535750u;  // "XSWP"
// signature, base, width, height, pitch, format | has_fetch << 8, fetch[6]
inline constexpr std::uint16_t kFrontBufferSwapPayloadDwords = 12u;
inline constexpr std::uint32_t kFrontBufferSwapPacketDwords = 1u + kFrontBufferSwapPayloadDwords;

// The complete NOP packet: header followed by its payload, in logical dwords.
[[nodiscard]] constexpr std::array<std::uint32_t, kFrontBufferSwapPacketDwords>
encode_front_buffer_swap(const FrontBufferSwap& swap) {
  std::array<std::uint32_t, kFrontBufferSwapPacketDwords> packet{};
  packet[0] = make_packet_type3(Type3Opcode::Nop, kFrontBufferSwapPayloadDwords);
  packet[1] = kFrontBufferSwapSignature;
  packet[2] = swap.base_address;
  packet[3] = swap.width;
  packet[4] = swap.height;
  packet[5] = swap.pitch;
  packet[6] = std::uint32_t(swap.format) | (std::uint32_t(swap.has_fetch_constant) << 8);
  for (std::size_t i = 0; i < swap.fetch_constant.size(); ++i)
    packet[7 + i] = swap.fetch_constant[i];
  return packet;
}

// The swap a NOP payload carries, if it is one.
[[nodiscard]] constexpr std::optional<FrontBufferSwap> decode_front_buffer_swap(
    std::span<const std::uint32_t> nop_payload) noexcept {
  if (nop_payload.size() != kFrontBufferSwapPayloadDwords ||
      nop_payload[0] != kFrontBufferSwapSignature || (nop_payload[5] >> 9) != 0u)
    return std::nullopt;
  FrontBufferSwap swap{};
  swap.base_address = nop_payload[1];
  swap.width = nop_payload[2];
  swap.height = nop_payload[3];
  swap.pitch = nop_payload[4];
  swap.format = static_cast<std::uint8_t>(nop_payload[5]);
  swap.has_fetch_constant = ((nop_payload[5] >> 8) & 1u) != 0u;
  for (std::size_t i = 0; i < swap.fetch_constant.size(); ++i)
    swap.fetch_constant[i] = nop_payload[6 + i];
  return swap;
}

}  // namespace xenon::gpu
