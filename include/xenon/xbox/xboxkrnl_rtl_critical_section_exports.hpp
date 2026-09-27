#pragma once

namespace xenon::core {
class ExportRegistry;
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// Rtl*CriticalSection exports - a real X_RTL_CRITICAL_SECTION embedded
// directly in guest memory (never a Handle), matching the real Xbox 360
// structure layout verified against xenia-project/xenia's
// X_RTL_CRITICAL_SECTION (src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc):
//   offset 0x00: X_DISPATCH_HEADER header (16 bytes - see
//                xex_dispatcher_header.hpp; type=1/EventSynchronization,
//                offset 0x01 repurposed as spin-count/256)
//   offset 0x10: int32_t  lock_count      (-1 = unlocked)
//   offset 0x14: int32_t  recursion_count (0 = unlocked, N = held N times by
//                the owning thread)
//   offset 0x18: uint32_t owning_thread   (0 = unlocked, otherwise the
//                owning guest thread's identity - see
//                xboxkrnl_rtl_critical_section_exports.cpp for exactly what
//                Xenon stores here)
// Total size 28 (0x1C) bytes, matching the real, documented Xbox 360
// structure exactly (NOT the larger 32-byte desktop Win32 CRITICAL_SECTION -
// this is a title-visible ABI a game may have pre-initialized in its own
// embedded data, so the size and field offsets must match exactly).
//
// Ordinals verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc): RtlInitializeCriticalSection 0x12E,
// RtlInitializeCriticalSectionAndSpinCount 0x12F, RtlEnterCriticalSection
// 0x125, RtlTryEnterCriticalSection 0x141, RtlLeaveCriticalSection 0x130.

[[nodiscard]] bool rtl_initialize_critical_section_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool rtl_initialize_critical_section_and_spin_count_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool rtl_enter_critical_section_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool rtl_try_enter_critical_section_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool rtl_leave_critical_section_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

// Convenience registrar for callers (tests/tools) that already have a
// constructed KernelProcess. XenonSession::init_exports() instead registers
// lambdas that lazily dereference kernel_process_ at call time - see
// xboxkrnl_sync_exports.hpp's identical note for why.
[[nodiscard]] bool register_xboxkrnl_rtl_critical_section_exports(
    xenon::core::ExportRegistry& registry, xenon::kernel::KernelProcess& process);

}  // namespace xenon::xbox
