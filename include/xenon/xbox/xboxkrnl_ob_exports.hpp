#pragma once

namespace xenon::core {
struct ExportCallContext;
}

namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// ObReferenceObjectByHandle (ordinal 0x110 / 272).
// Guest ABI: r3 = handle, r4 = object type descriptor guest pointer (0 = no
// type check), r5 = guest pointer to receive the object's guest-visible
// address -> r3 = NTSTATUS.
//
// Real AC6 repro: reached during startup (right after a guest ExCreateThread)
// with no case registered at all, which XenonSession surfaces as an
// unresolved-import trap and kills guest execution outright.
[[nodiscard]] bool ob_reference_object_by_handle_export(
    kernel::KernelProcess& process, core::ExportCallContext& context);

// ObDereferenceObject (ordinal 0x105 / 261).
// Guest ABI: r3 = the guest-visible object address a prior
// ObReferenceObjectByHandle call returned -> r3 = NTSTATUS (always Success on
// real hardware for a valid pointer). Xenon's KernelObject lifetime is
// std::shared_ptr-owned via kernel::HandleTable, not the manual refcount real
// Xbox 360 objects use, and ObReferenceObjectByHandle above never took an
// extra manual reference for its return value - only kernel::HandleTable's
// own ownership keeps the object alive - so there is nothing to release
// here. This is an intentional, narrower model of a real primitive, not a
// silent no-op standing in for unfinished work: it is unconditionally
// correct as long as nothing manually races closing the same object's
// handle against a still-in-flight dereferenced pointer, which no currently
// implemented export does.
[[nodiscard]] bool ob_dereference_object_export(kernel::KernelProcess& process,
                                                core::ExportCallContext& context);

}  // namespace xenon::xbox
