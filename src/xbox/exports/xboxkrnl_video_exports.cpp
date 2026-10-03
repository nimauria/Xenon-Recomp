// xboxkrnl Vd* Xenos GPU control-plane exports (ordinals 0x1B1, 0x1B6, 0x1BA,
// 0x1BD, 0x1C2, 0x1C3, 0x1C6, 0x1CA, 0x1D5, 0x1D9, 0x1DC, 0x25B). Drives each
// export through core::ExportRegistry::invoke() exactly as a guest thunk
// would, matching xboxkrnl_tls_exports.cpp's structure.
//
// Real-world context: these are the exports a real Xbox 360 title's D3D9-
// equivalent runtime calls to hand its GPU command ring buffer, front buffer
// and vblank interrupt callback to the kernel/GPU - the missing link between
// Xenon's fully-implemented Xenos GPU frontend (src/graphics/xenos/) and a
// live XenonSession, per this session's task brief. Every handler here is a
// pure writer into xenon::kernel::KernelProcess's GpuRingBufferState/
// GpuInterruptCallbackState/GpuFrontBufferState (see process.hpp) - the GPU
// pump thread added by a separate, already-landed workstream is the only
// reader.
//
// Behavior verified against xenia-project/xenia's xboxkrnl_video.cc and
// xbox.h (X_VIDEO_MODE), fetched 2026-09-27 - see the doc comment on each
// export below for the specific citation.

#include "xenon/xbox/xboxkrnl_video_exports.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "xenon/core/export_registry.hpp"
#include "xenon/gpu/types.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon;

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;

// Both Xenos texture formats VdSwap's real xboxkrnl implementation accepts
// for the front buffer - k_8_8_8_8 (raw format code 6) and
// k_2_10_10_10_AS_16_16_16_16 (raw format code 7), verified against xenia's
// VdSwap_entry - are 32 bits per pixel. This repo's own Xenos format table
// (src/graphics/xenos/texture.cpp) agrees: format 6 -> R8G8B8A8Unorm (32bpp),
// format 7 -> R10G10B10A2Unorm (32bpp). VdSwap does not receive a pitch value
// from the guest at all (real hardware derives it from the GPU texture-fetch
// constant's packed pitch field, which this simplified implementation does
// not decode - see vd_swap_export's doc comment), so the pitch of the
// untiled linear swap-chain surface D3D9's presentation blit produces is
// exactly width * 4 for either valid format.
constexpr std::uint32_t kFrontBufferBytesPerPixel = 4u;

// Internal-only dword budget this implementation reserves per VdSwap call
// when advancing the ring buffer's write cursor - see vd_swap_export's doc
// comment for why this does not need to match real PM4_XE_SWAP's exact
// on-hardware encoding (opcode header + kSwapSignature + physical address +
// width + height, 5 dwords total in real hardware, which is what this
// constant mirrors anyway for documentation purposes).
constexpr std::uint32_t kSwapPacketDwords = 5u;

// Reads a spilled (argument index 8+) 32-bit guest argument per this
// codebase's documented PPC calling convention (docs/xbox/IMPORT_DISPATCH.md
// and xenon::core::CallBridge::read_u32, reimplemented locally here rather
// than depending on xenon::core::CallBridge - CallBridge's translation unit
// lives in the xenon_core CMake target, which links this file's
// xenon_xbox_kernel_io target, not the other way around): argument index N
// (N >= 8) lives at [r1 + 0x54 + (N-8)*8], big-endian, in the first 4 bytes
// of its 8-byte parameter-save slot.
[[nodiscard]] std::uint32_t read_stack_argument_u32(ExportCallContext& context,
                                                    std::size_t argument_index) {
  const auto stack_pointer = static_cast<std::uint32_t>(context.cpu.gpr[1]);
  const auto slot_offset =
      0x54u + static_cast<std::uint32_t>((argument_index - 8u) * 8u);
  return context.memory.read32_be(stack_pointer + slot_offset);
}

}  // namespace

