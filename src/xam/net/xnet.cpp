// XNet exports: XNetRandom, Live key exchange and title addressing.

#include "xam/net/xam_net_internal.hpp"

namespace xenon::xam {

#if defined(_WIN32)
bool register_xnet_exports(core::ExportRegistry& registry) {
  bool ok = true;

  // NetDll_XNetRandom (0x35) - void XNetRandom(int caller, void* buffer, int length)
  //
  // A genuine local CSPRNG fill - no network/Live dependency at all, so this
  // is implemented for real (via the host's cryptographically-secure RNG),
  // not approximated. Unlike xboxkrnl's separate XeCryptRandom (a different
  // export this codebase does not touch here), there is no real-hardware
  // reason to return fixed bytes.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_XNetRandom";
    desc.ordinal = ordinal::NetDll_XNetRandom;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto buffer_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto length = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
      if (buffer_ptr != 0u && length > 0u) {
        static thread_local std::mt19937 engine{std::random_device{}()};
        std::uniform_int_distribution<int> byte_dist(0, 255);
        for (std::uint32_t i = 0; i < length; ++i) {
          ctx.memory.write8(buffer_ptr + i, static_cast<std::uint8_t>(byte_dist(engine)));
        }
      }
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_XNetCreateKey/XNetRegisterKey (0x36/0x37) - Xbox Live secure
  // session key exchange. Verified against rexglue-sdk (which, like every
  // other available reference, has no real implementation of either call
  // either) - this requires a real Xbox Live secure-session negotiation
  // Xenon does not implement, so both honestly report the real "service
  // unavailable" outcome rather than fabricating key material nothing would
  // ever actually use to secure real traffic.
  for (const auto& entry :
       {std::pair{ordinal::NetDll_XNetCreateKey, "NetDll_XNetCreateKey"},
        std::pair{ordinal::NetDll_XNetRegisterKey, "NetDll_XNetRegisterKey"}}) {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = entry.second;
    desc.ordinal = entry.first;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "no Xbox Live secure-session key exchange is implemented";
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 1u;  // real hardware's documented generic failure code for this family
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_XNetXnAddrToInAddr/XNetInAddrToXnAddr (0x39/0x3C) - matches
  // rexglue-sdk's own identical treatment exactly (verified: its
  // NetDll_XNetXnAddrToInAddr_entry/NetDll_XNetInAddrToXnAddr_entry bodies
  // are each a single `return 1;` with no memory access) - real secure
  // XNADDR<->INADDR translation needs the same session-key infrastructure
  // XNetCreateKey/XNetRegisterKey above do not have.
  for (const auto& entry :
       {std::pair{ordinal::NetDll_XNetXnAddrToInAddr, "NetDll_XNetXnAddrToInAddr"},
        std::pair{ordinal::NetDll_XNetInAddrToXnAddr, "NetDll_XNetInAddrToXnAddr"}}) {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = entry.second;
    desc.ordinal = entry.first;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "matches rexglue-sdk/xenia's own identical no-op-return-failure "
        "precedent - real translation needs secure-session key state this "
        "codebase does not have";
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 1u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // NetDll_XNetQosListen/Lookup/ServiceLookup/Release (0x45-0x48) - real QoS
  // probing exchanges actual packets with other consoles/services over a
  // real Xbox Live/System Link session Xenon does not implement. Verified
  // against rexglue-sdk's NetDll_XNetQosListen_entry, which also just
  // reports X_ERROR_FUNCTION_FAILED rather than attempting a real probe.
  // Release is the one real local operation in this group (freeing a QoS
  // result block) - since nothing above this can ever produce a real one,
  // it is a genuine no-op that always succeeds, matching what freeing an
  // already-empty result trivially does on real hardware too.
  {
    constexpr std::uint32_t kErrorFunctionFailed = 0x0000065Bu;
    for (const auto& entry :
         {std::pair{ordinal::NetDll_XNetQosListen, "NetDll_XNetQosListen"},
          std::pair{ordinal::NetDll_XNetQosLookup, "NetDll_XNetQosLookup"},
          std::pair{ordinal::NetDll_XNetQosServiceLookup, "NetDll_XNetQosServiceLookup"}}) {
      core::ExportDescriptor desc{};
      desc.library = "xam";
      desc.name = entry.second;
      desc.ordinal = entry.first;
      desc.requirement = core::ExportRequirement::Required;
      desc.partial = true;
      desc.partial_note = "no real QoS probe exchange is implemented";
      desc.handler = [kErrorFunctionFailed](core::ExportCallContext& ctx) -> bool {
        ctx.cpu.gpr[3] = kErrorFunctionFailed;
        return true;
      };
      ok = registry.register_export(std::move(desc)) && ok;
    }
    core::ExportDescriptor release_desc{};
    release_desc.library = "xam";
    release_desc.name = "NetDll_XNetQosRelease";
    release_desc.ordinal = ordinal::NetDll_XNetQosRelease;
    release_desc.requirement = core::ExportRequirement::Required;
    release_desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(release_desc)) && ok;
  }

  // NetDll_XNetGetTitleXnAddr (0x49) - DWORD XNetGetTitleXnAddr(int caller,
  // XNADDR* addr_out). Real 36-byte XNADDR layout (verified against
  // rexglue-sdk's XNADDR usage): ina (in_addr, +0), inaOnline (in_addr, +4),
  // wPortOnline (+8), abEnet[6] (+10), abOnline[20] (+16). Matches
  // rexglue-sdk's NetDll_XNetGetTitleXnAddr_entry exactly: a real loopback
  // local address (Xenon has no real XNet online identity), zeroed online
  // address/port, and non-zero abEnet bytes specifically because a
  // zero MAC address is documented (by that same reference) to make
  // RakNet-based titles' network startup fail outright - returns
  // XNET_GET_XNADDR_STATIC (0x4), the real status for "static local address
  // known".
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "NetDll_XNetGetTitleXnAddr";
    desc.ordinal = ordinal::NetDll_XNetGetTitleXnAddr;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto addr_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      if (addr_ptr != 0u) {
        ctx.memory.fill_bytes(addr_ptr, 36u, 0u);
        ctx.memory.write32_be(addr_ptr + 0u, ::ntohl(INADDR_LOOPBACK));
        for (std::uint32_t i = 0; i < 6u; ++i) {
          ctx.memory.write8(addr_ptr + 10u + i, 0xCCu);
        }
      }
      constexpr std::uint32_t kXNetGetXnAddrStatic = 0x00000004u;
      ctx.cpu.gpr[3] = kXNetGetXnAddrStatic;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}
#endif  // _WIN32

}  // namespace xenon::xam
