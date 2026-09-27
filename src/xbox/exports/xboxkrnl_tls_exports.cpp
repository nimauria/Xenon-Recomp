#include "xenon/xbox/xboxkrnl_tls_exports.hpp"

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;

}  // namespace

bool ke_tls_alloc_export(xenon::kernel::KernelProcess& process, ExportCallContext& context) {
  const auto slot = process.allocate_tls_slot();
  if (slot != xenon::kernel::KernelProcess::kTlsOutOfIndexes) {
    if (auto* thread = process.thread_manager().get_thread_ptr(context.thread_id)) {
      static_cast<void>(thread->set_tls(slot, 0u));
    }
  }
  context.cpu.gpr[3] = static_cast<std::uint64_t>(slot);
  return true;
}

bool ke_tls_free_export(xenon::kernel::KernelProcess& process, ExportCallContext& context) {
  const auto slot = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  if (slot == xenon::kernel::KernelProcess::kTlsOutOfIndexes) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  process.free_tls_slot(slot);
  context.cpu.gpr[3] = 1u;
  return true;
}

bool ke_tls_get_value_export(xenon::kernel::KernelProcess& process, ExportCallContext& context) {
  const auto slot = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  std::uint64_t value = 0u;
  if (auto* thread = process.thread_manager().get_thread_ptr(context.thread_id)) {
    if (const auto stored = thread->get_tls(slot)) {
      value = *stored;
    }
  }
  context.cpu.gpr[3] = value;
  return true;
}

bool ke_tls_set_value_export(xenon::kernel::KernelProcess& process, ExportCallContext& context) {
  const auto slot = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto value = context.cpu.gpr[4];
  bool ok = false;
  if (auto* thread = process.thread_manager().get_thread_ptr(context.thread_id)) {
    ok = thread->set_tls(slot, value);
  }
  context.cpu.gpr[3] = ok ? 1u : 0u;
  return true;
}

bool register_xboxkrnl_tls_exports(xenon::core::ExportRegistry& registry,
                                   xenon::kernel::KernelProcess& process) {
  struct Binding {
    std::uint32_t ordinal;
    const char* name;
    bool (*handler)(xenon::kernel::KernelProcess&, ExportCallContext&);
  };
  static constexpr Binding kBindings[] = {
      {0x152u, "KeTlsAlloc", &ke_tls_alloc_export},
      {0x153u, "KeTlsFree", &ke_tls_free_export},
      {0x154u, "KeTlsGetValue", &ke_tls_get_value_export},
      {0x155u, "KeTlsSetValue", &ke_tls_set_value_export},
  };
  for (const auto& binding : kBindings) {
    xenon::core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = binding.name;
    descriptor.ordinal = binding.ordinal;
    descriptor.requirement = xenon::core::ExportRequirement::Required;
    descriptor.handler = [&process, fn = binding.handler](ExportCallContext& ctx) {
      return fn(process, ctx);
    };
    if (!registry.register_export(std::move(descriptor))) return false;
  }
  return true;
}

}  // namespace xenon::xbox