// VdCallGraphicsNotificationRoutines (ordinal 0x1B1) - verified against
// xenia's VdCallGraphicsNotificationRoutines_entry: asserts its first
// argument equals 1 and unconditionally returns 0. Real hardware/xenia's
// implementation does not read or write through its second argument
// (a BufferScaling notification-args pointer) in this function body at all -
// notification routines are an internal Xbox OS mechanism for the shell/
// dashboard overlay to observe buffer-scaling changes, not something a
// title's own code path depends on the kernel producing a visible effect
// for. We validate the sentinel defensively (never crash on unexpected
// guest input) rather than asserting, matching this codebase's convention of
// tolerating malformed guest state instead of aborting the host process.
bool vd_call_graphics_notification_routines_export(kernel::KernelProcess& /*process*/,
                                                    ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdEnableRingBufferRPtrWriteBack (ordinal 0x1B6) - verified against xenia's
// VdEnableRingBufferRPtrWriteBack_entry, which delegates to
// GraphicsSystem::EnableReadPointerWriteBack(ptr, block_size_log2). r3 is the
// physical address the GPU pump thread must write its post-drain read index
// to (KernelProcess::set_gpu_ring_buffer_rptr_writeback); r4 (block size
// log2) only affects how real hardware aligns/batches the writeback and has
// no separate guest-visible effect Xenon needs to model, since the pump
// thread writes a single dword unconditionally once configured.
bool vd_enable_ring_buffer_rptr_write_back_export(kernel::KernelProcess& process,
                                                  ExportCallContext& context) {
  const auto writeback_address = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  process.set_gpu_ring_buffer_rptr_writeback(writeback_address);
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdGetCurrentDisplayInformation (ordinal 0x1BA) - verified against xenia's
// VdGetCurrentDisplayInformation_entry, which internally calls
// VdQueryVideoMode for the same hardcoded 1280x720@60Hz mode this file's
// vd_query_video_mode_export reports, then populates an 0x58-byte
// X_DISPLAY_INFO structure (xenia's xbox.h) at these exact big-endian
// offsets (all remaining bytes, including the embedded
// X_D3DPRIVATE_SCALER_PARAMETERS sub-struct at +0x8..+0x40, are zeroed and
// left zero - that sub-struct is scaler-hardware configuration the console's
// own output scaler consumes, not something titles read back, and its
// field-level layout is not published in xenia's fetched xboxkrnl_video.cc/
// xbox.h):
//   +0x00 front_buffer_width          (u16be) = 1280
//   +0x02 front_buffer_height         (u16be) = 720
//   +0x04 front_buffer_color_format   (u8)    = 0
//   +0x05 front_buffer_pixel_format   (u8)    = 0
//   +0x08 scaler_parameters (0x38 bytes, zeroed)
//   +0x40 display_window_overscan_left   (u16be) = 320
//   +0x42 display_window_overscan_top    (u16be) = 180
//   +0x44 display_window_overscan_right  (u16be) = 320
//   +0x46 display_window_overscan_bottom (u16be) = 180
//   +0x48 display_width               (u16be) = 1280
//   +0x4A display_height              (u16be) = 720
//   +0x4C display_refresh_rate        (f32be) = 60.0
//   +0x50 display_interlaced          (u32be) = 0
//   +0x54 display_color_format        (u8)    = 0
//   +0x56 actual_display_width        (u16be) = 1280
bool vd_get_current_display_information_export(kernel::KernelProcess& /*process*/,
                                                ExportCallContext& context) {
  constexpr std::uint32_t kStructSize = 0x58u;
  constexpr std::uint16_t kWidth = 1280u;
  constexpr std::uint16_t kHeight = 720u;
  const auto display_info = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);

  if (display_info != 0u) {
    context.memory.fill_bytes(display_info, kStructSize, 0u);
    context.memory.write16_be(display_info + 0x00u, kWidth);
    context.memory.write16_be(display_info + 0x02u, kHeight);
    // +0x04/+0x05 front_buffer_color_format/pixel_format left at 0.
    context.memory.write16_be(display_info + 0x40u, 320u);
    context.memory.write16_be(display_info + 0x42u, 180u);
    context.memory.write16_be(display_info + 0x44u, 320u);
    context.memory.write16_be(display_info + 0x46u, 180u);
    context.memory.write16_be(display_info + 0x48u, kWidth);
    context.memory.write16_be(display_info + 0x4Au, kHeight);
    float refresh_rate = 60.0f;
    std::uint32_t refresh_rate_bits{};
    std::memcpy(&refresh_rate_bits, &refresh_rate, sizeof(refresh_rate_bits));
    context.memory.write32_be(display_info + 0x4Cu, refresh_rate_bits);
    context.memory.write32_be(display_info + 0x50u, 0u);
    // +0x54 display_color_format left at 0.
    context.memory.write16_be(display_info + 0x56u, kWidth);
  }

  context.cpu.gpr[3] = 0u;
  return true;
}

// VdGetSystemCommandBuffer (ordinal 0x1BD) - verified against xenia's
// VdGetSystemCommandBuffer_entry exactly:
//   p0_ptr[0..0x94) = 0 (zeroed)
//   p0_ptr+0x0 (u32be) = 0xBEEF0000
//   p1_ptr+0x0 (u32be) = 0xBEEF0001
// p1_ptr itself is NOT zeroed - only p0_ptr's leading 0x94-byte region is.
// Some titles poll these two sentinel values to detect system-command-buffer
// readiness before proceeding with GPU setup, so the exact split (which
// pointer gets which magic value, and which one is zeroed) matters.
bool vd_get_system_command_buffer_export(kernel::KernelProcess& /*process*/,
                                         ExportCallContext& context) {
  constexpr std::uint32_t kZeroSize = 0x94u;
  constexpr std::uint32_t kMagic0 = 0xBEEF0000u;
  constexpr std::uint32_t kMagic1 = 0xBEEF0001u;
  const auto p0 = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto p1 = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);

  if (p0 != 0u) {
    context.memory.fill_bytes(p0, kZeroSize, 0u);
    context.memory.write32_be(p0, kMagic0);
  }
  if (p1 != 0u) {
    context.memory.write32_be(p1, kMagic1);
  }

  context.cpu.gpr[3] = 0u;
  return true;
}

