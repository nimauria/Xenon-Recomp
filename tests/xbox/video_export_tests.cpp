// xboxkrnl Vd* Xenos GPU control-plane export tests (ordinals 0x1B1, 0x1B6,
// 0x1BA, 0x1BD, 0x1C2, 0x1C3, 0x1C6, 0x1CA, 0x1D5, 0x1D9, 0x1DC, 0x25B).
// Drives each export through a real core::ExportRegistry against a real
// kernel::KernelProcess + memory::AddressSpace, exactly as a guest thunk
// would - mirrors tls_export_tests.cpp's fixture pattern.

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>

#include "xenon/core/export_registry.hpp"
#include "xenon/gpu/front_buffer_swap.hpp"
#include "xenon/gpu/types.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_video_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kOrdCallGraphicsNotificationRoutines = 0x1B1u;
constexpr std::uint32_t kOrdEnableRingBufferRPtrWriteBack = 0x1B6u;
constexpr std::uint32_t kOrdGetCurrentDisplayInformation = 0x1BAu;
constexpr std::uint32_t kOrdGetSystemCommandBuffer = 0x1BDu;
constexpr std::uint32_t kOrdInitializeEngines = 0x1C2u;
constexpr std::uint32_t kOrdInitializeRingBuffer = 0x1C3u;
constexpr std::uint32_t kOrdIsHSIOTrainingSucceeded = 0x1C6u;
constexpr std::uint32_t kOrdQueryVideoMode = 0x1CAu;
constexpr std::uint32_t kOrdSetGraphicsInterruptCallback = 0x1D5u;
constexpr std::uint32_t kOrdSetSystemCommandBufferGpuIdentifierAddress = 0x1D9u;
constexpr std::uint32_t kOrdShutdownEngines = 0x1DCu;
constexpr std::uint32_t kOrdSwap = 0x25Bu;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  std::shared_ptr<kernel::KernelThread> thread;
  core::ExportRegistry registry;

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    thread = process->thread_manager().create_thread([] { return 0u; }, {});
    assert(thread != nullptr);
    assert(xbox::register_xboxkrnl_video_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, thread->thread_id()};
    return registry.invoke("xboxkrnl", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc(std::uint32_t size, std::uint32_t alignment = 16u) {
    memory::GuestAddress addr{};
    assert(address_space->allocate(size, alignment, memory::kReadWrite, false, addr));
    return addr;
  }
};

void test_ordinals_are_registered() {
  Fixture fixture;
  const std::pair<std::uint32_t, const char*> kExpected[] = {
      {kOrdCallGraphicsNotificationRoutines, "VdCallGraphicsNotificationRoutines"},
      {kOrdEnableRingBufferRPtrWriteBack, "VdEnableRingBufferRPtrWriteBack"},
      {kOrdGetCurrentDisplayInformation, "VdGetCurrentDisplayInformation"},
      {kOrdGetSystemCommandBuffer, "VdGetSystemCommandBuffer"},
      {kOrdInitializeEngines, "VdInitializeEngines"},
      {kOrdInitializeRingBuffer, "VdInitializeRingBuffer"},
      {kOrdIsHSIOTrainingSucceeded, "VdIsHSIOTrainingSucceeded"},
      {kOrdQueryVideoMode, "VdQueryVideoMode"},
      {kOrdSetGraphicsInterruptCallback, "VdSetGraphicsInterruptCallback"},
      {kOrdSetSystemCommandBufferGpuIdentifierAddress,
       "VdSetSystemCommandBufferGpuIdentifierAddress"},
      {kOrdShutdownEngines, "VdShutdownEngines"},
      {kOrdSwap, "VdSwap"},
  };
  for (const auto& [ordinal, name] : kExpected) {
    assert(fixture.registry.contains("xboxkrnl.exe", ordinal));
    assert(fixture.registry.contains("xboxkrnl", name));
  }
}

