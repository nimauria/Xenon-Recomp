// draw-bench: a deterministic, mission-shaped draw stream through a real
// native backend, for measuring and profiling the host cost of a draw without
// running a game.
//
// Each frame draws `--draws` DMA-indexed triangle lists (`--indices` 16-bit
// big-endian indices each, over a shared vertex buffer fetched by the vertex
// shader) into a 640x360 color + depth target, changing the vertex and pixel
// shader constants between draws as titles do. Most draws use one of
// `--meshes` static index buffers (what a title's meshes are); every frame
// rewrites `--dynamic` of them with the CPU (particles, UI). The frame ends
// with a resolve to guest memory that the guest is made to observe, as a
// title's frame does.
//
// Reports wall time per frame and host microseconds per draw on the
// consuming thread. --profile samples every thread with the host sampler and
// prints its report. --vulkan uses the Vulkan backend (D3D12 is the default
// on Windows).
//
//   draw-bench [--frames N] [--warmup N] [--draws N] [--indices N]
//              [--meshes N] [--dynamic N] [--msaa 1|2|4] [--vulkan] [--profile]

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/logging/host_sampler.hpp"
#include "xenon/memory/address_space.hpp"

#if defined(XENON_DRAW_BENCH_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif
#if defined(XENON_DRAW_BENCH_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
#endif

using namespace xenon::gpu;

namespace {

constexpr std::uint32_t kWidth = 640, kHeight = 360;
constexpr std::uint32_t kVertexBase = 0x00100000u;
// 256 vertices a mesh, each mesh's within a small patch of the target, so
// triangles are mesh-sized rather than screen-sized: the GPU's fill work stays
// that of real geometry and the measurement stays the host's.
constexpr std::uint32_t kVerticesPerMesh = 256;
constexpr std::uint32_t kVertexCount = 65536;
constexpr std::uint32_t kPatchPixels = 24;
constexpr std::uint32_t kIndexBase = 0x00400000u;
constexpr std::uint32_t kIndexStride = 0x4000u;  // per mesh
constexpr std::uint32_t kResolveVertices = 0x00080000u;
constexpr std::uint32_t kResolveDestination = 0x01000000u;
constexpr std::uint32_t kDepthBaseTile = 1024;

struct Options {
  std::uint32_t frames{120};
  std::uint32_t warmup{20};
  std::uint32_t draws{1000};
  std::uint32_t indices{900};
  std::uint32_t meshes{256};
  std::uint32_t dynamic{24};
  std::uint32_t msaa{1};
  bool vulkan{};
  bool profile{};
};

bool parse(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto number = [&](std::uint32_t& out) {
      if (i + 1 >= argc) return false;
      out = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      return true;
    };
    if (arg == "--frames" && number(options.frames)) continue;
    if (arg == "--warmup" && number(options.warmup)) continue;
    if (arg == "--draws" && number(options.draws)) continue;
    if (arg == "--indices" && number(options.indices)) continue;
    if (arg == "--meshes" && number(options.meshes)) continue;
    if (arg == "--dynamic" && number(options.dynamic)) continue;
    if (arg == "--msaa" && number(options.msaa)) continue;
    if (arg == "--vulkan") { options.vulkan = true; continue; }
    if (arg == "--profile") { options.profile = true; continue; }
    std::cerr << "draw-bench: unknown or incomplete option '" << arg << "'\n";
    return false;
  }
  options.indices = (std::max)(3u, options.indices / 3u * 3u);
  options.meshes = (std::max)(1u, options.meshes);
  options.dynamic = (std::min)(options.dynamic, options.meshes);
  if (options.indices * 2u > kIndexStride) {
    std::cerr << "draw-bench: --indices exceeds " << kIndexStride / 2u << "\n";
    return false;
  }
  if (options.msaa != 1 && options.msaa != 2 && options.msaa != 4) {
    std::cerr << "draw-bench: --msaa takes 1, 2 or 4\n";
    return false;
  }
  return true;
}

void write_guest(xenon::memory::AddressSpace& memory, std::uint32_t address, const void* data,
                 std::size_t size) {
  if (!memory.write_physical(address,
                             std::span(static_cast<const std::byte*>(data), size))) {
    std::cerr << "draw-bench: guest write failed\n";
    std::exit(2);
  }
}

ir::ShaderLoad decoded_shader_load(ShaderStage stage, std::uint32_t tag, DecodedShader decoded) {
  const std::vector<std::uint32_t> words{0xC1EA0000u | tag, 0u, 0u};
  ShaderProgram program(stage, words, 0);
  decoded.stage = stage;
  decoded.source_hash = program.hash();
  decoded.complete = true;
  return ir::ShaderLoad{Type3Opcode::ImLoadImmediate, true, 0u, program,
                        std::make_shared<const DecodedShader>(std::move(decoded)), {}};
}

