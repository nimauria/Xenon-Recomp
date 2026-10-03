#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace xenon::xam {

// Xbox 360 XNet/Winsock socket handles (the NetDll_* family, xam.xex ordinals
// 0x01-0x20ish) live in their own namespace, separate from the xboxkrnl
// Ob*/NT handle table - real hardware's AFD/Winsock socket values are not NT
// handles either. One instance is owned per session (see register_net_exports()),
// constructed fresh each time XenonSession::initialize() registers xam exports and
// destroyed when the export registry that captured it is cleared, so a session
// restart never sees a previous run's sockets or last-error state.
//
// Real socket I/O is delegated to the host's own Winsock/BSD sockets: an Xbox 360
// title calling socket()/bind()/sendto()/recvfrom() for LAN discovery or peer
// traffic gets a genuine, working socket, not a fabricated success. On a
// non-Windows build (no host socket API wired up here yet) every operation fails
// with a real WSA error rather than pretending to succeed.
class XamSocketManager {
 public:
  using GuestHandle = std::uint32_t;
  // NT/Winsock convention: 0 is a valid low handle value on some platforms, so
  // -1 (all bits set), not 0, is the real "invalid socket" sentinel every guest
  // checks r3 against.
  static constexpr GuestHandle kInvalidHandle = 0xFFFFFFFFu;

  XamSocketManager() = default;
  ~XamSocketManager();
  XamSocketManager(const XamSocketManager&) = delete;
  XamSocketManager& operator=(const XamSocketManager&) = delete;

  // af/type/protocol are the guest's raw AF_INET/SOCK_* values, which are numerically
  // identical to the host's (both are Berkeley-socket-derived). Returns kInvalidHandle
  // and records a WSA error for `thread_id` on failure.
  [[nodiscard]] GuestHandle create(std::uint32_t af, std::uint32_t type,
                                   std::uint32_t protocol, std::uint32_t thread_id);
  // Registers an already-open native socket descriptor (e.g. accept()'s
  // return value, which is a real, distinct, already-connected socket - not
  // a duplicate of the listening socket) under a new guest handle, the same
  // handle namespace create() allocates from.
  [[nodiscard]] GuestHandle adopt(std::uintptr_t native_socket);
  // Real hardware's closesocket() semantics: returns false (guest should see -1) if
  // `handle` is not a currently-open socket of this manager's.
  [[nodiscard]] bool close(GuestHandle handle, std::uint32_t thread_id);

  // The host socket descriptor for `handle`, or nullopt if not open (sets WSAENOTSOCK
  // for `thread_id` in that case - every NetDll_* op below needs exactly this check).
  [[nodiscard]] std::optional<std::uintptr_t> resolve(GuestHandle handle,
                                                       std::uint32_t thread_id);

  void set_last_error(std::uint32_t thread_id, std::uint32_t error) noexcept;
  [[nodiscard]] std::uint32_t last_error(std::uint32_t thread_id) const noexcept;
  // The real WSAENOTSOCK value (10038), used consistently by every NetDll_* handler
  // for "handle does not name an open socket of this process".
  static constexpr std::uint32_t kNotSocket = 10038u;

  [[nodiscard]] std::size_t open_count() const;

 private:
  // Ensures the host's Winsock is initialized before the first socket() call -
  // real Xbox 360 hardware's local socket stack needs no separate startup, so a
  // title is free to call NetDll_socket without ever calling NetDll_WSAStartup
  // first; this manager must not depend on the guest having done so.
  void ensure_winsock_ready();

  mutable std::mutex mutex_{};
  bool winsock_ready_{false};
  std::unordered_map<GuestHandle, std::uintptr_t> sockets_{};
  std::unordered_map<std::uint32_t, std::uint32_t> last_error_{};
  GuestHandle next_handle_{1};
};

}  // namespace xenon::xam
