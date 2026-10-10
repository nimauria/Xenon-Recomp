#pragma once

namespace xenon::core {
class ExportRegistry;
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// NtAllocateVirtualMemory needs kernel::KernelProcess (for its
// KernelMemory/AddressSpace), which is not yet constructed at
// XenonSession::init_exports() time - exposed individually so the caller can
// register a lambda that dereferences its own lazily-bound KernelProcess
// pointer at CALL time, exactly like xboxkrnl_sync_exports.hpp's Nt* handlers
// (see XenonSession::init_exports()'s kSyncBindings).
[[nodiscard]] bool nt_allocate_virtual_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

// MmAllocatePhysicalMemoryEx (ordinal 0xBA). Same KernelProcess-at-call-time
// need as NtAllocateVirtualMemory above.
//
// Real AC6 repro: reached during startup with no case registered at all.
[[nodiscard]] bool mm_allocate_physical_memory_ex_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

[[nodiscard]] bool mm_free_physical_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_get_physical_address_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_query_address_protect_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_query_allocation_size_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_query_statistics_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_set_address_protect_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

// Virtual-memory and kernel-memory exports (all need the KernelProcess's
// AddressSpace; registered lazily-bound in XenonSession::init_exports()).
[[nodiscard]] bool nt_free_virtual_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_query_virtual_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_protect_virtual_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_allocate_physical_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_map_io_space_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_is_address_valid_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_create_kernel_stack_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool mm_delete_kernel_stack_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_get_image_page_table_entry_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_allocate_encrypted_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool nt_free_encrypted_memory_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

// Registers the xboxkrnl guest memory-management exports this pass adds that
// need no KernelProcess: KeFlushUserModeTb. Ordinal verified against the
// xenia-project/xenia xboxkrnl export table (xboxkrnl_table.inc) rather than
// guessed, per this project's own hard-learned lesson about
// plausible-but-wrong ordinals (RtlImageXexHeaderField / 0x12B).
//
// Safe to call repeatedly on the same registry. No session dependency: the
// handler here touches no session/process state at all - see
// xboxkrnl_memory_exports.cpp for why a no-op is the behaviorally-correct
// implementation on Xenon's host-VM-backed memory model, not a placeholder.
[[nodiscard]] bool register_xboxkrnl_memory_exports(
    xenon::core::ExportRegistry& registry);

}  // namespace xenon::xbox
