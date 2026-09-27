#include "xenon/xbox/xboxkrnl_process_exports.hpp"

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/process.hpp"

namespace xenon::xbox {

using xenon::core::ExportCallContext;

// KeGetCurrentProcessType (ordinal 0x66)
// Guest ABI: (no parameters) -> r3 = process type
// (X_PROCTYPE_IDLE=0/X_PROCTYPE_USER=1/X_PROCTYPE_SYSTEM=2).
bool ke_get_current_process_type_export(kernel::KernelProcess& process,
                                        ExportCallContext& context) {
  context.cpu.gpr[3] = process.process_type();
  return true;
}

// KeSetCurrentProcessType (ordinal 0x9A)
// Guest ABI: r3 = process type (X_PROCTYPE_IDLE=0/X_PROCTYPE_USER=1/
// X_PROCTYPE_SYSTEM=2) -> void (no meaningful return; r3 left untouched).
// Real hardware asserts the value is <= 2; an out-of-range value is simply
// stored as-is here (matching xenia's own behavior outside debug-assert
// builds) rather than silently clamped or rejected, since a subsequent Get
// must still observe exactly whatever the title set, not a Xenon-invented
// correction.
bool ke_set_current_process_type_export(kernel::KernelProcess& process,
                                        ExportCallContext& context) {
  process.set_process_type(static_cast<std::uint32_t>(context.cpu.gpr[3]));
  return true;
}

namespace {
struct ProcessExportSpec {
  std::uint32_t ordinal;
  const char* name;
  bool (*handler)(kernel::KernelProcess&, ExportCallContext&);
};

// Ordinals verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc), not guessed - see xboxkrnl_process_exports.hpp.
const ProcessExportSpec kProcessExports[] = {
    {0x066u, "KeGetCurrentProcessType", &ke_get_current_process_type_export},
    {0x09Au, "KeSetCurrentProcessType", &ke_set_current_process_type_export},
};
}  // namespace

bool register_xboxkrnl_process_exports(core::ExportRegistry& registry,
                                       kernel::KernelProcess& process) {
  for (const auto& spec : kProcessExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.handler = [&process, fn = spec.handler](ExportCallContext& ctx) {
      return fn(process, ctx);
    };

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