// VdInitializeRingBuffer's r4 is log2 of the ring size in 8-byte units: the
// ring is 1 << (r4 + 3) bytes (xenia's and rexglue's InitializeRingBuffer).
// AC6 sizes its first ring with r4 = 12 and places its second ring exactly
// 32 KiB later; read as 1 << r4 bytes the second ring was 16384 dwords
// instead of 131072, the title's write pointer ran past the end and the
// GPU stopped executing commands after 1366 frames.
void test_initialize_ring_buffer_converts_log2_size() {
  Fixture fixture;
  constexpr std::uint32_t kBaseAddress = 0x0010'0000u;
  constexpr std::uint32_t kSizeLog2 = 16u;  // 512 KiB -> 131072 dwords
  cpu::CpuState cpu{};
  cpu.gpr[3] = kBaseAddress;
  cpu.gpr[4] = kSizeLog2;
  assert(fixture.invoke(kOrdInitializeRingBuffer, cpu).success);

  const auto ring = fixture.process->gpu_ring_buffer();
  assert(ring.base_address == kBaseAddress);
  assert(ring.capacity_dwords == (1u << (kSizeLog2 + 3u)) / 4u);
  assert(ring.configured());

  // AC6's first ring: r4 = 12 spans the 32 KiB up to its second ring.
  cpu.gpr[3] = 0x19B38000u;
  cpu.gpr[4] = 12u;
  assert(fixture.invoke(kOrdInitializeRingBuffer, cpu).success);
  const auto first = fixture.process->gpu_ring_buffer();
  assert(first.base_address + first.capacity_dwords * 4u == 0x19B40000u);
}

// VdEnableRingBufferRPtrWriteBack must store the physical writeback address.
void test_enable_ring_buffer_rptr_write_back_stores_address() {
  Fixture fixture;
  constexpr std::uint32_t kWritebackAddress = 0x0020'0000u;
  cpu::CpuState cpu{};
  cpu.gpr[3] = kWritebackAddress;
  cpu.gpr[4] = 6u;  // block size log2, not separately modeled
  assert(fixture.invoke(kOrdEnableRingBufferRPtrWriteBack, cpu).success);

  assert(fixture.process->gpu_ring_buffer().rptr_writeback_address == kWritebackAddress);
}

// VdSetGraphicsInterruptCallback must store callback+context such that a
// subsequent KernelProcess::gpu_interrupt_callback() reflects it.
void test_set_graphics_interrupt_callback_round_trips() {
  Fixture fixture;
  constexpr std::uint32_t kCallback = 0x8200'1000u;
  constexpr std::uint32_t kContext = 0xCAFEBABEu;
  cpu::CpuState cpu{};
  cpu.gpr[3] = kCallback;
  cpu.gpr[4] = kContext;
  assert(fixture.invoke(kOrdSetGraphicsInterruptCallback, cpu).success);

  const auto callback = fixture.process->gpu_interrupt_callback();
  assert(callback.callback_address == kCallback);
  assert(callback.context == kContext);
}

