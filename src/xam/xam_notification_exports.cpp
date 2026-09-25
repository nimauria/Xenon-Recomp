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

  // XamNotifyCreateListener (0x028A)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamNotifyCreateListener";
    desc.ordinal = ordinal::XamNotifyCreateListener;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [&notification_manager](core::ExportCallContext& ctx) -> bool {
      const auto out_handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[1]);

      const auto listener_id = notification_manager.create_listener();
      
      if (out_handle_ptr != 0) {
        write_u32_be(ctx.memory, out_handle_ptr, listener_id);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XNotifyGetNext (0x028B) - real xam.xex identity has no "Xam" prefix;
  // see xam_exports.hpp for the rename rationale.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XNotifyGetNext";
    desc.ordinal = ordinal::XNotifyGetNext;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "reports whether a notification is pending but never writes the "
        "notification payload/id to guest memory - a caller that checks "
        "for a specific notification type cannot observe it";
    desc.handler = [&notification_manager](core::ExportCallContext& ctx) -> bool {
      auto notification = notification_manager.get_next_notification();
      
      if (!notification.has_value()) {
        ctx.cpu.gpr[3] = result::Empty;
        return true;
      }

      // Could write notification data to guest memory here
      // For now, just return success
      ctx.cpu.gpr[3] = result::Success;
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
