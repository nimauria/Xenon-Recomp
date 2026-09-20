#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>

#include "xenon/audio/exports.hpp"
#include "xenon/audio/system.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/memory/types.hpp"

using namespace xenon;
using namespace xenon::audio;

namespace {
class FakeBackend final : public AudioBackend {
 public:
  bool open(const AudioBackendConfig&, HostRenderCallback callback,
            std::string*) override {
    callback_ = std::move(callback);
    open_ = true;
    return true;
  }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }
  HostRenderCallback callback_{};
  bool open_{};
};
}

int main() {
  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  AudioSystem audio(memory, std::make_unique<FakeBackend>());
  std::string error;
  assert(audio.initialize(&error));

  core::ExportRegistry registry;
  assert(register_xbox_audio_exports(registry, audio));
  assert(registry.contains("xboxkrnl.exe", 0x1F2));
  assert(registry.contains("xboxkrnl", 0x224));
  assert(registry.contains("xboxkrnl", 0x34C));

  cpu::CpuState cpu{};
  core::ExportCallContext call{cpu, memory, 0, 0};
  auto result = registry.invoke("xboxkrnl", 0x1F2, call);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0);

  cpu::GuestAddress context_out{};
  assert(memory.allocate(4, 4, memory::kReadWrite, false, context_out));
  cpu.gpr[3] = context_out;
  result = registry.invoke("xboxkrnl", 0x224, call);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == 0);
  const auto context = memory.read32_be(context_out);
  assert(context != 0);
  assert(audio.xma().owns_context(context));

  // XMASetLoopData takes a hardware XMA_CONTEXT_DATA pointer. Regression-test
  // the packed 64-byte ABI rather than the compact XMA_CONTEXT_INIT loop tail.
  cpu::GuestAddress loop_context{};
  assert(memory.allocate(kXmaContextBytes, 64, memory::kReadWrite, false,
                         loop_context));
  XmaContextData loop_data{};
  loop_data.loop_start = 0x12345u;
  loop_data.loop_end = 0x23456u;
  loop_data.loop_count = 7u;
  loop_data.loop_subframe_end = 3u;
  loop_data.loop_subframe_skip = 4u;
  std::array<std::byte, kXmaContextBytes> loop_bytes{};
  loop_data.encode(loop_bytes);
  memory.write_bytes(loop_context, loop_bytes);
  cpu.gpr[3] = context;
  cpu.gpr[4] = loop_context;
  result = registry.invoke("xboxkrnl", 0x236, call);
  assert(result.handled && result.success && cpu.gpr[3] == 0);
  XmaContextData loop_result{};
  assert(audio.xma().read_context(context, loop_result));
  assert(loop_result.loop_start == loop_data.loop_start);
  assert(loop_result.loop_end == loop_data.loop_end);
  assert(loop_result.loop_count == loop_data.loop_count);
  assert(loop_result.loop_subframe_end == loop_data.loop_subframe_end);
  assert(loop_result.loop_subframe_skip == loop_data.loop_subframe_skip);

  cpu::GuestAddress callback_pair{};
  cpu::GuestAddress driver_out{};
  assert(memory.allocate(8, 4, memory::kReadWrite, false, callback_pair));
  assert(memory.allocate(4, 4, memory::kReadWrite, false, driver_out));
  memory.write32_be(callback_pair, 0x1000u);
  memory.write32_be(callback_pair + 4, 0x12345678u);
  cpu.gpr[3] = callback_pair;
  cpu.gpr[4] = driver_out;
  result = registry.invoke("xboxkrnl", 0x1F3, call);
  assert(result.handled && result.success && cpu.gpr[3] == 0);
  const auto handle = memory.read32_be(driver_out);
  assert((handle & 0xFFFF0000u) == 0x41550000u);

  cpu.gpr[3] = handle;
  result = registry.invoke("xboxkrnl", 0x1F4, call);
  assert(result.handled && result.success && cpu.gpr[3] == 0);

  cpu.gpr[3] = context;
  result = registry.invoke("xboxkrnl", 0x226, call);
  assert(result.handled && result.success && cpu.gpr[3] == 0);

  audio.shutdown();
  std::cout << "Audio export tests passed\n";
  return 0;
}
