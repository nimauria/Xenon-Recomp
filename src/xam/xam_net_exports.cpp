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
#include <string>
#include <vector>

#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/xam_socket_manager.hpp"

namespace xenon::xam {

namespace {

#if defined(_WIN32)
// AF_INET only: real Xbox 360 XNet addressing is exclusively IPv4, and this is the
// one address family every verified reference (xenia, rexglue-sdk) implements -
// there is no real AF_INET6 traffic to be correct or incorrect about here.
constexpr std::uint16_t kAfInet = 2u;
constexpr std::size_t kSockAddrInSize = 16u;  // sin_family, sin_port, sin_addr, 8 zero bytes

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
bool read_sockaddr_in(cpu::MemoryPort& memory, cpu::GuestAddress address, SOCKADDR_IN& out) {
  std::memset(&out, 0, sizeof(out));
  if (address == 0u) return false;
  const auto family = memory.read16_be(address + 0u);
  if (family != kAfInet) return false;
  out.sin_family = AF_INET;
  out.sin_port = ::htons(memory.read16_be(address + 2u));
  out.sin_addr.s_addr = ::htonl(memory.read32_be(address + 4u));
  return true;
}

void write_sockaddr_in(cpu::MemoryPort& memory, cpu::GuestAddress address,
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
constexpr std::uint32_t kMaxTransferBytes = 1u << 20;  // 1 MiB

std::vector<std::byte> read_guest_buffer(cpu::MemoryPort& memory, cpu::GuestAddress address,
                                         std::uint32_t length) {
  std::vector<std::byte> bytes(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<std::byte>(memory.read8(address + i));
  }
  return bytes;
}

void write_guest_buffer(cpu::MemoryPort& memory, cpu::GuestAddress address,
                        const std::vector<std::byte>& bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    memory.write8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(bytes[i]));
  }
}

// Resolves r4 (the guest socket handle) to a native SOCKET, or fails the call with
// WSAENOTSOCK (matching every NetDll_* handler below - see resolve()'s own doc
// comment). Shared by every socket-taking export so the not-a-socket contract is
// identical everywhere rather than re-implemented per handler.
std::optional<SOCKET> resolve_socket(XamSocketManager& sockets, core::ExportCallContext& ctx,
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
constexpr std::uint32_t kMaxFdSetEntries = 64u;

struct GuestFdSet {
  std::vector<XamSocketManager::GuestHandle> handles{};
};

bool read_fd_set(cpu::MemoryPort& memory, cpu::GuestAddress address, GuestFdSet& out) {
  if (address == 0u) return false;
  const auto count = memory.read32_be(address);
  out.handles.reserve(std::min<std::uint32_t>(count, kMaxFdSetEntries));
  for (std::uint32_t i = 0; i < count && i < kMaxFdSetEntries; ++i) {
    out.handles.push_back(memory.read32_be(address + 4u + i * 4u));
  }
  return true;
}

void write_fd_set(cpu::MemoryPort& memory, cpu::GuestAddress address, const GuestFdSet& in) {
  if (address == 0u) return;
  memory.write32_be(address, static_cast<std::uint32_t>(in.handles.size()));
  for (std::uint32_t i = 0; i < in.handles.size(); ++i) {
    memory.write32_be(address + 4u + i * 4u, in.handles[i]);
  }
}
#endif  // _WIN32

}  // namespace

bool register_net_exports(core::ExportRegistry& registry) {
  bool ok = true;
  // Owns every socket opened through the exports registered below - see this
  // class's own doc comment for why the lifetime is tied to this one
  // registration call (one per XenonSession::initialize()) rather than being a
  // process-wide or global singleton.
  auto sockets = std::make_shared<XamSocketManager>();

  // NetDll_WSAStartup (0x01) - int WSAStartup(WORD version, X_WSADATA* data)
  //
  // Guest ABI: r3 = caller type, r4 = requested WinSock version (WORD, low
  // byte = major), r5 = X_WSADATA* out -> r3 = WinSock error code (0 =
  // success). Real hardware and every mature Xbox 360 emulator (xenia,
  // rexglue-sdk's src/kernel/xam/xam_net.cpp NetDll_WSAStartup_entry) answer
  // this by calling the HOST's real WSAStartup and copying its real result
  // back into the guest struct - this is a genuine local initialization with
  // no network dependency, exactly like XNetStartup above, not a stub.
  //
  // Only the fields with a verified real offset/consequence are populated:
  //   - wVersion (offset 0, WORD): the negotiated version, always 2.2 (the
  //     version every Xbox 360 title's XNet/WinSock stack was built against).
  //   - vendor_info_ptr (offset 0x190 / 400, DWORD): round-tripped unchanged
  //     from whatever the guest struct already held. rexglue-sdk's reference
  //     documents that some titles (their comment cites Xbox title ID
  //     5841099F) compare this value across calls and bugcheck if it
  //     changes, so Xenon preserves that exact contract rather than writing
  //     a new value here.
  // The remaining descriptive fields (version_high, description,
  // system_status, max_sockets, max_udpdg) are real Winsock output nothing
  // observed in practice inspects programmatically - rather than guess at
  // this struct's exact real padding/alignment (the verified 0x190 vendor
  // offset does not fall out of a naive sum of the documented field sizes,
  // meaning there is compiler-inserted padding this reference does not spell
  // out), the struct region is left untouched beyond the two verified
  // fields, matching a real conservative implementation that reports success
  // without claiming byte-exact knowledge of fields nothing here reads back.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_WSAStartup";
    desc.ordinal = ordinal::NetDll_WSAStartup;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "only writes the two X_WSADATA fields with a verified real offset "
        "(wVersion at +0, vendor_info_ptr round-tripped at +0x190); the "
        "struct's other descriptive fields (version_high, description, "
        "system_status, max_sockets, max_udpdg) are left as the guest "
        "already had them rather than guessing at unverified padding/offsets";
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
#if defined(_WIN32)
      WSADATA host_data{};
      const int result = ::WSAStartup(MAKEWORD(2, 2), &host_data);
      if (result != 0) {
        ctx.cpu.gpr[3] = static_cast<std::uint32_t>(result);
        return true;
      }
#endif
      if (data_ptr != 0u) {
        ctx.memory.write16_be(data_ptr + 0u, 0x0202u);  // wVersion = 2.2
        const auto vendor_ptr = ctx.memory.read32_be(data_ptr + 0x190u);
        ctx.memory.write32_be(data_ptr + 0x190u, vendor_ptr);
      }
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_WSACleanup (0x02) - int WSACleanup()
  //
  // Guest ABI: r3 = caller type -> r3 = WinSock error code (0 = success).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_WSACleanup";
    desc.ordinal = ordinal::NetDll_WSACleanup;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
#if defined(_WIN32)
      ::WSACleanup();
#endif
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_XNetStartup (0x33) - int XNetStartup(XNetStartupParams* params)
  //
  // r3 = caller type (XNCALLER_TITLE etc.), r4 = optional params struct
  // pointer. Real hardware initializes the local XNet/Winsock subsystem and
  // returns 0 unconditionally here - this succeeds with no network cable and
  // no Xbox Live signin, since it only prepares local state; whether a real
  // connection exists is only observable through later calls (e.g. a future
  // XNetGetEthernetLinkStatus/XNetGetTitleXnAddr implementation). Xenon does
  // not yet have a consumer for the params struct's tunables (socket/QoS
  // limits), so it is intentionally not copied out of guest memory yet - add
  // that once XNetGetOpt(option=1) round-trips it back to guest code.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_XNetStartup";
    desc.ordinal = ordinal::NetDll_XNetStartup;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_XNetCleanup (0x34) - int XNetCleanup(void* params)
  //
  // Tears down the local XNet/Winsock state XNetStartup set up. Since Xenon
  // does not yet hold any allocated XNet resources to release (see the
  // XNetStartup note above), this is a genuine no-op on real hardware too
  // when nothing was ever allocated - it always returns 0.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_XNetCleanup";
    desc.ordinal = ordinal::NetDll_XNetCleanup;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

#if defined(_WIN32)
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
#endif  // _WIN32

  return ok;
}

}  // namespace xenon::xam
