#pragma once

namespace xenon::core {
struct ExportCallContext;
class ExportRegistry;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// Vd* Xenos GPU control-plane exports (xboxkrnl.exe) - verified against
// xenia-project/xenia's xboxkrnl_video.cc/xbox.h (the project's documented
// reference; see CLAUDE.md's Research rule). These are the exports a real
// Xbox 360 D3D9-equivalent guest runtime calls during boot and every frame to
// hand its GPU command ring buffer and swap-chain front buffer over to the
// kernel/GPU. They write into xenon::kernel::KernelProcess's
// GpuRingBufferState/GpuInterruptCallbackState/GpuFrontBufferState (see
// process.hpp) - a separate, already-landed workstream's XenonSession GPU
// pump thread is the only reader of that state; this file is purely the
// writer side of that contract, matching xboxkrnl_tls_exports.hpp's file
// layout and calling convention.
//
// Real ordinals verified against xenia's xboxkrnl_table.inc:
//   0x1B1 VdCallGraphicsNotificationRoutines
//   0x1B6 VdEnableRingBufferRPtrWriteBack
//   0x1BA VdGetCurrentDisplayInformation
//   0x1BD VdGetSystemCommandBuffer
//   0x1C2 VdInitializeEngines
//   0x1C3 VdInitializeRingBuffer
//   0x1C6 VdIsHSIOTrainingSucceeded
//   0x1CA VdQueryVideoMode
//   0x1D5 VdSetGraphicsInterruptCallback
//   0x1D9 VdSetSystemCommandBufferGpuIdentifierAddress
//   0x1DC VdShutdownEngines
//   0x25B VdSwap
//
// Argument convention: matches every other export in this codebase - integer/
// pointer arguments 0-7 are read from context.cpu.gpr[3..10]; arguments 8+
// spill to the guest stack at [r1+0x54], [r1+0x5C], ... (8 bytes/slot, value
// in the first 4 bytes, big-endian) - see docs/xbox/IMPORT_DISPATCH.md.
// VdSwap is the only export here with more than 8 arguments; it reads its
// 9th/10th arguments from the stack directly (see xboxkrnl_video_exports.cpp's
// read_stack_argument_u32() helper) rather than via xenon::core::CallBridge,
// since CallBridge's translation unit lives in the xenon_core CMake target,
// which links this file's xenon_xbox_kernel_io target, not the other way
// around.

[[nodiscard]] bool vd_call_graphics_notification_routines_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_enable_ring_buffer_rptr_write_back_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_get_current_display_information_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_get_system_command_buffer_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_initialize_engines_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_initialize_ring_buffer_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_is_hsio_training_succeeded_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_query_video_mode_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_set_graphics_interrupt_callback_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_set_system_command_buffer_gpu_identifier_address_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_shutdown_engines_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);
[[nodiscard]] bool vd_swap_export(
    xenon::kernel::KernelProcess& process, xenon::core::ExportCallContext& context);

// Convenience registrar for callers (tests/tools) that already have a
// constructed KernelProcess - see xboxkrnl_tls_exports.hpp's identical note
// for why XenonSession::init_exports() instead registers lambdas directly.
[[nodiscard]] bool register_xboxkrnl_video_exports(xenon::core::ExportRegistry& registry,
                                                    xenon::kernel::KernelProcess& process);

}  // namespace xenon::xbox
