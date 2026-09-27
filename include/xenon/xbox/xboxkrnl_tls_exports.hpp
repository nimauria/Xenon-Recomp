#pragma once

namespace xenon::core {
struct ExportCallContext;
class ExportRegistry;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// KeTlsAlloc/KeTlsFree/KeTlsGetValue/KeTlsSetValue (ordinals 0x152-0x155 /
// 338-341) - verified against the xenia-project/xenia xboxkrnl export table
// and its real implementation (xboxkrnl_threading.cc):
//   - KeTlsAlloc(): allocates the next free process-wide slot index (real
//     Win32 TlsAlloc semantics - allocation state is process-wide, values are
//     per-thread), resets the CALLING thread's value for that slot to 0 (not
//     every thread's - matches xenia exactly), returns
//     KernelProcess::kTlsOutOfIndexes (UINT32_MAX) if none are free.
//   - KeTlsFree(tls_index): marks the slot free again. Returns 0 (failure)
//     for kTlsOutOfIndexes, 1 otherwise - the real xboxkrnl has no other
//     error branch.
//   - KeTlsGetValue(tls_index): returns the calling thread's stored value,
//     or 0 for an out-of-range slot - the real xboxkrnl never faults here.
//   - KeTlsSetValue(tls_index, value): stores into the calling thread's
//     slot; returns 1 on success, 0 if the slot is out of range.
[[nodiscard]] bool ke_tls_alloc_export(xenon::kernel::KernelProcess& process,
                                       xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_tls_free_export(xenon::kernel::KernelProcess& process,
                                      xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_tls_get_value_export(xenon::kernel::KernelProcess& process,
                                           xenon::core::ExportCallContext& context);
[[nodiscard]] bool ke_tls_set_value_export(xenon::kernel::KernelProcess& process,
                                           xenon::core::ExportCallContext& context);

// Convenience registrar for callers (tests/tools) that already have a
// constructed KernelProcess. XenonSession::init_exports() instead registers
// lambdas that lazily dereference kernel_process_ at call time - see
// xboxkrnl_sync_exports.hpp's identical note for why.
[[nodiscard]] bool register_xboxkrnl_tls_exports(xenon::core::ExportRegistry& registry,
                                                 xenon::kernel::KernelProcess& process);

}  // namespace xenon::xbox
