#include "xenon/xam/xam_system_exports.hpp"

#include <cstring>
#include <mutex>

#include "xenon/core/session.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

}  // namespace

bool register_system_exports(core::ExportRegistry& registry, core::XenonSession& session) {
  bool ok = true;

  // XamGetSystemVersion (0x0282) - no arguments, returns a packed
  // dashboard/kernel build-version DWORD in r3.
  //
  // Real hardware returns the running kernel's actual build number, but
  // guest code only ever uses this value for a "does this feature exist
  // yet" comparison, not for anything display-visible. Xenia (and its
  // rexglue-sdk fork, src/kernel/xam/xam_info.cpp's
  // XamGetSystemVersion_entry) deliberately return 0 here rather than
  // inventing a plausible-looking high build number: guest code that
  // branches on this value treats a higher number as "newer kernel, assume
  // feature X is present", so a made-up modern-looking value is more likely
  // to route execution into a completely unimplemented codepath than an old
  // one is. Returning 0 always selects the oldest/most-conservative branch,
  // which is exactly the behavior a real kernel's *lowest* possible build
  // number would produce - so this is a genuine compatibility-motivated
  // choice, not a placeholder.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamGetSystemVersion";
    desc.ordinal = ordinal::XamGetSystemVersion;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XGetGameRegion (0x03CC) - no arguments, returns the console's configured
  // game-region flags (XC_GAME_REGION_*) in r3.
  //
  // Real hardware reports the region(s) the running kernel/console are
  // configured to allow; xenia-project/xenia's xeXGetGameRegion() (verified
  // reference) returns 0xFFFF unconditionally - every region bit set, i.e.
  // "region-free" - rather than modeling a specific retail console's region
  // lock. Xenon matches that: AC6 (and any other title) calls this purely to
  // decide whether to show a region-mismatch warning, and a real dev/debug
  // kit (which is exactly the kind of hardware most emulation targets, and
  // the only kind that never needs to reject a disc) also reports every
  // region allowed.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XGetGameRegion";
    desc.ordinal = ordinal::XGetGameRegion;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0xFFFFu;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XGetAVPack (0x03CB) - no arguments, returns the AV cable type connected
  // to the console in r3.
  //
  // Verified against rexglue-sdk's XGetAVPack_entry (itself crediting
  // xenia): titles use this as a PAL/NTSC-capability gate - 6 (VGA) is the
  // one value every available reference reports unconditionally, and titles
  // that branch on "not one of {3,4,6,8}" treat an unrecognized value as a
  // hard error. Matching that exact real value, not inventing a different
  // plausible-looking one, is what keeps this gate passing.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XGetAVPack";
    desc.ordinal = ordinal::XGetAVPack;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 6u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XGetVideoMode (0x03D1) - X_VIDEO_MODE* out, no return value.
  //
  // Verified against rexglue-sdk's XGetVideoMode_entry, which literally
  // delegates to xboxkrnl's VdQueryVideoMode - the same real export Xenon
  // already implements (xboxkrnl_video_exports.cpp's
  // vd_query_video_mode_export), writing the identical 48-byte big-endian
  // X_VIDEO_MODE struct (1280x720@60Hz, widescreen, hi-def, NTSC-M). Rather
  // than depend on xboxkrnl's registry entry (a different export
  // registration, with no guaranteed call-through path between the two
  // modules' handlers), this writes the same verified struct directly so
  // the two exports can never disagree.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XGetVideoMode";
    desc.ordinal = ordinal::XGetVideoMode;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto video_mode = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
      if (video_mode != 0u) {
        write_u32_be(ctx.memory, video_mode + 0x00u, 1280u);
        write_u32_be(ctx.memory, video_mode + 0x04u, 720u);
        write_u32_be(ctx.memory, video_mode + 0x08u, 0u);
        write_u32_be(ctx.memory, video_mode + 0x0Cu, 1u);
        write_u32_be(ctx.memory, video_mode + 0x10u, 1u);
        float refresh_rate = 60.0f;
        std::uint32_t refresh_rate_bits{};
        std::memcpy(&refresh_rate_bits, &refresh_rate, sizeof(refresh_rate_bits));
        write_u32_be(ctx.memory, video_mode + 0x14u, refresh_rate_bits);
        write_u32_be(ctx.memory, video_mode + 0x18u, 1u);
        write_u32_be(ctx.memory, video_mode + 0x1Cu, 0x4Au);
        write_u32_be(ctx.memory, video_mode + 0x20u, 0x01u);
        write_u32_be(ctx.memory, video_mode + 0x24u, 0u);
        write_u32_be(ctx.memory, video_mode + 0x28u, 0u);
        write_u32_be(ctx.memory, video_mode + 0x2Cu, 0u);
      }
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamAlloc/XamFree (0x01EA/0x01EC) - the XAM-owned heap, kept separate from
  // the title's own CRT heap on real hardware. Backed by the process guest
  // heap under its own XAM heap identity, so small XAM blocks are
  // sub-allocated from shared segments (a dedicated 64 KiB virtual region per
  // call exhausted the guest address space) and a later XamFree releases
  // exactly the block it was given. Blocks are always returned zeroed.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamAlloc";
    desc.ordinal = ordinal::XamAlloc;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session](core::ExportCallContext& ctx) -> bool {
      const auto size = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto out_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      auto* process = session.kernel_process();
      if (process == nullptr || out_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      const auto address = process->guest_heap().allocate(
          kernel::GuestHeapManager::kXamHeapHandle, kernel::GuestHeapManager::kHeapZeroMemory,
          size);
      if (address == 0u) {
        ctx.cpu.gpr[3] = result::FunctionFailed;
        return true;
      }
      write_u32_be(ctx.memory, out_ptr, address);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamFree";
    desc.ordinal = ordinal::XamFree;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session](core::ExportCallContext& ctx) -> bool {
      const auto address = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      auto* process = session.kernel_process();
      if (process == nullptr || address == 0u) {
        ctx.cpu.gpr[3] = result::Success;
        return true;
      }
      static_cast<void>(
          process->guest_heap().free(kernel::GuestHeapManager::kXamHeapHandle, 0u, address));
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamGetExecutionId (0x0280) - X_EXECUTION_ID** out -> HRESULT.
  //
  // Real hardware returns a pointer directly into the loaded XEX's own
  // header region (the XEX_HEADER_EXECUTION_INFO optional header). Xenon's
  // loader parses that same header into XexImage::execution_info rather than
  // keeping a stable guest pointer to the raw header bytes, so this writes
  // the already-parsed fields into a small lazily-allocated guest buffer, in
  // the real 24-byte big-endian X_EXECUTION_ID layout (media_id, version,
  // base_version, title_id, platform, executable_table, disc_number,
  // disc_count, savegame_id), and returns that buffer's address - the same
  // guest-observable fields a real pointer would expose, sourced from
  // Xenon's own real parse of the real header rather than re-deriving a raw
  // pointer into header bytes this architecture does not keep mapped.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamGetExecutionId";
    desc.ordinal = ordinal::XamGetExecutionId;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "returns a Xenon-owned scratch buffer populated from the parsed XEX "
        "execution-info header, not a real pointer into the XEX's own header "
        "region - the field values are real, the pointer's target is not";
    desc.handler = [&session](core::ExportCallContext& ctx) -> bool {
      const auto out_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
      const auto* loaded = session.loaded_xex();
      auto* process = session.kernel_process();
      if (out_ptr == 0u || loaded == nullptr || process == nullptr) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      static std::mutex cache_mutex;
      static std::uint32_t cached_address = 0u;
      std::uint32_t address{};
      {
        std::scoped_lock lock(cache_mutex);
        if (cached_address == 0u) {
          std::uint32_t allocated = 0u;
          if (!process->memory().allocate_virtual(allocated, 24u, memory::kReadWrite,
                                                   /*top_down=*/false, /*zero_initialize=*/true)) {
            ctx.cpu.gpr[3] = result::FunctionFailed;
            return true;
          }
          const auto& info = loaded->image.execution_info;
          write_u32_be(ctx.memory, allocated + 0x00u, info.media_id);
          write_u32_be(ctx.memory, allocated + 0x04u, info.version.value);
          write_u32_be(ctx.memory, allocated + 0x08u, info.base_version.value);
          write_u32_be(ctx.memory, allocated + 0x0Cu, info.title_id);
          ctx.memory.write8(allocated + 0x10u, info.platform);
          ctx.memory.write8(allocated + 0x11u, info.executable_table);
          ctx.memory.write8(allocated + 0x12u, info.disc_number);
          ctx.memory.write8(allocated + 0x13u, info.disc_count);
          write_u32_be(ctx.memory, allocated + 0x14u, info.savegame_id);
          cached_address = allocated;
        }
        address = cached_address;
      }
      write_u32_be(ctx.memory, out_ptr, address);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamLoaderLaunchTitle/XamLoaderTerminateTitle (0x01A4/0x01A9) - both end
  // the running title on real hardware (LaunchTitle additionally stages a
  // new title to launch next; Xenon's single-title-per-process session has
  // nothing to stage that into, matching this being a Required-but-partial
  // export rather than a full implementation). Mapped onto
  // XenonSession::stop()'s existing cooperative-stop contract - the same
  // mechanism KeBugCheckEx already uses for "this title cannot continue" -
  // rather than a no-op that would leave the guest spinning in a title that
  // asked to exit.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamLoaderTerminateTitle";
    desc.ordinal = ordinal::XamLoaderTerminateTitle;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session](core::ExportCallContext&) -> bool {
      static_cast<void>(session.stop());
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamLoaderLaunchTitle";
    desc.ordinal = ordinal::XamLoaderLaunchTitle;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "terminates the running title like XamLoaderTerminateTitle but does "
        "not stage or launch a new one - Xenon's session owns exactly one "
        "title per process";
    desc.handler = [&session](core::ExportCallContext&) -> bool {
      static_cast<void>(session.stop());
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
