// Listening-socket exports: getsockname, listen, accept and __WSAFDIsSet.

#include "xam/net/xam_net_internal.hpp"

namespace xenon::xam {

#if defined(_WIN32)
bool register_net_listen_exports(core::ExportRegistry& registry,
                                 const std::shared_ptr<XamSocketManager>& sockets) {
  bool ok = true;

  // NetDll_getsockname (0x09) - int getsockname(int caller, SOCKET s, XSOCKADDR_IN* name, int* namelen)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_getsockname";
    desc.ordinal = ordinal::NetDll_getsockname;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto name_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      SOCKADDR_IN address{};
      int address_len = sizeof(address);
      const int result =
          ::getsockname(*native, reinterpret_cast<SOCKADDR*>(&address), &address_len);
      if (result != 0) {
        sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      } else if (name_ptr != 0u) {
        write_sockaddr_in(ctx.memory, name_ptr, address);
      }
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_listen (0x0D) - int listen(int caller, SOCKET s, int backlog)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_listen";
    desc.ordinal = ordinal::NetDll_listen;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto backlog = static_cast<int>(ctx.cpu.gpr[5]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      const int result = ::listen(*native, backlog);
      if (result != 0) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_accept (0x0E) - SOCKET accept(int caller, SOCKET s, XSOCKADDR_IN* addr, int* addrlen)
  //
  // Accept produces a genuinely new, distinct native socket for the accepted
  // connection - not the same descriptor as the listening socket - so it is
  // registered under its own new guest handle via XamSocketManager::adopt(),
  // the same handle namespace NetDll_socket allocates from.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_accept";
    desc.ordinal = ordinal::NetDll_accept;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto addr_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      SOCKADDR_IN address{};
      int address_len = sizeof(address);
      const SOCKET accepted = ::accept(*native, reinterpret_cast<SOCKADDR*>(&address), &address_len);
      if (accepted == INVALID_SOCKET) {
        sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(XamSocketManager::kInvalidHandle);
        return true;
      }
      if (addr_ptr != 0u) write_sockaddr_in(ctx.memory, addr_ptr, address);
      ctx.cpu.gpr[3] = sockets->adopt(static_cast<std::uintptr_t>(accepted));
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll___WSAFDIsSet (0x22) - int __WSAFDIsSet(SOCKET s, x_fd_set* set)
  //
  // Real ABI takes the guest socket handle directly (not through the
  // `caller` convention most NetDll_* calls use), matching every available
  // reference - this checks the guest's own handle list rather than
  // resolving to a native SOCKET first, since the guest fd_set already
  // stores guest handles (see GuestFdSet/read_fd_set above).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll___WSAFDIsSet";
    desc.ordinal = ordinal::NetDll___WSAFDIsSet;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[3]);
      const auto set_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      GuestFdSet set;
      if (!read_fd_set(ctx.memory, set_ptr, set)) {
        ctx.cpu.gpr[3] = 0u;
        return true;
      }
      const auto found = std::find(set.handles.begin(), set.handles.end(), handle) != set.handles.end();
      ctx.cpu.gpr[3] = found ? 1u : 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}
#endif  // _WIN32

}  // namespace xenon::xam
