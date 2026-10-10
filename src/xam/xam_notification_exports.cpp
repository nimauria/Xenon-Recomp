#include "xenon/core/export_registry.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

}  // namespace

bool register_notification_exports(core::ExportRegistry& registry,
                                   NotificationManager& notification_manager) {
  bool ok = true;

  // XamNotifyCreateListener (0x028A) - real ABI is a dword_result_t, verified
  // against xenia's XamNotifyCreateListener_entry(qword_t mask,
  // dword_t max_version): mask=r3 (PPC64 passes a 64-bit qword in one GPR,
  // not a register pair), max_version=r4, and the HANDLE ITSELF is the
  // return value in r3 - there is no separate out-pointer parameter at all.
  // The previous stub read a nonexistent "out_handle_ptr" from gpr[1] (the
  // guest stack pointer register - never a valid argument slot) and always
  // returned result::Success (0) in r3, which a real caller reads directly
  // as "listener handle 0" - i.e. an invalid handle - even though a listener
  // was actually created internally. Returning the real id directly in r3
  // is the fix; see NotificationManager's own "stubbed" listener-management
  // comment for the separate, already-disclosed limitation that its queue
  // is global rather than partitioned per listener/mask.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamNotifyCreateListener";
    desc.ordinal = ordinal::XamNotifyCreateListener;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "the returned listener handle does not back a per-listener filtered "
        "queue - NotificationManager has a single global FIFO shared by "
        "every listener, so the mask parameter is accepted but not applied";
    desc.handler = [&notification_manager](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = notification_manager.create_listener();
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XNotifyGetNext (0x028B) - real xam.xex identity has no "Xam" prefix;
  // see xam_exports.hpp for the rename rationale. Real ABI verified against
  // xenia's XNotifyGetNext_entry(handle, match_id, id_ptr, param_ptr):
  // handle=r3, match_id=r4, id_ptr=r5, param_ptr=r6. The previous stub read
  // none of these and never wrote id_ptr/param_ptr at all - this now writes
  // the real dequeued notification's type/param, fixing exactly the gap its
  // own partial_note described. match_id-based filtering (dequeue a specific
  // pending type out of order) is NOT honored: NotificationManager's queue
  // is FIFO-only with no by-type lookup, a separate, already-disclosed
  // limitation (see NotificationManager's class comment) rather than
  // something this export itself could fix.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XNotifyGetNext";
    desc.ordinal = ordinal::XNotifyGetNext;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "match_id-based filtering is not honored (always dequeues the "
        "front of the single global FIFO) - NotificationManager has no "
        "by-type lookup to dequeue a specific pending notification out of "
        "order";
    desc.handler = [&notification_manager](core::ExportCallContext& ctx) -> bool {
      const auto id_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto param_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);

      if (param_ptr != 0u) write_u32_be(ctx.memory, param_ptr, 0u);
      if (id_ptr != 0u) write_u32_be(ctx.memory, id_ptr, 0u);

      auto notification = notification_manager.get_next_notification();
      if (!notification.has_value()) {
        ctx.cpu.gpr[3] = 0u;  // FALSE: no notification dequeued.
        return true;
      }

      if (id_ptr != 0u) {
        write_u32_be(ctx.memory, id_ptr, static_cast<std::uint32_t>(notification->type));
      }
      if (param_ptr != 0u) {
        write_u32_be(ctx.memory, param_ptr, static_cast<std::uint32_t>(notification->param));
      }

      ctx.cpu.gpr[3] = 1u;  // TRUE: a notification was dequeued.
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XNotifyPositionUI (0x028C) - Stubbed; real xam.xex identity has no
  // "Xam" prefix, see xam_exports.hpp.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XNotifyPositionUI";
    desc.ordinal = ordinal::XNotifyPositionUI;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      // No UI to position
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