// VdGetSystemCommandBuffer must zero p0_ptr's leading 0x94 bytes, write
// 0xBEEF0000 at p0_ptr+0 and 0xBEEF0001 at p1_ptr+0, and must NOT zero
// p1_ptr's memory beyond its own first dword.
void test_get_system_command_buffer_writes_exact_sentinels() {
  Fixture fixture;
  const auto p0 = fixture.alloc(0x94u);
  const auto p1 = fixture.alloc(0x10u);

  // Pre-fill both with a recognizable marker so zeroing is actually observed
  // (not just coincidentally already zero).
  fixture.address_space->fill(p0, 0x94u, 0xAAu);
  fixture.address_space->fill(p1, 0x10u, 0xAAu);
  fixture.address_space->write32_be(p1 + 4u, 0x11223344u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = p0;
  cpu.gpr[4] = p1;
  assert(fixture.invoke(kOrdGetSystemCommandBuffer, cpu).success);

  assert(fixture.address_space->read32_be(p0) == 0xBEEF0000u);
  for (std::uint32_t offset = 4; offset < 0x94u; offset += 4u) {
    assert(fixture.address_space->read32_be(p0 + offset) == 0u);
  }
  assert(fixture.address_space->read32_be(p1) == 0xBEEF0001u);
  // p1 beyond its own first dword must be untouched (real xenia only zeroes
  // p0_ptr, never p1_ptr).
  assert(fixture.address_space->read32_be(p1 + 4u) == 0x11223344u);
}

// VdQueryVideoMode must populate the 48-byte X_VIDEO_MODE struct at the
// documented field offsets/values.
void test_query_video_mode_populates_struct() {
  Fixture fixture;
  const auto mode = fixture.alloc(48u);
  cpu::CpuState cpu{};
  cpu.gpr[3] = mode;
  assert(fixture.invoke(kOrdQueryVideoMode, cpu).success);

  assert(fixture.address_space->read32_be(mode + 0x00u) == 1280u);
  assert(fixture.address_space->read32_be(mode + 0x04u) == 720u);
  assert(fixture.address_space->read32_be(mode + 0x08u) == 0u);
  assert(fixture.address_space->read32_be(mode + 0x0Cu) == 1u);
  assert(fixture.address_space->read32_be(mode + 0x10u) == 1u);
  const auto refresh_bits = fixture.address_space->read32_be(mode + 0x14u);
  float refresh{};
  std::memcpy(&refresh, &refresh_bits, sizeof(refresh));
  assert(std::abs(refresh - 60.0f) < 0.001f);
  assert(fixture.address_space->read32_be(mode + 0x18u) == 1u);
  assert(fixture.address_space->read32_be(mode + 0x1Cu) == 0x4Au);
  assert(fixture.address_space->read32_be(mode + 0x20u) == 0x01u);
  assert(fixture.address_space->read32_be(mode + 0x24u) == 0u);
  assert(fixture.address_space->read32_be(mode + 0x28u) == 0u);
  assert(fixture.address_space->read32_be(mode + 0x2Cu) == 0u);
}

// VdGetCurrentDisplayInformation must populate the 0x58-byte X_DISPLAY_INFO
// struct at the documented offsets and leave the rest zeroed.
void test_get_current_display_information_populates_struct() {
  Fixture fixture;
  const auto info = fixture.alloc(0x58u);
  fixture.address_space->fill(info, 0x58u, 0xFFu);

  cpu::CpuState cpu{};
  cpu.gpr[3] = info;
  assert(fixture.invoke(kOrdGetCurrentDisplayInformation, cpu).success);

  assert(fixture.address_space->read16_be(info + 0x00u) == 1280u);
  assert(fixture.address_space->read16_be(info + 0x02u) == 720u);
  assert(fixture.address_space->read16_be(info + 0x40u) == 320u);
  assert(fixture.address_space->read16_be(info + 0x42u) == 180u);
  assert(fixture.address_space->read16_be(info + 0x44u) == 320u);
  assert(fixture.address_space->read16_be(info + 0x46u) == 180u);
  assert(fixture.address_space->read16_be(info + 0x48u) == 1280u);
  assert(fixture.address_space->read16_be(info + 0x4Au) == 720u);
  const auto refresh_bits = fixture.address_space->read32_be(info + 0x4Cu);
  float refresh{};
  std::memcpy(&refresh, &refresh_bits, sizeof(refresh));
  assert(std::abs(refresh - 60.0f) < 0.001f);
  assert(fixture.address_space->read32_be(info + 0x50u) == 0u);
  assert(fixture.address_space->read16_be(info + 0x56u) == 1280u);
  // The embedded scaler_parameters sub-struct (+0x08..+0x40) is zeroed, not
  // left at the 0xFF pre-fill marker.
  assert(fixture.address_space->read32_be(info + 0x08u) == 0u);
  assert(fixture.address_space->read32_be(info + 0x30u) == 0u);
}

// VdInitializeEngines/VdIsHSIOTrainingSucceeded return success (1);
// VdShutdownEngines/VdSetSystemCommandBufferGpuIdentifierAddress/
// VdCallGraphicsNotificationRoutines are documented real no-ops (0).
void test_simple_return_value_exports() {
  Fixture fixture;

  cpu::CpuState init_cpu{};
  assert(fixture.invoke(kOrdInitializeEngines, init_cpu).success);
  assert(init_cpu.gpr[3] == 1u);

  cpu::CpuState hsio_cpu{};
  assert(fixture.invoke(kOrdIsHSIOTrainingSucceeded, hsio_cpu).success);
  assert(hsio_cpu.gpr[3] == 1u);

  cpu::CpuState shutdown_cpu{};
  assert(fixture.invoke(kOrdShutdownEngines, shutdown_cpu).success);
  assert(shutdown_cpu.gpr[3] == 0u);

  cpu::CpuState gpu_id_cpu{};
  gpu_id_cpu.gpr[3] = 0x1234u;
  assert(fixture.invoke(kOrdSetSystemCommandBufferGpuIdentifierAddress, gpu_id_cpu).success);
  assert(gpu_id_cpu.gpr[3] == 0u);

  cpu::CpuState notify_cpu{};
  notify_cpu.gpr[3] = 1u;
  assert(fixture.invoke(kOrdCallGraphicsNotificationRoutines, notify_cpu).success);
  assert(notify_cpu.gpr[3] == 0u);
}

// VdSwap must read the front-buffer virtual address/format/width/height from
// the guest-memory pointers the game passed (r8, r9, and the stack-spilled
// width/height pointers at [r1+0x54]/[r1+0x5C]), translate the front-buffer
// address to physical, and publish it via KernelProcess::gpu_front_buffer().
void test_swap_populates_front_buffer() {
  Fixture fixture;

  // The actual front-buffer surface memory - only its address matters here,
  // so a single committed page is sufficient (its real content is never
  // read).
  constexpr std::uint32_t kWidth = 1280u;
  constexpr std::uint32_t kHeight = 720u;
  constexpr std::uint32_t kFormat = 6u;  // k_8_8_8_8
  const auto front_buffer_va = fixture.alloc(0x1000u, 0x1000u);
  const auto expected_physical = fixture.address_space->get_physical_address(front_buffer_va);
  assert(expected_physical != 0xFFFFFFFFu);

  // Output-pointer indirection cells VdSwap dereferences.
  const auto frontbuffer_cell = fixture.alloc(4u);
  const auto format_cell = fixture.alloc(4u);
  const auto width_cell = fixture.alloc(4u);
  const auto height_cell = fixture.alloc(4u);
  fixture.address_space->write32_be(frontbuffer_cell, front_buffer_va);
  fixture.address_space->write32_be(format_cell, kFormat);
  fixture.address_space->write32_be(width_cell, kWidth);
  fixture.address_space->write32_be(height_cell, kHeight);

  // A guest stack frame big enough to hold the 9th/10th argument slots.
  const auto stack = fixture.alloc(0x80u, 16u);
  fixture.address_space->write32_be(stack + 0x54u, width_cell);
  fixture.address_space->write32_be(stack + 0x5Cu, height_cell);

  cpu::CpuState cpu{};
  cpu.gpr[1] = stack;
  cpu.gpr[3] = 0u;  // buffer_ptr: no ring configured in this test
  cpu.gpr[8] = frontbuffer_cell;
  cpu.gpr[9] = format_cell;
  cpu.gpr[10] = 0u;  // color_space_ptr, unused
  assert(fixture.invoke(kOrdSwap, cpu).success);

  const auto front_buffer = fixture.process->gpu_front_buffer();
  assert(front_buffer.base_address == expected_physical);
  assert(front_buffer.width == kWidth);
  assert(front_buffer.height == kHeight);
  assert(front_buffer.pitch == kWidth * 4u);
  assert(front_buffer.format == kFormat);
  assert(front_buffer.generation >= 1u);
  assert(!front_buffer.has_fetch_constant);
}

// Regression (Ace Combat 6): VdSwap's second argument is the front buffer's
// texture fetch constant, which says how scan-out reads it (tiled, endian
// swap, pitch, format). It was ignored, the presented texture was described
// from width/height alone and never valid, and every present failed with
// "invalid Xenos texture descriptor" - a black window. VdSwap now keeps the
// fetch constant with its base address set to the front buffer, as xenia's
// VdSwap does.
void test_swap_keeps_the_front_buffer_fetch_constant() {
  Fixture fixture;
  const auto front_buffer_va = fixture.alloc(0x4000u, 0x1000u);
  const auto physical = fixture.address_space->get_physical_address(front_buffer_va);
  assert(physical != 0xFFFFFFFFu && (physical & 0xFFFu) == 0u);
  const auto frontbuffer_cell = fixture.alloc(4u);
  const auto format_cell = fixture.alloc(4u);
  const auto width_cell = fixture.alloc(4u);
  const auto height_cell = fixture.alloc(4u);
  fixture.address_space->write32_be(frontbuffer_cell, front_buffer_va);
  fixture.address_space->write32_be(format_cell, 6u);
  fixture.address_space->write32_be(width_cell, 1280u);
  fixture.address_space->write32_be(height_cell, 720u);
  // A tiled 1280x720 k_8_8_8_8 texture, 8in32 endian, whose header holds a
  // stale base address (0x12345000).
  const std::array<std::uint32_t, 6> header{
      0x80000002u | (1280u / 32u) << 22, 0x12345000u | 2u << 6 | 6u,
      (1279u) | (719u << 13), 0x00000000u, 0x00000000u, 0x00000200u};
  const auto fetch = fixture.alloc(24u, 16u);
  for (std::uint32_t i = 0; i < 6u; ++i) fixture.address_space->write32_be(fetch + i * 4u, header[i]);
  const auto stack = fixture.alloc(0x80u, 16u);
  fixture.address_space->write32_be(stack + 0x54u, width_cell);
  fixture.address_space->write32_be(stack + 0x5Cu, height_cell);

  cpu::CpuState cpu{};
  cpu.gpr[1] = stack;
  cpu.gpr[3] = 0u;
  cpu.gpr[4] = fetch;
  cpu.gpr[8] = frontbuffer_cell;
  cpu.gpr[9] = format_cell;
  assert(fixture.invoke(kOrdSwap, cpu).success);

  const auto front_buffer = fixture.process->gpu_front_buffer();
  assert(front_buffer.has_fetch_constant);
  auto expected = header;
  expected[1] = (header[1] & 0xFFFu) | physical;
  assert(front_buffer.fetch_constant == expected);
}

// VdSwap fills its reserved ring region but never moves the ring's write
// pointer: the title publishes it with its own CP_RB_WPTR store, as on
// hardware. Moving it as well raced that store - AC6's command processor
// ended up past a write pointer VdSwap had moved backwards and replayed its
// whole ring as new work.
void test_swap_leaves_ring_write_index_to_the_title() {
  Fixture fixture;
  constexpr std::uint32_t kCapacityDwords = 256u;
  const auto ring_va = fixture.alloc(kCapacityDwords * 4u, 0x1000u);
  const auto ring_physical = fixture.address_space->get_physical_address(ring_va);
  assert(ring_physical != 0xFFFFFFFFu);
  fixture.process->configure_gpu_ring_buffer(ring_physical, kCapacityDwords);
  const auto stack = fixture.alloc(0x80u, 16u);

  // The title has already published work up to dword 120; its swap region
  // starts earlier, at dword 10.
  fixture.process->set_gpu_ring_buffer_write_index(120u);
  cpu::CpuState cpu{};
  cpu.gpr[1] = stack;
  cpu.gpr[3] = ring_va + 10u * 4u;
  assert(fixture.invoke(kOrdSwap, cpu).success);
  assert(fixture.process->gpu_ring_buffer().write_index == 120u);
  // The 64 reserved dwords hold NOPs for the command processor to skip.
  for (std::uint32_t i = 0; i < 64u; ++i)
    assert(fixture.address_space->read32_be(ring_va + (10u + i) * 4u) ==
           gpu::make_packet_type2());
}

// Regression (Ace Combat 6 intro movie): VdSwap published the new front buffer
// when the CPU called it, before the title had even submitted the frame's
// resolve. The next vsync presented a buffer still holding an older frame and
// the real one was never shown - about a third of the movie's presents
// repeated a frame (choppy, out-of-order looking playback). With a ring region
// the flip is a swap record the command processor executes in stream order;
// the front buffer changes only then.
void test_swap_takes_effect_in_command_stream_order() {
  Fixture fixture;
  constexpr std::uint32_t kCapacityDwords = 256u;
  const auto ring_va = fixture.alloc(kCapacityDwords * 4u, 0x1000u);
  const auto front_buffer_va = fixture.alloc(0x4000u, 0x1000u);
  const auto physical = fixture.address_space->get_physical_address(front_buffer_va);
  assert(physical != 0xFFFFFFFFu);
  const auto frontbuffer_cell = fixture.alloc(4u);
  const auto format_cell = fixture.alloc(4u);
  const auto width_cell = fixture.alloc(4u);
  const auto height_cell = fixture.alloc(4u);
  fixture.address_space->write32_be(frontbuffer_cell, front_buffer_va);
  fixture.address_space->write32_be(format_cell, 6u);
  fixture.address_space->write32_be(width_cell, 1280u);
  fixture.address_space->write32_be(height_cell, 720u);
  const std::array<std::uint32_t, 6> header{
      0x80000002u | (1280u / 32u) << 22, 0x12345000u | 2u << 6 | 6u,
      (1279u) | (719u << 13), 0x00000000u, 0x00000000u, 0x00000200u};
  const auto fetch = fixture.alloc(24u, 16u);
  for (std::uint32_t i = 0; i < 6u; ++i) fixture.address_space->write32_be(fetch + i * 4u, header[i]);
  const auto stack = fixture.alloc(0x80u, 16u);
  fixture.address_space->write32_be(stack + 0x54u, width_cell);
  fixture.address_space->write32_be(stack + 0x5Cu, height_cell);

  cpu::CpuState cpu{};
  cpu.gpr[1] = stack;
  cpu.gpr[3] = ring_va + 10u * 4u;
  cpu.gpr[4] = fetch;
  cpu.gpr[8] = frontbuffer_cell;
  cpu.gpr[9] = format_cell;
  assert(fixture.invoke(kOrdSwap, cpu).success);

  // Nothing is scanned out until the command processor reaches the swap.
  assert(fixture.process->gpu_front_buffer().generation == 0u);

  // The region starts with the swap record, then NOPs to its end.
  std::array<std::uint32_t, gpu::kFrontBufferSwapPacketDwords> packet{};
  for (std::uint32_t i = 0; i < packet.size(); ++i)
    packet[i] = fixture.address_space->read32_be(ring_va + (10u + i) * 4u);
  const auto decoded_header = gpu::decode_packet_header(packet[0]);
  assert(decoded_header.type == gpu::PacketType::Type3);
  assert(decoded_header.opcode == gpu::Type3Opcode::Nop);
  assert(decoded_header.count == gpu::kFrontBufferSwapPayloadDwords);
  const auto swap = gpu::decode_front_buffer_swap(std::span(packet).subspan(1));
  assert(swap.has_value());
  assert(swap->base_address == physical);
  assert(swap->width == 1280u && swap->height == 720u && swap->pitch == 1280u * 4u);
  assert(swap->format == 6u);
  assert(swap->has_fetch_constant);
  auto expected = header;
  expected[1] = (header[1] & 0xFFFu) | physical;
  assert(swap->fetch_constant == expected);
  for (std::uint32_t i = gpu::kFrontBufferSwapPacketDwords; i < 64u; ++i)
    assert(fixture.address_space->read32_be(ring_va + (10u + i) * 4u) ==
           gpu::make_packet_type2());
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl Vd* GPU control-plane exports...\n";

  test_ordinals_are_registered();
  test_initialize_ring_buffer_converts_log2_size();
  test_enable_ring_buffer_rptr_write_back_stores_address();
  test_set_graphics_interrupt_callback_round_trips();
  test_get_system_command_buffer_writes_exact_sentinels();
  test_query_video_mode_populates_struct();
  test_get_current_display_information_populates_struct();
  test_simple_return_value_exports();
  test_swap_populates_front_buffer();
  test_swap_keeps_the_front_buffer_fetch_constant();
  test_swap_leaves_ring_write_index_to_the_title();
  test_swap_takes_effect_in_command_stream_order();

  std::cout << "All xboxkrnl Vd* GPU control-plane export tests passed!\n";
  return 0;
}
