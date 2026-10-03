#include "xenon/xam/xam_socket_manager.hpp"

#if defined(_WIN32)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#endif

namespace xenon::xam {

XamSocketManager::~XamSocketManager() {
  std::scoped_lock lock(mutex_);
#if defined(_WIN32)
  for (const auto& [guest_handle, native] : sockets_) {
    ::closesocket(static_cast<SOCKET>(native));
  }
  if (winsock_ready_) ::WSACleanup();
#endif
  sockets_.clear();
}

void XamSocketManager::ensure_winsock_ready() {
#if defined(_WIN32)
  // Called with mutex_ already held by create().
  if (winsock_ready_) return;
  WSADATA data{};
  // Idempotent from the host's point of view regardless of whether the guest
  // separately called NetDll_WSAStartup (Winsock reference-counts WSAStartup/
  // WSACleanup pairs internally) - this only guarantees Xenon's OWN pairing is
  // balanced (one Startup here, one Cleanup in the destructor above).
  winsock_ready_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
}

XamSocketManager::GuestHandle XamSocketManager::create(std::uint32_t af, std::uint32_t type,
                                                        std::uint32_t protocol,
                                                        std::uint32_t thread_id) {
#if defined(_WIN32)
  {
    std::scoped_lock lock(mutex_);
    ensure_winsock_ready();
  }
  const SOCKET native = ::socket(static_cast<int>(af), static_cast<int>(type),
                                 static_cast<int>(protocol));
  if (native == INVALID_SOCKET) {
    set_last_error(thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
    return kInvalidHandle;
  }
  std::scoped_lock lock(mutex_);
  GuestHandle handle = next_handle_++;
  // next_handle_ is a monotonically increasing 32-bit counter; wrap-around would
  // require ~4 billion concurrently-open sockets in one session, which real
  // hardware cannot sustain either (a handful of real socket descriptors max).
  sockets_.emplace(handle, static_cast<std::uintptr_t>(native));
  return handle;
#else
  // No host socket API wired up on this platform yet - real WSA error, not a fake
  // handle (WSAEAFNOSUPPORT: the requested address family is not supported here).
  static_cast<void>(af);
  static_cast<void>(type);
  static_cast<void>(protocol);
  set_last_error(thread_id, 10047u);
  return kInvalidHandle;
#endif
}

XamSocketManager::GuestHandle XamSocketManager::adopt(std::uintptr_t native_socket) {
  std::scoped_lock lock(mutex_);
  GuestHandle handle = next_handle_++;
  sockets_.emplace(handle, native_socket);
  return handle;
}

bool XamSocketManager::close(GuestHandle handle, std::uint32_t thread_id) {
  std::uintptr_t native = 0;
  {
    std::scoped_lock lock(mutex_);
    const auto it = sockets_.find(handle);
    if (it == sockets_.end()) {
      // Not set_last_error(): mutex_ is already held here (non-recursive), and
      // set_last_error() takes it again - see resolve()'s equivalent inline write.
      last_error_[thread_id] = kNotSocket;
      return false;
    }
    native = it->second;
    sockets_.erase(it);
  }
#if defined(_WIN32)
  ::closesocket(static_cast<SOCKET>(native));
#else
  static_cast<void>(native);
#endif
  return true;
}

std::optional<std::uintptr_t> XamSocketManager::resolve(GuestHandle handle,
                                                         std::uint32_t thread_id) {
  std::scoped_lock lock(mutex_);
  const auto it = sockets_.find(handle);
  if (it == sockets_.end()) {
    last_error_[thread_id] = kNotSocket;
    return std::nullopt;
  }
  return it->second;
}

void XamSocketManager::set_last_error(std::uint32_t thread_id, std::uint32_t error) noexcept {
  std::scoped_lock lock(mutex_);
  last_error_[thread_id] = error;
}

std::uint32_t XamSocketManager::last_error(std::uint32_t thread_id) const noexcept {
  std::scoped_lock lock(mutex_);
  const auto it = last_error_.find(thread_id);
  return it == last_error_.end() ? 0u : it->second;
}

std::size_t XamSocketManager::open_count() const {
  std::scoped_lock lock(mutex_);
  return sockets_.size();
}

}  // namespace xenon::xam
