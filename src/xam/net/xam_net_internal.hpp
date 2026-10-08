#pragma once

// Private to the XAM network exports: guest <-> host marshalling for
// sockaddr_in, buffers, socket handles and fd_sets. Windows only, like the
// socket exports that use it.

#include "xenon/xam/xam_net_exports.hpp"

#if defined(_WIN32)
// Must come after any Windows headers already pulled in transitively (same
// ordering constraint as everywhere else in this codebase that touches
// winsock2.h) - defined here, first, so this file owns its own ordering
// rather than depending on include order elsewhere.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/xam_socket_manager.hpp"

namespace xenon::xam {

#if defined(_WIN32)
// AF_INET only: real Xbox 360 XNet addressing is exclusively IPv4, and this is the
// one address family every verified reference (xenia, rexglue-sdk) implements -
// there is no real AF_INET6 traffic to be correct or incorrect about here.
inline constexpr std::uint16_t kAfInet = 2u;
inline constexpr std::size_t kSockAddrInSize = 16u;  // sin_family, sin_port, sin_addr, 8 zero bytes

// The guest's XSOCKADDR_IN is byte-for-byte the standard Berkeley sockaddr_in
// layout (verified against every available reference - it exists specifically to
// be wire-compatible with BSD sockets). sin_port/sin_addr are true network-order
// fields: since the guest CPU is big-endian, the raw bytes the guest already wrote
// there (via a no-op htons/htonl on real hardware) are numerically read correctly
// by read16_be/read32_be into a host-native integer VALUE - which must then be
// re-encoded into network-order bytes for the host's (little-endian) sockaddr_in
// via htons/htonl. sin_family is not a network-order field (just a copied
// discriminator), so no byte-order conversion applies to it beyond the normal
// big-endian guest read.
inline bool read_sockaddr_in(cpu::MemoryPort& memory, cpu::GuestAddress address, SOCKADDR_IN& out) {
  std::memset(&out, 0, sizeof(out));
  if (address == 0u) return false;
  const auto family = memory.read16_be(address + 0u);
  if (family != kAfInet) return false;
  out.sin_family = AF_INET;
  out.sin_port = ::htons(memory.read16_be(address + 2u));
  out.sin_addr.s_addr = ::htonl(memory.read32_be(address + 4u));
  return true;
}

inline void write_sockaddr_in(cpu::MemoryPort& memory, cpu::GuestAddress address,
                       const SOCKADDR_IN& in) {
  if (address == 0u) return;
  memory.write16_be(address + 0u, kAfInet);
  memory.write16_be(address + 2u, ::ntohs(in.sin_port));
  memory.write32_be(address + 4u, ::ntohl(in.sin_addr.s_addr));
  for (std::uint32_t i = 8u; i < kSockAddrInSize; ++i) memory.write8(address + i, 0u);
}

// Bounds a guest-supplied transfer length before allocating a host buffer for it -
// same defensive contract as xboxkrnl NtReadFile/NtWriteFile's own transfer cap.
// Real Xbox 360 UDP/TCP sends are always far smaller than this.
inline constexpr std::uint32_t kMaxTransferBytes = 1u << 20;  // 1 MiB

inline std::vector<std::byte> read_guest_buffer(cpu::MemoryPort& memory, cpu::GuestAddress address,
                                         std::uint32_t length) {
  std::vector<std::byte> bytes(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<std::byte>(memory.read8(address + i));
  }
  return bytes;
}

inline void write_guest_buffer(cpu::MemoryPort& memory, cpu::GuestAddress address,
                        const std::vector<std::byte>& bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    memory.write8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(bytes[i]));
  }
}

// Resolves r4 (the guest socket handle) to a native SOCKET, or fails the call with
// WSAENOTSOCK (matching every NetDll_* handler below - see resolve()'s own doc
// comment). Shared by every socket-taking export so the not-a-socket contract is
// identical everywhere rather than re-implemented per handler.
inline std::optional<SOCKET> resolve_socket(XamSocketManager& sockets, core::ExportCallContext& ctx,
                                     XamSocketManager::GuestHandle handle) {
  const auto native = sockets.resolve(handle, ctx.thread_id);
  if (!native) {
    ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
    return std::nullopt;
  }
  return static_cast<SOCKET>(*native);
}

// The guest's x_fd_set: a BE uint32 count followed by up to 64 BE uint32 guest
// socket handles (verified layout - xenia's identical x_fd_set, itself matching
// the real Xbox 360 XNet select() ABI).
inline constexpr std::uint32_t kMaxFdSetEntries = 64u;

struct GuestFdSet {
  std::vector<XamSocketManager::GuestHandle> handles{};
};

inline bool read_fd_set(cpu::MemoryPort& memory, cpu::GuestAddress address, GuestFdSet& out) {
  if (address == 0u) return false;
  const auto count = memory.read32_be(address);
  out.handles.reserve(std::min<std::uint32_t>(count, kMaxFdSetEntries));
  for (std::uint32_t i = 0; i < count && i < kMaxFdSetEntries; ++i) {
    out.handles.push_back(memory.read32_be(address + 4u + i * 4u));
  }
  return true;
}

inline void write_fd_set(cpu::MemoryPort& memory, cpu::GuestAddress address, const GuestFdSet& in) {
  if (address == 0u) return;
  memory.write32_be(address, static_cast<std::uint32_t>(in.handles.size()));
  for (std::uint32_t i = 0; i < in.handles.size(); ++i) {
    memory.write32_be(address + 4u + i * 4u, in.handles[i]);
  }
}
#endif  // _WIN32

// Each registers one family of NetDll_* exports; see register_net_exports().
bool register_net_socket_setup_exports(core::ExportRegistry& registry,
                                       const std::shared_ptr<XamSocketManager>& sockets);
bool register_net_transfer_exports(core::ExportRegistry& registry,
                                   const std::shared_ptr<XamSocketManager>& sockets);
bool register_net_listen_exports(core::ExportRegistry& registry,
                                 const std::shared_ptr<XamSocketManager>& sockets);
bool register_xnet_exports(core::ExportRegistry& registry);

}  // namespace xenon::xam