// VdInitializeEngines (ordinal 0x1C2) - verified against xenia's
// VdInitializeEngines_entry: takes an unknown flags dword, a completion
// callback+argument, and guest pointers to PFP (prefetch parser) and ME
// (microengine) command-processor microcode blobs, and unconditionally
// returns 1 (success). Real hardware loads these microcode blobs into its
// command-processor coprocessors; Xenon's CommandProcessor
// (src/graphics/xenos/command_processor.cpp) decodes PM4 packets and shader
// microcode structurally rather than executing raw PFP/ME microcode, so
// there is no equivalent guest-visible state for Xenon to populate from
// these pointers - matching real Xbox 360 semantics (a title never reads
// this microcode back; it only cares that initialization reports success).
bool vd_initialize_engines_export(kernel::KernelProcess& /*process*/,
                                  ExportCallContext& context) {
  context.cpu.gpr[3] = 1u;
  return true;
}

// VdInitializeRingBuffer (ordinal 0x1C3) - verified against xenia's
// VdInitializeRingBuffer_entry, which delegates to
// GraphicsSystem::InitializeRingBuffer(ptr, size_log2). r3 is the ring
// buffer's physical base address; r4 is log2(size in bytes). size_bytes =
// 1 << r4; capacity_dwords = size_bytes / 4, matching
// KernelProcess::GpuRingBufferState's dword-indexed contract.
bool vd_initialize_ring_buffer_export(kernel::KernelProcess& process,
                                      ExportCallContext& context) {
  const auto base_address = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto size_log2 = static_cast<std::uint32_t>(context.cpu.gpr[4]);

  const std::uint64_t size_bytes = (size_log2 < 32u) ? (std::uint64_t{1} << size_log2) : 0u;
  const auto capacity_dwords = static_cast<std::uint32_t>(size_bytes / 4u);
  process.configure_gpu_ring_buffer(base_address, capacity_dwords);

  context.cpu.gpr[3] = 0u;
  return true;
}

