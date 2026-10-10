#pragma once

namespace xenon::core {
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// Kernel pool exports. All need the process's KernelPool, so they are bound
// lazily in XenonSession::init_exports() like the other KernelProcess exports.
// Ordinals are from the xenia xboxkrnl export table.

// ExAllocatePool (0x09): r3 = size -> r3 = guest pointer or 0 (tag 'None').
[[nodiscard]] bool ex_allocate_pool_export(xenon::kernel::KernelProcess& process,
                                           xenon::core::ExportCallContext& context);
// ExAllocatePoolWithTag (0x0A): r3 = size, r4 = tag -> r3 = pointer or 0.
[[nodiscard]] bool ex_allocate_pool_with_tag_export(xenon::kernel::KernelProcess& process,
                                                    xenon::core::ExportCallContext& context);
// ExAllocatePoolTypeWithTag (0x0B): r3 = size, r4 = tag, r5 = pool type (paged
// and non-paged are the same memory on this platform) -> r3 = pointer or 0.
[[nodiscard]] bool ex_allocate_pool_type_with_tag_export(xenon::kernel::KernelProcess& process,
                                                         xenon::core::ExportCallContext& context);
// ExFreePool (0x0F): r3 = pointer (NULL is a no-op) -> void.
[[nodiscard]] bool ex_free_pool_export(xenon::kernel::KernelProcess& process,
                                       xenon::core::ExportCallContext& context);
// ExQueryPoolBlockSize (0x13): r3 = pointer, r4 = optional PBOOLEAN
// QuotaCharged -> r3 = usable bytes in the block (0 for an unknown pointer).
[[nodiscard]] bool ex_query_pool_block_size_export(xenon::kernel::KernelProcess& process,
                                                   xenon::core::ExportCallContext& context);

}  // namespace xenon::xbox