// vfetch r1 = float4 at fetch constant 0; r1 += c0 (a per-draw offset);
// e62 = r1.
DecodedShader vertex_shader() {
  DecodedShader vertex{};
  DecodedControlFlow cf{};
  cf.opcode = ControlFlowOpcode::ExecEnd;
  cf.count = 2;
  vertex.control_flow = {cf};
  DecodedInstruction fetch{};
  fetch.kind = ShaderInstructionKind::VertexFetch;
  fetch.vertex_fetch.destination_register = 1;
  fetch.vertex_fetch.destination_swizzle = 0x688;
  fetch.vertex_fetch.data_format = 38;
  fetch.vertex_fetch.stride_dwords = 4;
  DecodedInstruction position{};
  position.alu.address = 1;
  position.alu.vector_opcode = 0;  // add
  position.alu.scalar_opcode = 50;
  position.alu.vector_destination = 62;
  position.alu.vector_write_mask = 0xF;
  position.alu.export_data = true;
  position.alu.sources = {AluSource{1u, 0xE4u, true}, AluSource{0u, 0xE4u, false},
                          AluSource{1u, 0xE4u, true}};
  vertex.instructions = {fetch, position};
  vertex.reflection.float_constants = {0};
  vertex.reflection.vertex_fetch_constants = {0};
  vertex.reflection.vertex_fetches = {0};
  vertex.reflection.exports = {62};
  vertex.reflection.position_exports = 1;
  vertex.reflection.temporary_register_count = 2;
  return vertex;
}

// oC0 = c0.
DecodedShader pixel_shader() {
  DecodedShader pixel{};
  DecodedControlFlow cf{};
  cf.opcode = ControlFlowOpcode::ExecEnd;
  cf.count = 1;
  pixel.control_flow = {cf};
  DecodedInstruction color{};
  color.alu.vector_opcode = 2;
  color.alu.scalar_opcode = 50;
  color.alu.vector_write_mask = 0xF;
  color.alu.export_data = true;
  for (auto& source : color.alu.sources) source = {0u, 0xE4u, false};
  pixel.instructions = {color};
  pixel.reflection.float_constants = {0};
  pixel.reflection.exports = {0};
  pixel.reflection.color_exports = 1;
  pixel.reflection.temporary_register_count = 1;
  return pixel;
}

template <typename Backend>
void write_register(Backend& backend, std::uint32_t index, std::uint32_t value) {
  backend.consume(ir::Command{ir::RegisterWrite{index, value}});
}

