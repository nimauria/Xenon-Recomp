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

  // XamNotifyCreateListener (0x0210)
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

  // XamNotifyGetNext (0x0211)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamNotifyGetNext";
    desc.ordinal = ordinal::XamNotifyGetNext;
    desc.requirement = core::ExportRequirement::Stubbed;
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

  // XamNotifyPositionUI (0x0212) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamNotifyPositionUI";
    desc.ordinal = ordinal::XamNotifyPositionUI;
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
