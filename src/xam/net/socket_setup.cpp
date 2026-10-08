// Socket lifetime and configuration exports: socket, closesocket, shutdown,
// ioctlsocket, setsockopt/getsockopt, bind, connect and select.

#include "xam/net/xam_net_internal.hpp"

namespace xenon::xam {

#if defined(_WIN32)
bool register_net_socket_setup_exports(core::ExportRegistry& registry,
                                       const std::shared_ptr<XamSocketManager>& sockets) {
  bool ok = true;

  // NetDll_socket (0x03) - SOCKET socket(int caller, int af, int type, int protocol)
  //
  // Guest ABI: r3 = caller type, r4 = address family, r5 = socket type, r6 =
  // protocol -> r3 = guest socket handle, or -1 on failure (WSAGetLastError()
  // reports why). af/type/protocol are the same numeric values as host Winsock's
  // (both are Berkeley-socket-derived), so they pass through unchanged. A real
  // host socket is created here (see XamSocketManager) - this is genuine local
  // resource allocation with no network dependency, exactly like real Xbox 360
  // hardware's socket() (only later connect()/send() calls can observe "no
  // network"), so it must not be faked with a fabricated handle.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_socket";
    desc.ordinal = ordinal::NetDll_socket;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto af = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto type = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
      const auto protocol = static_cast<std::uint32_t>(ctx.cpu.gpr[6]);
      ctx.cpu.gpr[3] = sockets->create(af, type, protocol, ctx.thread_id);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_closesocket (0x04) - int closesocket(int caller, SOCKET s)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_closesocket";
    desc.ordinal = ordinal::NetDll_closesocket;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      ctx.cpu.gpr[3] = sockets->close(handle, ctx.thread_id) ? 0u : static_cast<std::uint32_t>(-1);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_shutdown (0x05) - int shutdown(int caller, SOCKET s, int how)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_shutdown";
    desc.ordinal = ordinal::NetDll_shutdown;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto how = static_cast<int>(ctx.cpu.gpr[5]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      const int result = ::shutdown(*native, how);
      if (result != 0) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_ioctlsocket (0x06) - int ioctlsocket(int caller, SOCKET s, long cmd, u_long* arg)
  //
  // The real ABI's `cmd` values (FIONBIO/FIONREAD) are the same numeric constants
  // on the host's Winsock, so the guest's cmd forwards unchanged - only the single
  // DWORD argument needs big-endian marshalling.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_ioctlsocket";
    desc.ordinal = ordinal::NetDll_ioctlsocket;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto cmd = static_cast<long>(ctx.cpu.gpr[5]);
      const auto arg_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      u_long arg = arg_ptr != 0u ? ctx.memory.read32_be(arg_ptr) : 0u;
      const int result = ::ioctlsocket(*native, cmd, &arg);
      if (arg_ptr != 0u) ctx.memory.write32_be(arg_ptr, static_cast<std::uint32_t>(arg));
      if (result != 0) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_setsockopt (0x07) - int setsockopt(int caller, SOCKET s, int level, int
  // optname, const char* optval, int optlen)
  //
  // Xbox 360 titles overwhelmingly use this for a single 4-byte BOOL/int option
  // (SO_BROADCAST, SO_REUSEADDR, SO_SNDBUF, SO_RCVBUF, ...) - level/optname share
  // the host's numeric values (verified against xenia/rexglue: XNet's socket-option
  // constants are the plain Winsock ones), and the value itself is a local
  // parameter (not a network-order field), so a plain big-endian read is correct.
  // A non-4-byte optlen (some exotic option this title does not use) is rejected
  // with a real error rather than guessing at its layout.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_setsockopt";
    desc.ordinal = ordinal::NetDll_setsockopt;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "only 4-byte (DWORD) option values are marshalled; a "
                        "differently-sized optval is rejected with WSAEFAULT "
                        "rather than guessed at";
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto level = static_cast<int>(ctx.cpu.gpr[5]);
      const auto optname = static_cast<int>(ctx.cpu.gpr[6]);
      const auto optval_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);
      const auto optlen = static_cast<std::uint32_t>(ctx.cpu.gpr[8]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      if (optlen != 4u || optval_ptr == 0u) {
        sockets->set_last_error(ctx.thread_id, WSAEFAULT);
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
        return true;
      }
      const std::uint32_t value = ctx.memory.read32_be(optval_ptr);
      const int result = ::setsockopt(*native, level, optname,
                                      reinterpret_cast<const char*>(&value), sizeof(value));
      if (result != 0) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_getsockopt (0x08) - the read side of setsockopt above; same 4-byte-only
  // scope and the same reasoning for why that is a real, not a guessed, contract.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_getsockopt";
    desc.ordinal = ordinal::NetDll_getsockopt;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "only round-trips a 4-byte (DWORD) option value";
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto level = static_cast<int>(ctx.cpu.gpr[5]);
      const auto optname = static_cast<int>(ctx.cpu.gpr[6]);
      const auto optval_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);
      const auto optlen_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      if (optval_ptr == 0u || optlen_ptr == 0u || ctx.memory.read32_be(optlen_ptr) < 4u) {
        sockets->set_last_error(ctx.thread_id, WSAEFAULT);
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
        return true;
      }
      std::uint32_t value = 0;
      int native_len = sizeof(value);
      const int result = ::getsockopt(*native, level, optname, reinterpret_cast<char*>(&value),
                                      &native_len);
      if (result != 0) {
        sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      } else {
        ctx.memory.write32_be(optval_ptr, value);
        ctx.memory.write32_be(optlen_ptr, 4u);
      }
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_bind (0x0B) - int bind(int caller, SOCKET s, const XSOCKADDR_IN* name, int namelen)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_bind";
    desc.ordinal = ordinal::NetDll_bind;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "AF_INET (IPv4) addresses only, matching real Xbox 360 XNet";
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto name_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      SOCKADDR_IN address{};
      if (!read_sockaddr_in(ctx.memory, name_ptr, address)) {
        sockets->set_last_error(ctx.thread_id, WSAEAFNOSUPPORT);
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
        return true;
      }
      const int result =
          ::bind(*native, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address));
      if (result != 0) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_connect (0x0C) - int connect(int caller, SOCKET s, const XSOCKADDR* name, int namelen)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_connect";
    desc.ordinal = ordinal::NetDll_connect;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "AF_INET (IPv4) addresses only, matching real Xbox 360 XNet";
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto name_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      SOCKADDR_IN address{};
      if (!read_sockaddr_in(ctx.memory, name_ptr, address)) {
        sockets->set_last_error(ctx.thread_id, WSAEAFNOSUPPORT);
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
        return true;
      }
      const int result =
          ::connect(*native, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address));
      if (result != 0) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_select (0x0F) - int select(int caller, int nfds, x_fd_set* readfds,
  // x_fd_set* writefds, x_fd_set* exceptfds, const timeval* timeout)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_select";
    desc.ordinal = ordinal::NetDll_select;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto read_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto write_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);
      const auto except_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);
      const auto timeout_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);

      GuestFdSet read_set, write_set, except_set;
      const bool has_read = read_fd_set(ctx.memory, read_ptr, read_set);
      const bool has_write = read_fd_set(ctx.memory, write_ptr, write_set);
      const bool has_except = read_fd_set(ctx.memory, except_ptr, except_set);

      const auto build_native = [&](const GuestFdSet& guest_set, fd_set& native_set) {
        FD_ZERO(&native_set);
        for (const auto handle : guest_set.handles) {
          const auto native = sockets->resolve(handle, ctx.thread_id);
          if (native) FD_SET(static_cast<SOCKET>(*native), &native_set);
        }
      };
      fd_set native_read{}, native_write{}, native_except{};
      if (has_read) build_native(read_set, native_read);
      if (has_write) build_native(write_set, native_write);
      if (has_except) build_native(except_set, native_except);

      TIMEVAL timeout{};
      TIMEVAL* timeout_arg = nullptr;
      if (timeout_ptr != 0u) {
        timeout.tv_sec = static_cast<long>(ctx.memory.read32_be(timeout_ptr));
        timeout.tv_usec = static_cast<long>(ctx.memory.read32_be(timeout_ptr + 4u));
        timeout_arg = &timeout;
      }

      const int result = ::select(0, has_read ? &native_read : nullptr,
                                  has_write ? &native_write : nullptr,
                                  has_except ? &native_except : nullptr, timeout_arg);
      if (result == SOCKET_ERROR) {
        sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      } else {
        const auto filter = [&](GuestFdSet& guest_set, const fd_set& native_set) {
          GuestFdSet remaining;
          for (const auto handle : guest_set.handles) {
            const auto native = sockets->resolve(handle, ctx.thread_id);
            if (native && FD_ISSET(static_cast<SOCKET>(*native), &native_set)) {
              remaining.handles.push_back(handle);
            }
          }
          guest_set = std::move(remaining);
        };
        if (has_read) { filter(read_set, native_read); write_fd_set(ctx.memory, read_ptr, read_set); }
        if (has_write) { filter(write_set, native_write); write_fd_set(ctx.memory, write_ptr, write_set); }
        if (has_except) { filter(except_set, native_except); write_fd_set(ctx.memory, except_ptr, except_set); }
      }
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}
#endif  // _WIN32

}  // namespace xenon::xam
