// Data transfer and addressing exports: recv/recvfrom, send/sendto,
// inet_addr and the per-thread WSA last-error pair.

#include "xam/net/xam_net_internal.hpp"

namespace xenon::xam {

#if defined(_WIN32)
bool register_net_transfer_exports(core::ExportRegistry& registry,
                                   const std::shared_ptr<XamSocketManager>& sockets) {
  bool ok = true;

  // NetDll_recv (0x12) - int recv(int caller, SOCKET s, char* buf, int len, int flags)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_recv";
    desc.ordinal = ordinal::NetDll_recv;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto buf_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto len = std::min<std::uint32_t>(static_cast<std::uint32_t>(ctx.cpu.gpr[6]), kMaxTransferBytes);
      const auto flags = static_cast<int>(ctx.cpu.gpr[7]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      std::vector<char> buffer(len);
      const int result = ::recv(*native, buffer.data(), static_cast<int>(len), flags);
      if (result == SOCKET_ERROR) {
        sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      } else if (result > 0 && buf_ptr != 0u) {
        for (int i = 0; i < result; ++i) {
          ctx.memory.write8(buf_ptr + static_cast<std::uint32_t>(i),
                            static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(i)]));
        }
      }
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_recvfrom (0x14) - int recvfrom(int caller, SOCKET s, char* buf, int len,
  // int flags, XSOCKADDR_IN* from, int* fromlen)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_recvfrom";
    desc.ordinal = ordinal::NetDll_recvfrom;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto buf_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto len = std::min<std::uint32_t>(static_cast<std::uint32_t>(ctx.cpu.gpr[6]), kMaxTransferBytes);
      const auto flags = static_cast<int>(ctx.cpu.gpr[7]);
      const auto from_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      std::vector<char> buffer(len);
      SOCKADDR_IN from{};
      int from_len = sizeof(from);
      const int result = ::recvfrom(*native, buffer.data(), static_cast<int>(len), flags,
                                    reinterpret_cast<SOCKADDR*>(&from), &from_len);
      if (result == SOCKET_ERROR) {
        sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      } else {
        if (result > 0 && buf_ptr != 0u) {
          for (int i = 0; i < result; ++i) {
            ctx.memory.write8(buf_ptr + static_cast<std::uint32_t>(i),
                              static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(i)]));
          }
        }
        if (from_ptr != 0u) write_sockaddr_in(ctx.memory, from_ptr, from);
      }
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_send (0x16) - int send(int caller, SOCKET s, const char* buf, int len, int flags)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_send";
    desc.ordinal = ordinal::NetDll_send;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto buf_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto len = std::min<std::uint32_t>(static_cast<std::uint32_t>(ctx.cpu.gpr[6]), kMaxTransferBytes);
      const auto flags = static_cast<int>(ctx.cpu.gpr[7]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      const auto bytes = read_guest_buffer(ctx.memory, buf_ptr, len);
      const int result = ::send(*native, reinterpret_cast<const char*>(bytes.data()),
                                static_cast<int>(len), flags);
      if (result == SOCKET_ERROR) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_sendto (0x18) - int sendto(int caller, SOCKET s, const char* buf, int
  // len, int flags, const XSOCKADDR_IN* to, int tolen)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_sendto";
    desc.ordinal = ordinal::NetDll_sendto;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "AF_INET (IPv4) destination addresses only";
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<XamSocketManager::GuestHandle>(ctx.cpu.gpr[4]);
      const auto buf_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto len = std::min<std::uint32_t>(static_cast<std::uint32_t>(ctx.cpu.gpr[6]), kMaxTransferBytes);
      const auto flags = static_cast<int>(ctx.cpu.gpr[7]);
      const auto to_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);
      const auto native = resolve_socket(*sockets, ctx, handle);
      if (!native) return true;
      SOCKADDR_IN to{};
      if (!read_sockaddr_in(ctx.memory, to_ptr, to)) {
        sockets->set_last_error(ctx.thread_id, WSAEAFNOSUPPORT);
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
        return true;
      }
      const auto bytes = read_guest_buffer(ctx.memory, buf_ptr, len);
      const int result = ::sendto(*native, reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<int>(len), flags,
                                  reinterpret_cast<const SOCKADDR*>(&to), sizeof(to));
      if (result == SOCKET_ERROR) sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(::WSAGetLastError()));
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_inet_addr (0x1A) - unsigned long inet_addr(const char* cp)
  //
  // Guest ABI: r3 = guest C-string pointer -> r3 = IPv4 address (already in
  // network byte order - matches the real Xbox 360 ABI documented by every
  // reference, which byte-swaps the host's inet_addr() result before returning
  // it to a big-endian guest register, since the guest reads this as a plain
  // 32-bit register value, not through a memory store subject to guest
  // endianness). An empty string returns 0 (old-style inet_addr() convention,
  // not the modern -1-for-any-failure one) - verified against xenia's own
  // documented special case for exactly this ABI.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_inet_addr";
    desc.ordinal = ordinal::NetDll_inet_addr;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto str_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
      if (str_ptr == 0u) {
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(-1);
        return true;
      }
      std::string text;
      for (std::uint32_t offset = 0; offset < 256u; ++offset) {
        const auto c = ctx.memory.read8(str_ptr + offset);
        if (c == 0u) break;
        text.push_back(static_cast<char>(c));
      }
      const unsigned long address = ::inet_addr(text.c_str());
      if (address == INADDR_NONE && text.empty()) {
        ctx.cpu.gpr[3] = 0u;
        return true;
      }
      // inet_addr() already returns network-byte-order bytes; reinterpreting
      // those same bytes as a big-endian VALUE (rather than converting) is what
      // reproduces them correctly in the guest's big-endian register - an
      // ntohl() here would silently reverse them a second time.
      std::uint32_t network_order_bytes{};
      std::memcpy(&network_order_bytes, &address, sizeof(network_order_bytes));
      ctx.cpu.gpr[3] = ::ntohl(network_order_bytes);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_WSAGetLastError (0x1B) - int WSAGetLastError()
  //
  // Guest ABI: no arguments -> r3 = the calling guest thread's last WSA error,
  // matching real hardware's per-thread XThread::GetLastError() (a socket op
  // failure on one guest thread must not clobber another's error code).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_WSAGetLastError";
    desc.ordinal = ordinal::NetDll_WSAGetLastError;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = sockets->last_error(ctx.thread_id);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_WSASetLastError (0x1C) - void WSASetLastError(int error)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_WSASetLastError";
    desc.ordinal = ordinal::NetDll_WSASetLastError;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [sockets](core::ExportCallContext& ctx) -> bool {
      sockets->set_last_error(ctx.thread_id, static_cast<std::uint32_t>(ctx.cpu.gpr[3]));
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}
#endif  // _WIN32

}  // namespace xenon::xam
