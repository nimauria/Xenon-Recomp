// A program loaded again and again (titles reload the same microcode many
// times a frame; AC6 reloads it for every draw batch) is lowered and
// compiled once. Lowering on every PM4_IM_LOAD kept AC6's GPU command thread
// saturated, holding cutscenes and missions at 2-4 fps.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "xenon/gpu/backend_capabilities.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/shader.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/memory/address_space.hpp"

#if defined(XENON_TEST_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
#include "../../support/vulkan_probe.hpp"
#endif

#if defined(XENON_TEST_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif

namespace {

using namespace xenon::gpu;

// exec_end over four ALU instructions (the same program the lowering tests
// check lowers completely).
ShaderProgram vertex_program(std::uint32_t start_slot) {
  const std::vector<std::uint32_t> words = {
      1u | (4u << 12), std::uint32_t(ControlFlowOpcode::ExecEnd) << 12, 0u,
      62u | (1u << 15) | (1u << 16) | (50u << 26), 0x1Bu | (0xB0u << 8) | (0xC5u << 16),
      40u | (40u << 8) | (17u << 24) | (1u << 31),
      (1u << 20) | (11u << 26), 0x6Cu, (2u << 24) | (7u << 29),
      (4u << 8) | (1u << 20) | (43u << 26), 0x14u,
      9u | (2u << 24) | (1u << 29) | (1u << 30) | (1u << 31),
      (3u << 8) | (1u << 20) | (48u << 26), 0x01u,
      1u | (2u << 8) | (2u << 24) | (7u << 29)};
  return ShaderProgram(ShaderStage::Vertex, words, start_slot);
}

ir::ShaderLoad load_of(const ShaderProgram& program) {
  return ir::ShaderLoad{Type3Opcode::ImLoadImmediate, true, 0u, program,
                        std::make_shared<const DecodedShader>(ShaderDecoder::decode(program)), {}};
}

template <typename Backend>
void run(Backend& backend, const char* name) {
  xenon::memory::AddressSpace memory;
  Edram edram;
  backend.begin_submission(memory, edram);
  const auto program = vertex_program(0);
  const auto first = load_of(program);
  for (int i = 0; i < 8; ++i) backend.consume(ir::Command{first});
  // A separately decoded load of the same program is the same program.
  backend.consume(ir::Command{load_of(program)});
  backend.end_submission();

  auto coverage = backend.shader_coverage();
  assert(coverage.shaders_discovered == 1u);
  assert(coverage.translation_failures == 0u);
  // Handled once: one cache lookup that compiled (or came from disk), and
  // no further lookups for the eight repeats.
  assert(coverage.cache_misses + coverage.cache_disk_hits == 1u);
  assert(coverage.cache_hits == 0u);

  // A different start slot is a different program and is handled too.
  backend.begin_submission(memory, edram);
  backend.consume(ir::Command{load_of(vertex_program(1))});
  backend.consume(ir::Command{load_of(vertex_program(1))});
  backend.end_submission();
  coverage = backend.shader_coverage();
  assert(coverage.shaders_discovered == 2u);
  assert(coverage.cache_hits + coverage.cache_misses + coverage.cache_disk_hits == 2u);
  std::cout << name << ": repeated shader loads lowered and compiled once\n";
}

}  // namespace

int main() {
  bool ran = false;
#if defined(XENON_TEST_VULKAN)
  if (!xenon::test::vulkan_device_present()) {
    std::cout << "Vulkan: no Vulkan device on this machine; skipped\n";
  } else {
    xenon::gpu::vulkan::Backend backend;
    assert(backend.initialize({.enable_validation = false}));
    run(backend, "Vulkan");
    ran = true;
  }
#endif
#if defined(XENON_TEST_D3D12)
  if (!xenon::gpu::discover_backend_capabilities()[1].runtime_available) {
    std::cout << "D3D12: no hardware adapter on this machine; skipped\n";
  } else {
    xenon::gpu::d3d12::Backend backend;
    assert(backend.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run(backend, "D3D12");
    ran = true;
  }
#endif
  if (!ran) std::cout << "no native backend on this machine; skipped\n";
  return 0;
}
