#include "xenon/xam/xam_net_exports.hpp"

#if defined(_WIN32)
// Must come after any Windows headers already pulled in transitively (same
// ordering constraint as everywhere else in this codebase that touches
// winsock2.h) - defined here, first, so this file owns its own ordering
// rather than depending on include order elsewhere.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#endif

#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {

bool register_net_exports(core::ExportRegistry& registry) {
  bool ok = true;

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

  return ok;
}

}  // namespace xenon::xam