template <typename Backend>
int run(Backend& backend, const Options& options) {
  xenon::memory::AddressSpace memory;
  if (!memory.initialize()) {
    std::cerr << "draw-bench: guest memory could not be initialized\n";
    return 2;
  }
  std::mt19937 random(12345u);
  std::vector<float> vertices(kVertexCount * 4u);
  for (std::uint32_t patch = 0; patch < kVertexCount / kVerticesPerMesh; ++patch) {
    const float x0 = float(random() % (kWidth - kPatchPixels));
    const float y0 = float(random() % (kHeight - kPatchPixels));
    for (std::uint32_t i = 0; i < kVerticesPerMesh; ++i) {
      const auto v = patch * kVerticesPerMesh + i;
      vertices[v * 4u + 0u] = x0 + float(random() % kPatchPixels) - 0.5f;
      vertices[v * 4u + 1u] = y0 + float(random() % kPatchPixels) - 0.5f;
      vertices[v * 4u + 2u] = float(random() % 1000u) / 1000.0f;
      vertices[v * 4u + 3u] = 1.0f;
    }
  }
  write_guest(memory, kVertexBase, vertices.data(), vertices.size() * sizeof(float));
  std::vector<std::uint16_t> indices(options.indices);
  const auto write_mesh = [&](std::uint32_t mesh) {
    const auto first = (mesh % (kVertexCount / kVerticesPerMesh)) * kVerticesPerMesh;
    for (auto& index : indices) {
      const auto value = static_cast<std::uint16_t>(first + random() % kVerticesPerMesh);
      index = static_cast<std::uint16_t>((value << 8u) | (value >> 8u));  // big-endian
    }
    write_guest(memory, kIndexBase + mesh * kIndexStride, indices.data(),
                indices.size() * sizeof(std::uint16_t));
  };
  for (std::uint32_t mesh = 0; mesh < options.meshes; ++mesh) write_mesh(mesh);
  const std::array<float, 6> rectangle{-0.5f, -0.5f, float(kWidth) - 0.5f,
                                       -0.5f, -0.5f, float(kHeight) - 0.5f};
  write_guest(memory, kResolveVertices, rectangle.data(), sizeof(rectangle));
  Edram edram;
  edram.reset();

  const auto vertex_load = decoded_shader_load(ShaderStage::Vertex, 0x701, vertex_shader());
  const auto pixel_load = decoded_shader_load(ShaderStage::Pixel, 0x702, pixel_shader());
  const std::uint32_t msaa_log2 = options.msaa == 4 ? 2u : options.msaa == 2 ? 1u : 0u;

  const auto frame = [&](std::uint32_t index) {
    backend.begin_submission(memory, edram);
    if (index == 0u) {
      backend.consume(ir::Command{vertex_load});
      backend.consume(ir::Command{pixel_load});
    }
    write_register(backend, 0x2000, kWidth | (msaa_log2 << 16u));
    write_register(backend, 0x2001,
                   0u | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
    write_register(backend, 0x2002,
                   kDepthBaseTile | (static_cast<std::uint32_t>(DepthRenderTargetFormat::D24S8)
                                     << 16u));
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, kWidth | (kHeight << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, kWidth | (kHeight << 16u));
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
    write_register(backend, 0x2104, 0xFu);
    write_register(backend, 0x2200, (1u << 1u) | (1u << 2u) | (7u << 4u));
    write_register(backend, 0x2201, 0x00010001u);
    write_register(backend, 0x2204, 1u << 16u);
    write_register(backend, 0x2206, 1u << 10u);
    write_register(backend, 0x2100, 0xFFFFFFu);
    const auto fetch_constant = ResourceStateTracker::kFetchConstantBase;
    write_register(backend, fetch_constant, 3u | kVertexBase);
    write_register(backend, fetch_constant + 1u,
                   static_cast<std::uint32_t>(Endian::None) | ((kVertexCount * 4u) << 2u));
    // Dynamic meshes change every frame, as a title's particles do.
    for (std::uint32_t mesh = 0; mesh < options.dynamic; ++mesh) write_mesh(mesh);
    for (std::uint32_t draw_index = 0; draw_index < options.draws; ++draw_index) {
      const float jitter = float(draw_index % 7u) * 0.25f;
      const std::array<float, 4> offset{jitter, -jitter, 0.0f, 0.0f};
      for (std::uint32_t i = 0; i < 4; ++i)
        write_register(backend, 0x4000u + i, std::bit_cast<std::uint32_t>(offset[i]));
      const std::array<float, 4> color{float(draw_index % 255u) / 255.0f, 0.25f, 0.5f, 1.0f};
      for (std::uint32_t i = 0; i < 4; ++i)
        write_register(backend, 0x4400u + i, std::bit_cast<std::uint32_t>(color[i]));
      ir::DrawPacket draw{};
      draw.opcode = Type3Opcode::DrawIndx2;
      draw.primitive_type = PrimitiveType::TriangleList;
      draw.source = DrawSource::Dma;
      draw.index_format = IndexFormat::UInt16;
      draw.index_count = options.indices;
      draw.index_buffer.valid = true;
      draw.index_buffer.physical_address =
          kIndexBase + (draw_index * 2654435761u % options.meshes) * kIndexStride;
      draw.index_buffer.length_bytes = options.indices * 2u;
      draw.index_buffer.format = IndexFormat::UInt16;
      draw.index_buffer.endian = Endian::Swap8In16;
      draw.vertex_shader = {true, vertex_load.program.hash(), 0};
      draw.pixel_shader = {true, pixel_load.program.hash(), 0};
      backend.consume(ir::Command{std::move(draw)});
    }
    // The frame's resolve, observed by the guest.
    const auto resolve_fetch = ResourceStateTracker::kFetchConstantBase;
    write_register(backend, resolve_fetch, 3u | kResolveVertices);
    write_register(backend, resolve_fetch + 1u, static_cast<std::uint32_t>(Endian::None) | (6u << 2u));
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
    write_register(backend, 0x2318, static_cast<std::uint32_t>(CopyCommand::Raw) << 20u);
    write_register(backend, 0x2319, kResolveDestination);
    write_register(backend, 0x231A, kWidth | (kHeight << 16u));
    write_register(backend, 0x231B, static_cast<std::uint32_t>(Endian128::None) | (6u << 7u));
    ir::DrawPacket copy{};
    copy.opcode = Type3Opcode::DrawIndx2;
    copy.primitive_type = PrimitiveType::TriangleList;
    copy.source = DrawSource::AutoIndex;
    copy.index_count = 3;
    backend.consume(ir::Command{std::move(copy)});
    const bool published = backend.publish_gpu_writes(true);
    backend.end_submission();
    return published && backend.last_error().empty();
  };

  for (std::uint32_t i = 0; i < options.warmup; ++i) {
    if (!frame(i)) {
      std::cerr << "draw-bench: warm-up frame failed: " << backend.last_error() << "\n";
      return 1;
    }
  }
  xenon::logging::HostSampler sampler;
  if (options.profile) {
    xenon::logging::HostSamplerOptions sampling{};
    sampling.min_thread_samples = 20;
    sampling.top_functions = 40;
    (void)sampler.start(sampling);
  }
  const auto gpu_busy_before = perf::peek(perf::Counter::GpuBusyNs);
  const auto gpu_idle_before = perf::peek(perf::Counter::GpuIdleNs);
  const auto fence_before = perf::peek(perf::Counter::FenceWaitNs);
  const auto submits_before = perf::peek(perf::Counter::Submits);
  const auto hits_before = perf::peek(perf::Counter::IndexCacheHits);
  const auto misses_before = perf::peek(perf::Counter::IndexCacheMisses);
  const auto resident_before = perf::peek(perf::Counter::ResidentIndexDraws);
  std::vector<double> frame_ms;
  frame_ms.reserve(options.frames);
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < options.frames; ++i) {
    const auto frame_start = std::chrono::steady_clock::now();
    if (!frame(options.warmup + i)) {
      std::cerr << "draw-bench: frame failed: " << backend.last_error() << "\n";
      return 1;
    }
    frame_ms.push_back(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - frame_start)
                           .count());
  }
  const double total_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (options.profile) sampler.stop();
  std::sort(frame_ms.begin(), frame_ms.end());
  const auto at = [&](double p) {
    return frame_ms[(std::min)(frame_ms.size() - 1u,
                             static_cast<std::size_t>(p * double(frame_ms.size() - 1u)))];
  };
  const double draws = double(options.frames) * options.draws;
  std::cout << "draw-bench (" << (options.vulkan ? "Vulkan" : "D3D12") << "): " << options.frames
            << " frames of " << options.draws << " draws x " << options.indices
            << " indices, msaa " << options.msaa << ", " << options.meshes << " meshes ("
            << options.dynamic << " rewritten per frame)\n"
            << "  frame ms: mean " << total_ms / options.frames << ", p50 " << at(0.5)
            << ", p99 " << at(0.99) << ", max " << frame_ms.back() << "\n"
            << "  host us per draw (consuming thread, wall): " << total_ms * 1000.0 / draws
            << "\n"
            << "  index cache: " << perf::peek(perf::Counter::IndexCacheHits) - hits_before
            << " hits, " << perf::peek(perf::Counter::IndexCacheMisses) - misses_before
            << " misses; resident draws "
            << perf::peek(perf::Counter::ResidentIndexDraws) - resident_before << "\n"
            << "  per frame: GPU busy "
            << double(perf::peek(perf::Counter::GpuBusyNs) - gpu_busy_before) / 1e6 / options.frames
            << " ms, GPU idle between batches "
            << double(perf::peek(perf::Counter::GpuIdleNs) - gpu_idle_before) / 1e6 / options.frames
            << " ms, host fence waits "
            << double(perf::peek(perf::Counter::FenceWaitNs) - fence_before) / 1e6 / options.frames
            << " ms, submissions "
            << double(perf::peek(perf::Counter::Submits) - submits_before) / options.frames
            << "\n";
  if (options.profile) std::cout << "\n" << sampler.report();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  if (!parse(argc, argv, options)) return 2;
  xenon::logging::set_current_thread_name("draw-bench");
  if (options.vulkan) {
#if defined(XENON_DRAW_BENCH_VULKAN)
    xenon::gpu::vulkan::Backend backend;
    if (!backend.initialize({.enable_validation = false})) {
      std::cerr << "draw-bench: Vulkan backend: " << backend.error() << "\n";
      return 2;
    }
    return run(backend, options);
#else
    std::cerr << "draw-bench: built without the Vulkan backend\n";
    return 2;
#endif
  }
#if defined(XENON_DRAW_BENCH_D3D12)
  xenon::gpu::d3d12::Backend backend;
  if (!backend.initialize({.enable_debug_layer = false, .allow_software_adapter = false})) {
    std::cerr << "draw-bench: D3D12 backend: " << backend.error() << "\n";
    return 2;
  }
  return run(backend, options);
#else
  std::cerr << "draw-bench: no default backend on this host; pass --vulkan\n";
  return 2;
#endif
}
