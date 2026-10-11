#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/xam/notification_manager.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {

// XNotify listeners, as in xenia's xam_notify.cc (see NotificationManager):
//   XamNotifyCreateListener(qword mask, max_version) -> handle
//   XamNotifyCreateListenerInternal(qword mask, is_system, max_version) -> handle
//   XNotifyGetNext(handle, match_id, id_ptr, param_ptr) -> BOOL
// The handle is a waitable kernel object closed with NtClose; 0 is returned
// when no handle could be made. Earlier Xenon returned a counter as the
// "handle", kept one queue for every listener, ignored the mask and
// match_id, used invented notification ids, and sent no startup
// notifications.
bool register_notification_exports(core::ExportRegistry& registry,
                                   NotificationManager& notifications,
                                   core::XenonSession& session) {
  bool ok = true;

  const auto create = [&notifications, &session](core::ExportCallContext& ctx,
                                                 std::uint64_t mask, std::uint32_t max_version) {
    auto* process = session.kernel_process();
    kernel::Handle handle{};
    if (process == nullptr ||
        process->handle_table().insert(notifications.create_listener(mask, max_version),
                                       0xFFFFFFFFu, kernel::HandleFlags::None,
                                       handle) != kernel::KernelIoCode::Success) {
      handle = 0u;
    }
    ctx.cpu.gpr[3] = handle;
    return true;
  };

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamNotifyCreateListener";
    desc.ordinal = ordinal::XamNotifyCreateListener;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [create](core::ExportCallContext& ctx) -> bool {
      return create(ctx, ctx.cpu.gpr[3], static_cast<std::uint32_t>(ctx.cpu.gpr[4]));
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamNotifyCreateListenerInternal";
    desc.ordinal = ordinal::XamNotifyCreateListenerInternal;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [create](core::ExportCallContext& ctx) -> bool {
      return create(ctx, ctx.cpu.gpr[3], static_cast<std::uint32_t>(ctx.cpu.gpr[5]));
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XNotifyGetNext: *param_ptr is zeroed first, and with no id_ptr nothing
  // is dequeued (FALSE), as in xenia; a null param_ptr is allowed (Ghost
  // Recon Advanced Warfighter 2 passes one).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XNotifyGetNext";
    desc.ordinal = ordinal::XNotifyGetNext;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&notifications, &session](core::ExportCallContext& ctx) -> bool {
      const auto handle = static_cast<kernel::Handle>(ctx.cpu.gpr[3]);
      const auto match_id = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto id_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto param_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);
      if (param_ptr != 0u) ctx.memory.write32_be(param_ptr, 0u);
      ctx.cpu.gpr[3] = 0u;
      if (id_ptr == 0u) return true;
      ctx.memory.write32_be(id_ptr, 0u);
      auto* process = session.kernel_process();
      kernel::HandleView view{};
      if (process == nullptr ||
          process->handle_table().lookup(handle, view) != kernel::KernelIoCode::Success) {
        return true;
      }
      std::uint32_t id = 0;
      std::uint32_t param = 0;
      if (!notifications.dequeue(view.object.get(), match_id, id, param)) return true;
      ctx.memory.write32_be(id_ptr, id);
      if (param_ptr != 0u) ctx.memory.write32_be(param_ptr, param);
      ctx.cpu.gpr[3] = 1u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XNotifyPositionUI(position) and XNotifyDelayUI(delay_ms) place and
  // delay the system notification toasts; Xenon draws no system UI, so
  // there is nothing to move or delay (xenia ignores both too).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XNotifyPositionUI";
    desc.ordinal = ordinal::XNotifyPositionUI;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XNotifyDelayUI";
    desc.ordinal = ordinal::XNotifyDelayUI;
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