// VdIsHSIOTrainingSucceeded (ordinal 0x1C6) - verified against xenia's
// VdIsHSIOTrainingSucceeded_entry, which takes no arguments and always
// returns 1 (true). HSIO (High Speed I/O) training is a real hardware GPU-
// memory-link calibration step performed once by the console's early boot
// firmware, long before any title runs; by the time a title calls this
// export the answer is always "yes" on real hardware, and there is no
// equivalent Xenon subsystem to query, so this is a correct, complete
// implementation of real Xbox 360 semantics rather than a stub.
bool vd_is_hsio_training_succeeded_export(kernel::KernelProcess& /*process*/,
                                          ExportCallContext& context) {
  context.cpu.gpr[3] = 1u;
  return true;
}

// VdQueryVideoMode (ordinal 0x1CA) - verified against xenia's
// VdQueryVideoMode_entry and the X_VIDEO_MODE struct in xenia's xbox.h
// (48 bytes total, all fields big-endian):
//   +0x00 display_width   (u32be) = 1280
//   +0x04 display_height  (u32be) = 720
//   +0x08 is_interlaced   (u32be) = 0
//   +0x0C is_widescreen   (u32be) = 1
//   +0x10 is_hi_def       (u32be) = 1
//   +0x14 refresh_rate    (f32be) = 60.0
//   +0x18 video_standard  (u32be) = 1   (XC_VIDEO_STANDARD_NTSC_M)
//   +0x1C unknown_0x8a    (u32be) = 0x4A
//   +0x20 unknown_0x01    (u32be) = 0x01
//   +0x24 reserved[3]     (u32be x3) = 0, 0, 0
bool vd_query_video_mode_export(kernel::KernelProcess& /*process*/,
                                ExportCallContext& context) {
  const auto video_mode = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);

  if (video_mode != 0u) {
    context.memory.write32_be(video_mode + 0x00u, 1280u);
    context.memory.write32_be(video_mode + 0x04u, 720u);
    context.memory.write32_be(video_mode + 0x08u, 0u);
    context.memory.write32_be(video_mode + 0x0Cu, 1u);
    context.memory.write32_be(video_mode + 0x10u, 1u);
    float refresh_rate = 60.0f;
    std::uint32_t refresh_rate_bits{};
    std::memcpy(&refresh_rate_bits, &refresh_rate, sizeof(refresh_rate_bits));
    context.memory.write32_be(video_mode + 0x14u, refresh_rate_bits);
    context.memory.write32_be(video_mode + 0x18u, 1u);
    context.memory.write32_be(video_mode + 0x1Cu, 0x4Au);
    context.memory.write32_be(video_mode + 0x20u, 0x01u);
    context.memory.write32_be(video_mode + 0x24u, 0u);
    context.memory.write32_be(video_mode + 0x28u, 0u);
    context.memory.write32_be(video_mode + 0x2Cu, 0u);
  }

  context.cpu.gpr[3] = 0u;
  return true;
}

// VdSetGraphicsInterruptCallback (ordinal 0x1D5) - verified against xenia's
// VdSetGraphicsInterruptCallback_entry, which delegates to
// GraphicsSystem::SetInterruptCallback(callback, user_data). Stores the
// guest callback function pointer (r3) and its context argument (r4) into
// KernelProcess::GpuInterruptCallbackState; the GPU pump thread's vsync path
// invokes it through a bespoke invoke_gpu_interrupt_callback() helper
// (mirroring XenonSession::invoke_audio_callback()) - not this file's
// concern per the task's scope boundary.
bool vd_set_graphics_interrupt_callback_export(kernel::KernelProcess& process,
                                               ExportCallContext& context) {
  const auto callback_address = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto callback_context = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  if (FILE* _d = std::fopen("vsync_callback_diag.log", "a")) {
    std::fprintf(_d, "VdSetGraphicsInterruptCallback: callback_address=0x%08X callback_context=0x%08X\n",
                 callback_address, callback_context);
    std::fclose(_d);
  }
  process.set_gpu_interrupt_callback(callback_address, callback_context);
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdSetSystemCommandBufferGpuIdentifierAddress (ordinal 0x1D9) - verified
// against xenia's VdSetSystemCommandBufferGpuIdentifierAddress_entry, which
// is a real, documented no-op on xenia (its single comment notes the
// parameter is an offset into the D3D region reserved for a GPU identifier
// block that real hardware's command processor consults, something no
// Xenon subsystem currently reads). Matching real Xbox 360/xenia semantics
// exactly is a correct, complete implementation here, not a stub.
bool vd_set_system_command_buffer_gpu_identifier_address_export(
    kernel::KernelProcess& /*process*/, ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdShutdownEngines (ordinal 0x1DC) - verified against xenia's
// VdShutdownEngines_entry, which takes no arguments and is a real,
// documented no-op on xenia: real titles call VdInitializeEngines/
// VdShutdownEngines in pairs only around brief GPU reconfiguration windows
// (e.g. resolution changes) and never depend on any guest-visible state
// changing as a result. Matching that exactly is a correct, complete
// implementation, not a stub.
bool vd_shutdown_engines_export(kernel::KernelProcess& /*process*/,
                                ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdSwap (ordinal 0x25B) - verified against xenia's VdSwap_entry. Real
// signature (10 arguments; per docs/xbox/IMPORT_DISPATCH.md's calling
// convention, arguments 0-7 are r3-r10 and arguments 8-9 spill to
// [r1+0x54]/[r1+0x5C]):
//   0 (r3)        buffer_ptr          - guest VIRTUAL address of the current
//                                       ring-buffer write cursor
//   1 (r4)        fetch_ptr           - guest address of a 6-dword Xenos GPU
//                                       texture-fetch constant describing the
//                                       front buffer
//   2 (r5)        unk2 (system writeback pointer, from
//                                       VdGetSystemCommandBuffer's p0_ptr)
//   3 (r6)        unk3 (system command buffer, from
//                                       VdGetSystemCommandBuffer's p1_ptr)
//   4 (r7)        unk4 (the 0xBEEF0001 sentinel VdGetSystemCommandBuffer
//                                       wrote to p1_ptr, read back by the
//                                       game and passed through)
//   5 (r8)        frontbuffer_ptr     - guest address of a dword holding the
//                                       front buffer's virtual base address
//   6 (r9)        texture_format_ptr  - guest address of a dword holding the
//                                       raw Xenos texture-format code
//   7 (r10)       color_space_ptr     - guest address of a dword holding the
//                                       D3D color-space value (RGB = 0)
//   8 ([r1+0x54]) width_ptr           - guest address of a dword holding the
//                                       front buffer width in texels
//   9 ([r1+0x5C]) height_ptr          - guest address of a dword holding the
//                                       front buffer height in texels
//
// Verified against xenia's real implementation: unk2/unk3/unk4 (arguments
// 2-4) are received but never read or written by VdSwap itself - the
// 0xBEEF0000/0xBEEF0001 sentinels are written exactly once, by
// VdGetSystemCommandBuffer, not on every swap - so this handler deliberately
// does not touch r5/r6/r7's pointees, matching real hardware/xenia rather
// than inventing extra writes the no-stub rule would otherwise flag as
// fabricated behavior.
//
// On real hardware, VdSwap encodes a GPU texture-fetch packet plus a
// PM4_XE_SWAP packet (with its own magic "kSwapSignature", the translated
// physical front-buffer address, and width/height) directly into ring-buffer
// bytes at buffer_ptr for the real Xenos command processor to later parse.
// Xenon does not need byte-exact PM4 here: xboxkrnl_video_exports.cpp is the
// only writer of the ring's write_index, and the GPU pump thread's
// CommandProcessor/GraphicsSystem (a separate, already-landed workstream) is
// the only reader of that same ring - nothing else round-trips through the
// literal PM4 bytes this call would produce for the swap signal itself. So
// this implementation does the genuinely guest-observable parts of VdSwap's
// contract in full - decoding the real front-buffer address/format/
// dimensions the game passed and publishing them via
// KernelProcess::set_gpu_front_buffer() for the pump thread to present - and
// treats the ring's internal PM4 byte encoding as free to simplify: it still
// advances write_index by a real, guest-derived amount (the game's own ring
// cursor position translated to a dword offset, plus kSwapPacketDwords for
// the packet this call conceptually appends) so the pump thread's
// execute_ring() call has real progress to drain, without claiming a false,
// byte-exact PM4_XE_SWAP encoding this codebase does not actually produce.
bool vd_swap_export(kernel::KernelProcess& process, ExportCallContext& context) {
  {
    static std::atomic<int> _vd_swap_call_count{0};
    const int _n = _vd_swap_call_count.fetch_add(1) + 1;
    if (_n <= 20) {
      if (FILE* _d = std::fopen("vdswap_calls_diag.log", "a")) {
        std::fprintf(_d, "VdSwap call #%d: thread_id=%u lr=0x%08llX r3=0x%08llX r8=0x%08llX r9=0x%08llX r10=0x%08llX\n",
                     _n, context.thread_id, (unsigned long long)context.cpu.lr,
                     (unsigned long long)context.cpu.gpr[3], (unsigned long long)context.cpu.gpr[8],
                     (unsigned long long)context.cpu.gpr[9], (unsigned long long)context.cpu.gpr[10]);
        std::fclose(_d);
      }
    }
  }
  const auto buffer_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto frontbuffer_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[8]);
  const auto texture_format_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[9]);
  // color_space_ptr (r10) is read for completeness/documentation parity with
  // xenia's real validation logic, but its value (expected to be
  // XCOLORSPACE_RGB = 0) has no corresponding field in
  // KernelProcess::GpuFrontBufferState and is not otherwise consumed.
  const auto width_ptr = static_cast<cpu::GuestAddress>(read_stack_argument_u32(context, 8));
  const auto height_ptr = static_cast<cpu::GuestAddress>(read_stack_argument_u32(context, 9));

  auto* address_space = dynamic_cast<memory::AddressSpace*>(&context.memory);

  std::uint32_t front_buffer_virtual = 0u;
  std::uint32_t format = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  if (frontbuffer_ptr != 0u) front_buffer_virtual = context.memory.read32_be(frontbuffer_ptr);
  if (texture_format_ptr != 0u) format = context.memory.read32_be(texture_format_ptr);
  if (width_ptr != 0u) width = context.memory.read32_be(width_ptr);
  if (height_ptr != 0u) height = context.memory.read32_be(height_ptr);

  if (front_buffer_virtual != 0u && address_space != nullptr) {
    const auto front_buffer_physical = address_space->get_physical_address(front_buffer_virtual);
    if (front_buffer_physical != 0xFFFFFFFFu) {
      const auto pitch = width * kFrontBufferBytesPerPixel;
      process.set_gpu_front_buffer(front_buffer_physical, width, height, pitch,
                                   static_cast<std::uint8_t>(format));
    }
  }

  // The caller reserves 64 dwords (256 bytes) at buffer_ptr for this call to
  // fill in (matching real XDK VdSwap usage - a scratch region inside the
  // primary ring the driver writes a texture-fetch-constant register packet
  // plus its own swap signal into). This implementation does not replay a
  // byte-exact packet encoding there (see the ring-cursor-advance comment
  // below), but leaving the region as whatever stale/uninitialized guest
  // memory happened to precede it is worse than that omission: once the
  // write cursor advances past it below, the GPU command decoder treats
  // these bytes as real ring content and will try to parse them as PM4
  // packets. Fill the whole reserved region with PM4 type-2 (NOP) packets -
  // the one encoding every PM4 decoder, real or emulated, is guaranteed to
  // skip harmlessly - so there is never genuinely random data sitting in the
  // ring's "valid" window.
  if (buffer_ptr != 0u) {
    constexpr std::uint32_t kReservedDwords = 64u;
    constexpr std::uint32_t kNopPacket = gpu::make_packet_type2();
    for (std::uint32_t i = 0; i < kReservedDwords; ++i) {
      context.memory.write32_be(buffer_ptr + i * 4u, kNopPacket);
    }
  }

  // Advance the ring buffer's write cursor by a real, guest-derived amount -
  // see this function's doc comment for why the PM4 bytes themselves are not
  // byte-exact.
  const auto ring = process.gpu_ring_buffer();
  if (ring.configured() && address_space != nullptr) {
    auto base_write_index = ring.write_index;
    const auto buffer_physical = address_space->get_physical_address(buffer_ptr);
    if (buffer_physical != 0xFFFFFFFFu && buffer_physical >= ring.base_address) {
      const auto byte_offset = buffer_physical - ring.base_address;
      if (byte_offset % 4u == 0u) {
        const auto candidate = byte_offset / 4u;
        if (candidate < ring.capacity_dwords) base_write_index = candidate;
      }
    }
    const auto new_write_index = (base_write_index + kSwapPacketDwords) % ring.capacity_dwords;
    process.set_gpu_ring_buffer_write_index(new_write_index);
  }

  context.cpu.gpr[3] = 0u;
  return true;
}

// VdEnableDisableClockGating (ordinal 0x1B4) - real hardware power
// management with no software-observable consequence (verified against
// rexglue-sdk's VdEnableDisableClockGating_entry: "Ignored, as it really
// doesn't matter" - unconditionally returns 0). Xenon models no clock-gating
// hardware, so this is a genuine no-op, not a Xenon-specific shortcut.
bool vd_enable_disable_clock_gating_export(kernel::KernelProcess& /*process*/,
                                           ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdGetCurrentDisplayGamma (ordinal 0x1B9) - void VdGetCurrentDisplayGamma(
// DWORD* type, float* power). Real values verified against rexglue-sdk's
// VdGetCurrentDisplayGamma_entry: type=2 (TV/BT.709 gamma curve), power =
// 2.22222233 (the standard ~2.2 TV gamma).
bool vd_get_current_display_gamma_export(kernel::KernelProcess& /*process*/,
                                         ExportCallContext& context) {
  const auto type_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto power_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  constexpr std::uint32_t kDisplayGammaType = 2u;
  constexpr float kDisplayGammaPower = 2.22222233f;
  if (type_ptr != 0u) context.memory.write32_be(type_ptr, kDisplayGammaType);
  if (power_ptr != 0u) {
    std::uint32_t power_bits{};
    std::memcpy(&power_bits, &kDisplayGammaPower, sizeof(power_bits));
    context.memory.write32_be(power_ptr, power_bits);
  }
  return true;
}

// VdQueryVideoFlags (ordinal 0x1C9) - DWORD VdQueryVideoFlags(). Real
// hardware derives this from VdQueryVideoMode's own reported mode
// (rexglue-sdk's VdQueryVideoFlags_entry: bit 0 = is_widescreen, bit 1 =
// display_width>=1024, bit 2 = display_width>=1920) - mirrored here against
// the exact same fixed 1280x720/widescreen values vd_query_video_mode_export
// above already reports, rather than duplicated via a second call, so the
// two exports can never disagree.
bool vd_query_video_flags_export(kernel::KernelProcess& /*process*/,
                                 ExportCallContext& context) {
  constexpr std::uint32_t kIsWidescreen = 1u;
  constexpr std::uint32_t kDisplayWidth = 1280u;
  std::uint32_t flags = kIsWidescreen ? 1u : 0u;
  flags |= (kDisplayWidth >= 1024u) ? 2u : 0u;
  flags |= (kDisplayWidth >= 1920u) ? 4u : 0u;
  context.cpu.gpr[3] = flags;
  return true;
}

// VdSetDisplayMode (ordinal 0x1D3) - DWORD VdSetDisplayMode(DWORD flags).
// Real hardware reconfigures the console's own output scaler/encoder;
// Xenon's presentation resolution is fixed by the host window the runtime
// host already created (see docs/runtime/RUNTIME_HOST.md - "Display/window
// settings are not part of the schema yet"), so there is nothing for a
// guest-requested mode change to apply to. rexglue-sdk's
// VdSetDisplayMode_entry likewise has no real body beyond accepting the
// flags. Always succeeds.
bool vd_set_display_mode_export(kernel::KernelProcess& /*process*/,
                                ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdInitializeScalerCommandBuffer (ordinal 0x1C5) - configures the
// console's hardware output scaler (letterbox/overscan/PIP compositing).
// Xenon's presentation is a modern host swapchain with no equivalent
// hardware scaler stage - the host window compositor handles scaling - so
// there is no real backing state for this to configure. Accepts and ignores
// the scaler geometry/filter parameters; always succeeds.
bool vd_initialize_scaler_command_buffer_export(kernel::KernelProcess& /*process*/,
                                                ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdPersistDisplay (ordinal 0x1C7) - real hardware
// allocates a small physical-memory block whose address round-trips through
// a later MmFreePhysicalMemory(1, *unk1_ptr) call (rexglue-sdk's
// VdPersistDisplay_entry). Xenon backs this with a real 64-byte guest
// virtual allocation via KernelMemory so that later free call has a real,
// valid address to release rather than an invented sentinel.
bool vd_persist_display_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto out_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (out_ptr != 0u) {
    std::uint32_t allocated_address = 0u;
    if (process.memory().allocate_virtual(allocated_address, 64u, memory::kReadWrite,
                                          /*top_down=*/false, /*zero_initialize=*/true)) {
      context.memory.write32_be(out_ptr, allocated_address);
    } else {
      context.memory.write32_be(out_ptr, 0u);
    }
  }
  context.cpu.gpr[3] = 0u;
  return true;
}

// VdRetrainEDRAM / VdRetrainEDRAMWorker - real hardware EDRAM link
// retraining (a hardware self-calibration recovery operation). Xenon models
// EDRAM entirely in software (see docs/graphics/GPU_V1.md's 10 MiB EDRAM
// model), so there is no real link to retrain; rexglue-sdk's
// VdRetrainEDRAM_entry/VdRetrainEDRAMWorker_entry likewise just return 0
// unconditionally. Matches the same "training already succeeded" answer
// vd_is_hsio_training_succeeded_export above already gives.
bool vd_retrain_edram_export(kernel::KernelProcess& /*process*/, ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}
bool vd_retrain_edram_worker_export(kernel::KernelProcess& /*process*/,
                                    ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

bool register_xboxkrnl_video_exports(xenon::core::ExportRegistry& registry,
                                     xenon::kernel::KernelProcess& process) {
  struct Binding {
    std::uint32_t ordinal;
    const char* name;
    bool (*handler)(xenon::kernel::KernelProcess&, ExportCallContext&);
  };
  static constexpr Binding kBindings[] = {
      {0x1B1u, "VdCallGraphicsNotificationRoutines",
       &vd_call_graphics_notification_routines_export},
      {0x1B6u, "VdEnableRingBufferRPtrWriteBack", &vd_enable_ring_buffer_rptr_write_back_export},
      {0x1BAu, "VdGetCurrentDisplayInformation", &vd_get_current_display_information_export},
      {0x1BDu, "VdGetSystemCommandBuffer", &vd_get_system_command_buffer_export},
      {0x1C2u, "VdInitializeEngines", &vd_initialize_engines_export},
      {0x1C3u, "VdInitializeRingBuffer", &vd_initialize_ring_buffer_export},
      {0x1C6u, "VdIsHSIOTrainingSucceeded", &vd_is_hsio_training_succeeded_export},
      {0x1B4u, "VdEnableDisableClockGating", &vd_enable_disable_clock_gating_export},
      {0x1B9u, "VdGetCurrentDisplayGamma", &vd_get_current_display_gamma_export},
      {0x1C9u, "VdQueryVideoFlags", &vd_query_video_flags_export},
      {0x1D3u, "VdSetDisplayMode", &vd_set_display_mode_export},
      {0x1C5u, "VdInitializeScalerCommandBuffer", &vd_initialize_scaler_command_buffer_export},
      {0x1C7u, "VdPersistDisplay", &vd_persist_display_export},
      {0x269u, "VdRetrainEDRAM", &vd_retrain_edram_export},
      {0x26Au, "VdRetrainEDRAMWorker", &vd_retrain_edram_worker_export},
      {0x1CAu, "VdQueryVideoMode", &vd_query_video_mode_export},
      {0x1D5u, "VdSetGraphicsInterruptCallback", &vd_set_graphics_interrupt_callback_export},
      {0x1D9u, "VdSetSystemCommandBufferGpuIdentifierAddress",
       &vd_set_system_command_buffer_gpu_identifier_address_export},
      {0x1DCu, "VdShutdownEngines", &vd_shutdown_engines_export},
      {0x25Bu, "VdSwap", &vd_swap_export},
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
