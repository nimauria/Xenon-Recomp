#include <array>
#include <bit>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/backend_capabilities.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/gpu/texture.hpp"
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

constexpr std::uint32_t kResolveVertices = 0x1000;
constexpr std::uint32_t kColorResolveDestination = 0x20000;
constexpr std::uint32_t kDepthResolveDestination = 0x40000;
constexpr std::uint32_t kAliasResolveDestination = 0x60000;
constexpr std::uint32_t kSurfaceBaseTile = 32;
constexpr std::uint32_t kSurfacePitch = 64;
constexpr std::uint32_t kInitialHeight = 16;
constexpr std::uint32_t kGrownHeight = 32;
constexpr std::uint32_t kResolveExtent = 16;
constexpr std::uint32_t kDestinationPitch = 64;
constexpr std::uint32_t kDestinationHeight = 32;
constexpr std::uint32_t kColorClear = 0x11223344u;
constexpr std::uint32_t kDepthClear = 0x55667788u;
// A clear in the upper half of the depth range (code 0xE6D5C4, near the far
// plane, where most scenes' depth lies): above 2^23 a device converting on a
// 2^24 scale and one converting on the specified 2^24 - 1 scale need
// different depth writes for the same code.
constexpr std::uint32_t kFarDepthClear = 0xE6D5C4A5u;

void write_guest_bytes(xenon::memory::AddressSpace& memory,
                       std::uint32_t address, const void* source,
                       std::size_t size) {
  const auto bytes = std::span<const std::byte>(
      static_cast<const std::byte*>(source), size);
  assert(memory.write_physical(address, bytes));
}

std::uint32_t read_guest_word(const xenon::memory::AddressSpace& memory,
                              std::uint32_t address) {
  std::uint32_t value{};
  const auto* source = memory.physical_data(address);
  assert(source);
  std::memcpy(&value, source, sizeof(value));
  return value;
}

template <typename Backend>
void write_register(Backend& backend, std::uint32_t index,
                    std::uint32_t value) {
  backend.consume(ir::Command{ir::RegisterWrite{index, value}});
}

template <typename Backend>
void set_scissor_height(Backend& backend, std::uint32_t height) {
  write_register(backend, 0x200E, 0u);
  write_register(backend, 0x200F, kSurfacePitch | (height << 16u));
  // The window scissor (offset disabled) is the render target, as Direct3D sets it.
  write_register(backend, 0x2081, 1u << 31u);
  write_register(backend, 0x2082, kSurfacePitch | (height << 16u));
}

template <typename Backend>
void set_common_surface(Backend& backend, std::uint32_t height) {
  write_register(backend, 0x2000, kSurfacePitch);
  set_scissor_height(backend, height);
}

template <typename Backend>
void set_color_draw_state(Backend& backend, std::uint32_t height) {
  set_common_surface(backend, height);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::ColorDepth));
  write_register(backend, 0x2104, 0xFu);
  write_register(backend, 0x2001,
                 kSurfaceBaseTile |
                     (static_cast<std::uint32_t>(
                          ColorRenderTargetFormat::R8G8B8A8)
                      << 16u));
  write_register(backend, 0x2200, 0u);
}

template <typename Backend>
void set_depth_draw_state(Backend& backend, std::uint32_t height) {
  set_common_surface(backend, height);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::DepthOnly));
  write_register(backend, 0x2104, 0u);
  write_register(backend, 0x2002,
                 kSurfaceBaseTile |
                     (static_cast<std::uint32_t>(
                          DepthRenderTargetFormat::D24S8)
                      << 16u));
  write_register(backend, 0x2200, (1u << 1u) | (1u << 2u));
}

template <typename Backend>
void set_resolve_fetch(Backend& backend) {
  const auto fetch = ResourceStateTracker::kFetchConstantBase;
  write_register(backend, fetch, 3u | kResolveVertices);
  write_register(backend, fetch + 1u,
                 static_cast<std::uint32_t>(Endian::None) | (6u << 2u));
}

template <typename Backend>
void set_resolve_destination(Backend& backend, std::uint32_t address,
                             std::uint8_t format,
                             Endian128 endian = Endian128::None,
                             std::int8_t exponent_bias = 0,
                             bool red_blue_swap = false) {
  write_register(backend, 0x2319, address);
  write_register(backend, 0x231A,
                 kDestinationPitch | (kDestinationHeight << 16u));
  const auto bias = static_cast<std::uint32_t>(exponent_bias) & 0x3Fu;
  write_register(backend, 0x231B,
                 static_cast<std::uint32_t>(endian) |
                     (std::uint32_t(format) << 7u) | (bias << 16u) |
                     (std::uint32_t(red_blue_swap) << 24u));
}

template <typename Backend>
void issue_dummy_draw(Backend& backend) {
  ir::DrawPacket draw{};
  draw.opcode = Type3Opcode::DrawIndx2;
  draw.primitive_type = PrimitiveType::TriangleList;
  draw.source = DrawSource::AutoIndex;
  draw.index_count = 3;
  backend.consume(ir::Command{std::move(draw)});
}

// The guest learns the GPU got past a resolve through a fence or an
// interrupt; the command processor publishes the resolve's queued readback
// before either (Backend::publish_gpu_writes()), so the resolved bytes are in
// guest memory by the time the guest can look.
template <typename Backend>
void guest_observes_resolve(Backend& backend) {
  assert(backend.publish_gpu_writes(true));
}

template <typename Backend>
void issue_color_resolve(Backend& backend, std::uint32_t destination,
                         bool clear_after) {
  set_common_surface(backend, kGrownHeight);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318,
                 (clear_after ? (1u << 8u) : 0u) |
                     (static_cast<std::uint32_t>(CopyCommand::Raw) << 20u));
  set_resolve_destination(backend, destination, 6);
  if (clear_after) {
    write_register(backend, 0x231E, kColorClear);
    write_register(backend, 0x231F, 0u);
  }
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
}

template <typename Backend>
void issue_depth_resolve(Backend& backend, std::uint32_t destination,
                         bool clear_after, std::uint32_t depth_clear = kDepthClear) {
  set_common_surface(backend, kGrownHeight);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::Copy));
  // Use Convert plus deliberately color-style destination controls. The common
  // depth path must override these and preserve the exact D24S8 word.
  write_register(backend, 0x2318,
                 4u | (clear_after ? (1u << 9u) : 0u) |
                     (static_cast<std::uint32_t>(CopyCommand::Convert) << 20u));
  set_resolve_destination(backend, destination, 6, Endian128::None, 7, true);
  if (clear_after) write_register(backend, 0x231D, depth_clear);
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
}

std::uint32_t resolved_word_at(const xenon::memory::AddressSpace& memory,
                               std::uint32_t base, std::uint32_t x,
                               std::uint32_t y) {
  const auto offset = tiled_offset_2d(x, y, kDestinationPitch, 4);
  return read_guest_word(memory,
                         base + static_cast<std::uint32_t>(offset));
}

template <typename Backend>
void assert_backend_ok(const Backend& backend, std::string_view phase) {
  if (!backend.error().empty()) {
    std::cerr << phase << ": " << backend.error() << '\n';
  }
  assert(backend.error().empty());
  assert(backend.ready());
}

template <typename Backend>
void run_backend_ownership_alias_resolve_clear(Backend& backend,
                                                std::string_view name,
                                                std::uint32_t depth_clear = kDepthClear) {
  // Regression (CI: WARP and llvmpipe stored the post-resolve clear of
  // 0x556677 as 0x556676): the backend measured how this device stores D24
  // depth writes and found an encoding that stores every code exactly.
  const auto calibration = backend.unorm24_depth_calibration();
  assert(calibration && calibration->exact);
  std::cout << name << " D24 depth writes: "
            << xenon::gpu::unorm24_depth_write_name(calibration->write) << std::endl;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());

  const std::array<float, 6> resolve_vertices{
      -0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_vertices.data(),
                    sizeof(resolve_vertices));

  Edram edram;
  edram.reset();
  const EdramSurfaceLayout color_surface{
      kSurfaceBaseTile, kSurfacePitch, kGrownHeight, MsaaSamples::X1, false,
      false};

  // Start with non-uniform canonical data so the first real native owner must
  // import meaningful EDRAM rather than an all-zero surface.
  for (std::uint32_t y = 0; y < kGrownHeight; ++y) {
    for (std::uint32_t x = 0; x < kSurfacePitch; ++x) {
      const std::uint32_t value =
          0xA5000000u | ((y & 0xFFu) << 8u) | (x & 0xFFu);
      assert(write_edram_sample(edram, color_surface, x, y, {value, 0u}));
    }
  }

  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");

  // Acquire a real production color owner through Backend::consume(), then
  // increase the drawable height. This enters the target-growth branch which
  // flushes the old owner before replacing the native image.
  set_color_draw_state(backend, kInitialHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "initial color ownership");
  assert(backend.realized_render_target_count() == 1);

  set_color_draw_state(backend, kGrownHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "color target growth");
  assert(backend.realized_render_target_count() == 1);

  // Resolve the real color target and request the Xenos post-copy color clear.
  // The native owner holds every tile, so both run on the GPU in that image:
  // the resolved words reach guest memory, and the clear reaches EDRAM once
  // the owner hands its tiles back. Only the resolve region is cleared.
  const auto gpu_resolves = backend.performance_counters().gpu_resolves;
  issue_color_resolve(backend, kColorResolveDestination, true);
  assert_backend_ok(backend, "color resolve and clear");
  assert(backend.performance_counters().gpu_resolves == gpu_resolves + 1u);
  assert(resolved_word_at(memory, kColorResolveDestination, 0, 0) ==
         0xA5000000u);
  assert(backend.make_edram_canonical());
  assert(read_edram_sample(edram, color_surface, 0, 0)[0] == kColorClear);
  assert(read_edram_sample(edram, color_surface, 15, 15)[0] == kColorClear);
  assert(read_edram_sample(edram, color_surface, 16, 0)[0] == 0xA5000010u);

  // Reacquire the cleared bytes as a native color target, then request an
  // overlapping depth owner at the exact same tiles. The ownership tracker must
  // flush color, canonicalize the alias, and upload the bit-identical D24S8
  // representation to the depth image.
  set_color_draw_state(backend, kGrownHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "color reacquire after clear");

  set_depth_draw_state(backend, kGrownHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "color to depth alias transfer");

  // Resolve from the real depth owner. Xenos depth tiles exchange their two
  // 40-sample halves, so depth X=0 aliases color X=40 rather than color X=0.
  // The destination deliberately asks for a color conversion, but depth
  // resolves must still write the exact packed D24S8 bits. The depth image
  // owns every tile, so the resolve and the clear after it both run on the GPU
  // in that image; the clear reaches EDRAM when the owner hands its tiles back.
  const auto depth_counters = backend.performance_counters();
  issue_depth_resolve(backend, kDepthResolveDestination, true, depth_clear);
  assert_backend_ok(backend, "depth resolve and clear");
  assert(backend.performance_counters().gpu_depth_resolves ==
         depth_counters.gpu_depth_resolves + 1u);
  assert(backend.performance_counters().gpu_depth_clears == depth_counters.gpu_depth_clears + 1u);
  const auto depth_resolved =
      resolved_word_at(memory, kDepthResolveDestination, 0, 0);
  constexpr std::uint32_t kColorAtDepthZero = 0xA5000028u;
  if (depth_resolved != kColorAtDepthZero) {
    std::cerr << name << " depth alias resolved 0x" << std::hex
              << depth_resolved << ", expected 0x" << kColorAtDepthZero
              << std::dec << '\n';
  }
  assert(depth_resolved == kColorAtDepthZero);

  const EdramSurfaceLayout depth_surface{
      kSurfaceBaseTile, kSurfacePitch, kGrownHeight, MsaaSamples::X1, false,
      true};
  assert(backend.make_edram_canonical());
  if (const auto cleared = read_edram_sample(edram, depth_surface, 0, 0)[0];
      cleared != depth_clear) {
    std::cerr << name << " post-resolve depth clear left 0x" << std::hex << cleared
              << ", expected 0x" << depth_clear << std::dec << '\n';
  }
  assert(read_edram_sample(edram, depth_surface, 0, 0)[0] == depth_clear);
  assert(read_edram_sample(edram, depth_surface, 15, 15)[0] == depth_clear);
  assert(read_edram_sample(edram, depth_surface, 16, 0)[0] == 0xA5000038u);
  assert(read_edram_sample(edram, color_surface, 40, 0)[0] == depth_clear);

  // Transfer the modified depth alias back into the existing color target and
  // resolve it again. Seeing kDepthClear at the guest-memory boundary proves
  // depth -> canonical EDRAM -> color ownership preserved the exact bits.
  set_color_draw_state(backend, kGrownHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "depth to color alias transfer");

  const std::array<float, 6> depth_alias_vertices{
      39.5f, -0.5f, 55.5f, -0.5f, 39.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, depth_alias_vertices.data(),
                    sizeof(depth_alias_vertices));
  issue_color_resolve(backend, kAliasResolveDestination, false);
  assert_backend_ok(backend, "color resolve after depth alias");
  assert(resolved_word_at(memory, kAliasResolveDestination, 40, 0) ==
          depth_clear);
  assert(resolved_word_at(memory, kAliasResolveDestination, 55, 15) ==
          depth_clear);

  backend.end_submission();
  std::cout << name
            << " Backend::consume ownership/alias/resolve integration: ok\n";
}

// Regression (Ace Combat 6): a resolve whose surface no native render target
// matches - nothing drew through that exact view, or only a depth view of the
// same tiles did - was abandoned ("resolve source EDRAM surface is
// unavailable"), so AC6's front buffer was never written. On Xenos a resolve
// reads EDRAM itself: the result is whatever the tiles hold.
template <typename Backend>
void run_resolve_without_a_matching_render_target(Backend& backend,
                                                  std::string_view name) {
  constexpr std::uint32_t kCanonicalDestination = 0x80000;
  constexpr std::uint32_t kDepthOwnedDestination = 0xA0000;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 6> resolve_vertices{
      -0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_vertices.data(),
                    sizeof(resolve_vertices));
  Edram edram;
  edram.reset();
  const EdramSurfaceLayout color_surface{
      kSurfaceBaseTile, kSurfacePitch, kGrownHeight, MsaaSamples::X1, false, false};
  for (std::uint32_t y = 0; y < kGrownHeight; ++y) {
    for (std::uint32_t x = 0; x < kSurfacePitch; ++x) {
      assert(write_edram_sample(edram, color_surface, x, y,
                                {0x5A000000u | (y << 8u) | x, 0u}));
    }
  }
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");

  // No draw has bound this color surface: the resolve reads canonical EDRAM,
  // and its post-copy clear lands there.
  write_register(backend, 0x2001,
                 kSurfaceBaseTile |
                     (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  issue_color_resolve(backend, kCanonicalDestination, true);
  assert_backend_ok(backend, "resolve with no native color target");
  assert(backend.realized_render_target_count() == 0);
  assert(resolved_word_at(memory, kCanonicalDestination, 0, 0) == 0x5A000000u);
  assert(resolved_word_at(memory, kCanonicalDestination, 15, 15) == 0x5A000F0Fu);
  assert(read_edram_sample(edram, color_surface, 15, 15)[0] == kColorClear);
  assert(read_edram_sample(edram, color_surface, 16, 0)[0] == 0x5A000010u);

  // A depth image owns the tiles; a color resolve of them (no color target)
  // takes them back from it. Depth X=0 aliases color X=40, and the round
  // trip through the depth image keeps the bits.
  set_depth_draw_state(backend, kGrownHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "depth ownership");
  const std::array<float, 6> alias_vertices{
      39.5f, -0.5f, 55.5f, -0.5f, 39.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, alias_vertices.data(), sizeof(alias_vertices));
  write_register(backend, 0x2001,
                 kSurfaceBaseTile |
                     (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  issue_color_resolve(backend, kDepthOwnedDestination, false);
  assert_backend_ok(backend, "color resolve of depth-owned tiles");
  assert(resolved_word_at(memory, kDepthOwnedDestination, 40, 0) == 0x5A000028u);
  backend.end_submission();
  std::cout << name << " resolve without a matching render target: ok\n";
}

// AC6's frame: a scene drawn at 640x360 with 4x MSAA is resolved as a
// 1280x720 single-sample surface over the same EDRAM, converted, byte
// swapped and cleared. The resolve runs on the GPU straight out of the MSAA
// image: canonical EDRAM is not touched until the owner hands its tiles back,
// guest memory gets the converted words, and the clear lands in the image.
template <typename Backend>
void run_gpu_resolve_of_msaa_alias(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kMsaaPitch = 80;   // pixels, 4x MSAA
  constexpr std::uint32_t kMsaaHeight = 16;
  constexpr std::uint32_t kViewPitch = 160;  // the same samples as 1x pixels
  constexpr std::uint32_t kViewHeight = 32;
  constexpr std::uint32_t kResolveRight = 96;
  constexpr std::uint32_t kDestination = 0x00300000u;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 6> rectangle{-0.5f, -0.5f, float(kResolveRight) - 0.5f,
                                       -0.5f, -0.5f, float(kViewHeight) - 0.5f};
  write_guest_bytes(memory, kResolveVertices, rectangle.data(), sizeof(rectangle));
  Edram edram;
  edram.reset();
  const EdramSurfaceLayout view{kSurfaceBaseTile, kViewPitch, kViewHeight,
                                MsaaSamples::X1, false, false};
  const auto seeded = [](std::uint32_t x, std::uint32_t y) {
    return 0xC3000000u | (y << 8u) | x;  // R = x, G = y, B = 0, A = 0xC3
  };
  for (std::uint32_t y = 0; y < kViewHeight; ++y)
    for (std::uint32_t x = 0; x < kViewPitch; ++x)
      assert(write_edram_sample(edram, view, x, y, {seeded(x, y), 0u}));
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");

  const auto set_surface = [&](std::uint32_t pitch, std::uint32_t height, std::uint32_t msaa) {
    write_register(backend, 0x2000, pitch | (msaa << 16u));
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, pitch | (height << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, pitch | (height << 16u));
    write_register(backend, 0x2001,
                   kSurfaceBaseTile |
                       (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  };
  // The 4x MSAA scene takes the tiles.
  set_surface(kMsaaPitch, kMsaaHeight, 2u);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
  write_register(backend, 0x2104, 0xFu);
  write_register(backend, 0x2200, 0u);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "4x MSAA owner");

  // Resolve it as the 1x surface: Convert to k_8_8_8_8 with red/blue swapped,
  // 8-in-32 endian, then clear.
  const auto before = backend.performance_counters().gpu_resolves;
  set_surface(kViewPitch, kViewHeight, 0u);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318,
                 (1u << 8u) | (static_cast<std::uint32_t>(CopyCommand::Convert) << 20u));
  write_register(backend, 0x2319, kDestination);
  write_register(backend, 0x231A, kViewPitch | (kViewHeight << 16u));
  write_register(backend, 0x231B,
                 static_cast<std::uint32_t>(Endian128::Swap8In32) | (6u << 7u) | (1u << 24u));
  write_register(backend, 0x231E, kColorClear);
  write_register(backend, 0x231F, 0u);
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
  assert_backend_ok(backend, "GPU resolve");
  if (backend.performance_counters().gpu_resolves != before + 1u) {
    std::cerr << name << ": the resolve went through canonical EDRAM, not the GPU\n";
    assert(false);
  }
  // No flush happened: canonical EDRAM still has what was seeded.
  assert(read_edram_sample(edram, view, 0, 0)[0] == seeded(0, 0));

  for (std::uint32_t y = 0; y < kViewHeight; ++y) {
    for (std::uint32_t x = 0; x < kResolveRight; ++x) {
      const auto swapped = (seeded(x, y) & 0xFF00FF00u) | (x << 16u);  // B = x, R = 0
      const auto expected = ((swapped & 0xFFu) << 24u) | ((swapped & 0xFF00u) << 8u) |
                            ((swapped >> 8u) & 0xFF00u) | (swapped >> 24u);
      const auto word = read_guest_word(
          memory, kDestination + static_cast<std::uint32_t>(tiled_offset_2d(x, y, kViewPitch, 4)));
      if (word != expected) {
        std::cerr << name << " GPU resolve wrote 0x" << std::hex << word << " at (" << std::dec
                  << x << ',' << y << "), expected 0x" << std::hex << expected << std::dec << '\n';
        assert(false);
      }
    }
  }

  // The clear covers the rectangle's samples only; the rest keep the scene.
  assert(backend.make_edram_canonical());
  for (const auto [x, y] : {std::pair{0u, 0u}, std::pair{95u, 31u}, std::pair{40u, 17u}})
    assert(read_edram_sample(edram, view, x, y)[0] == kColorClear);
  for (const auto [x, y] : {std::pair{96u, 0u}, std::pair{159u, 31u}})
    assert(read_edram_sample(edram, view, x, y)[0] == seeded(x, y));
  backend.end_submission();
  std::cout << name << " GPU resolve of a 4x MSAA alias: ok\n";
}

// Regression (AC6 kill-cam and radio-portrait window torn into bands, and,
// once clears stopped taking all of EDRAM, the top of the mission screen):
// AC6 draws the window at pitch 240 with 4x MSAA over the tiles its 1280x720
// scene uses at pitch 640 with 2x MSAA. EDRAM is raw memory: whatever a
// surface of one layout leaves in its tiles, a surface of another layout
// over the same tiles must hand back sample for sample. The tiles go from a
// pitch-240 4x surface to a pitch-640 2x one and back, and canonical EDRAM
// must still hold exactly what was there.
template <typename Backend>
void run_tiles_round_trip_between_layouts(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kWindowPitch = 240, kWindowHeight = 144;  // 4x MSAA
  constexpr std::uint32_t kScenePitch = 640, kSceneHeight = 112;    // 2x MSAA, tiles 0-111
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  Edram edram;
  edram.reset();
  // The window's samples seen as 1x pixels: 4x is 2x2 samples a pixel.
  const EdramSurfaceLayout samples{0, kWindowPitch * 2u, kWindowHeight * 2u, MsaaSamples::X1,
                                   false, false};
  const auto seeded = [](std::uint32_t x, std::uint32_t y) {
    return 0x5A000000u ^ (y * 0x10001u) ^ (x * 0x0101u) ^ (x << 20u);
  };
  for (std::uint32_t y = 0; y < samples.height_pixels; ++y)
    for (std::uint32_t x = 0; x < samples.pitch_pixels; ++x)
      assert(write_edram_sample(edram, samples, x, y, {seeded(x, y), 0u}));
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");
  const auto own = [&](std::uint32_t pitch, std::uint32_t height, std::uint32_t msaa,
                       std::string_view phase) {
    write_register(backend, 0x2000, pitch | (msaa << 16u));
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, pitch | (height << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, pitch | (height << 16u));
    write_register(backend, 0x2001,
                   0u | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
    write_register(backend, 0x2104, 0xFu);
    write_register(backend, 0x2200, 0u);
    issue_dummy_draw(backend);
    assert_backend_ok(backend, phase);
  };
  own(kWindowPitch, kWindowHeight, 2u, "pitch-240 4x surface takes the tiles");
  own(kScenePitch, kSceneHeight, 1u, "pitch-640 2x surface takes them");
  own(kWindowPitch, kWindowHeight, 2u, "pitch-240 4x surface takes them back");
  assert(backend.make_edram_canonical());
  std::uint32_t wrong = 0;
  for (std::uint32_t y = 0; y < samples.height_pixels; ++y)
    for (std::uint32_t x = 0; x < samples.pitch_pixels; ++x) {
      const auto word = read_edram_sample(edram, samples, x, y)[0];
      if (word == seeded(x, y)) continue;
      if (wrong++ < 8)
        std::cerr << name << ": sample (" << x << ',' << y << ") holds 0x" << std::hex << word
                  << ", not 0x" << seeded(x, y) << std::dec << '\n';
    }
  if (wrong) {
    std::cerr << name << ": " << wrong << " of " << samples.pitch_pixels * samples.height_pixels
              << " samples changed crossing between EDRAM layouts\n";
    assert(false);
  }
  backend.end_submission();
  std::cout << name << " EDRAM tiles round-trip between pitch-240 4x and pitch-640 2x: ok\n";
}

// Regression (AC6 missions at 7-12 fps): two kinds of resolve every mission
// frame fell back to the CPU path - a readback and a GPU stall each - the
// 640x360 16_16_FLOAT target copied to k_16_16_FLOAT ("destination format has
// no GPU resolve codec") and 128/64/32-pixel copies reaching past their
// destination pitch ("resolve rectangle is wider than its destination"). The
// GPU now runs both and writes exactly the bytes the CPU path writes: half
// floats packed as float_to_half() packs them (signaling and quiet NaNs,
// infinity, denormals, overflow under an exponent bias), and columns past the
// pitch where tiled addressing puts them.
template <typename Backend>
void run_gpu_resolve_matches_the_cpu_path(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kPitch = 160, kHeight = 32, kRight = 96;
  constexpr std::uint32_t kDestination = 0x00400000u;
  struct Case {
    const char* label;
    ColorRenderTargetFormat format;
    std::uint8_t destination_format;
    std::uint16_t destination_pitch;
    std::int8_t exponent_bias;
    bool red_blue_swap;
    Endian128 endian;
    bool with_nans;  // NaN payloads are exact only for an unscaled copy
  };
  constexpr std::array<Case, 4> cases{{
      {"16_16_FLOAT -> k_16_16_FLOAT past its pitch", ColorRenderTargetFormat::R16G16Float, 31, 64,
       0, false, Endian128::Swap8In32, true},
      {"16_16_FLOAT -> k_16_16_16_16_FLOAT, exponent bias -1",
       ColorRenderTargetFormat::R16G16Float, 32, 128, -1, false, Endian128::None, false},
      {"16_16_16_16_FLOAT -> k_16_16_FLOAT, red/blue swap, bias +2",
       ColorRenderTargetFormat::R16G16B16A16Float, 31, 128, 2, true, Endian128::Swap8In16, false},
      {"8_8_8_8 -> k_8_8_8_8 past its pitch", ColorRenderTargetFormat::R8G8B8A8, 6, 32, 0, false,
       Endian128::None, false},
  }};
  // Halves: zeros, denormals, normals, the largest finite, infinities, NaNs.
  constexpr std::array<std::uint16_t, 14> kHalves{
      0x0000u, 0x8000u, 0x0001u, 0x03FFu, 0x0400u, 0x3C00u, 0xBC00u,
      0x3555u, 0x7BFFu, 0xFBFFu, 0x7C00u, 0xFC00u, 0x7E00u, 0x7D55u};
  for (std::uint32_t case_index = 0; case_index < cases.size(); ++case_index) {
    const auto& test_case = cases[case_index];
    // Each case on tiles of its own: the backend keeps the earlier cases'
    // images, which still own theirs.
    const auto base_tile = kSurfaceBaseTile + 32u * case_index;
    const bool wide = test_case.format == ColorRenderTargetFormat::R16G16B16A16Float;
    const bool half = test_case.format != ColorRenderTargetFormat::R8G8B8A8;
    const auto halves = test_case.with_nans ? kHalves.size() : kHalves.size() - 2u;
    const auto half_at = [&](std::uint32_t i) { return std::uint32_t(kHalves[i % halves]); };
    xenon::memory::AddressSpace memory;
    assert(memory.initialize());
    xenon::memory::AddressSpace reference;
    assert(reference.initialize());
    const std::array<float, 6> rectangle{-0.5f, -0.5f, float(kRight) - 0.5f,
                                         -0.5f, -0.5f, float(kHeight) - 0.5f};
    write_guest_bytes(memory, kResolveVertices, rectangle.data(), sizeof(rectangle));
    Edram edram;
    edram.reset();
    const EdramSurfaceLayout surface{base_tile, kPitch, kHeight, MsaaSamples::X1, wide, false};
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kPitch; ++x) {
        const auto i = x * 7u + y * 13u;
        const std::array<std::uint32_t, 2> words =
            half ? std::array<std::uint32_t, 2>{half_at(i) | (half_at(i + 3u) << 16u),
                                                half_at(i + 5u) | (half_at(i + 9u) << 16u)}
                 : std::array<std::uint32_t, 2>{0x01000000u * (x & 0xFFu) + (y << 8u) + x, 0u};
        assert(write_edram_sample(edram, surface, x, y, words));
      }
    }
    // What the CPU path writes: the canonical words in host storage, then
    // write_converted_resolve().
    CopyResolveState copy{};
    copy.destination_base = kDestination;
    copy.destination_pitch = test_case.destination_pitch;
    copy.destination_height = kHeight;
    copy.command = CopyCommand::Convert;
    copy.destination_endian = test_case.endian;
    copy.destination_format = test_case.destination_format;
    copy.destination_exponent_bias = test_case.exponent_bias;
    copy.destination_red_blue_swap = test_case.red_blue_swap;
    const ResolveRectangle resolved{0, 0, std::int32_t(kRight), std::int32_t(kHeight), true};
    const auto edram_bytes = wide ? 8u : 4u;
    std::vector<std::byte> canonical(std::size_t(kRight) * kHeight * edram_bytes);
    assert(resolve_edram_raw(edram, surface, 0, 0, kRight, kHeight, 0, canonical,
                             kRight * edram_bytes));
    const auto host_bytes = color_host_bytes_per_pixel(test_case.format);
    std::vector<std::byte> host(std::size_t(kRight) * kHeight * host_bytes);
    assert(edram_color_to_host(test_case.format, kRight, kHeight, canonical, kRight * edram_bytes,
                               host, kRight * host_bytes));
    const auto expected = write_converted_resolve(copy, test_case.format, resolved, host,
                                                  kRight * host_bytes, reference);
    assert(expected.valid && expected.modified_size);

    backend.begin_submission(memory, edram);
    assert_backend_ok(backend, "begin_submission");
    // A draw makes a native image the owner of the tiles.
    write_register(backend, 0x2000, kPitch);
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, kPitch | (kHeight << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, kPitch | (kHeight << 16u));
    write_register(backend, 0x2001,
                   base_tile | (static_cast<std::uint32_t>(test_case.format) << 16u));
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
    write_register(backend, 0x2104, 0xFu);
    write_register(backend, 0x2200, 0u);
    issue_dummy_draw(backend);
    assert_backend_ok(backend, test_case.label);

    const auto before = backend.performance_counters().gpu_resolves;
    set_resolve_fetch(backend);
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
    write_register(backend, 0x2318, static_cast<std::uint32_t>(CopyCommand::Convert) << 20u);
    write_register(backend, 0x2319, kDestination);
    write_register(backend, 0x231A, test_case.destination_pitch | (kHeight << 16u));
    write_register(backend, 0x231B,
                   static_cast<std::uint32_t>(test_case.endian) |
                       (std::uint32_t(test_case.destination_format) << 7u) |
                       ((static_cast<std::uint32_t>(test_case.exponent_bias) & 0x3Fu) << 16u) |
                       (std::uint32_t(test_case.red_blue_swap) << 24u));
    issue_dummy_draw(backend);
    guest_observes_resolve(backend);
    assert_backend_ok(backend, test_case.label);
    if (backend.performance_counters().gpu_resolves != before + 1u) {
      std::cerr << name << " " << test_case.label << ": resolved on the CPU path, not the GPU\n";
      assert(false);
    }
    const auto* gpu = memory.physical_data(expected.modified_address);
    const auto* cpu = reference.physical_data(expected.modified_address);
    assert(gpu && cpu);
    for (std::uint32_t i = 0; i < expected.modified_size; ++i) {
      if (gpu[i] == cpu[i]) continue;
      // The 16-byte block around it on each path, and the source pixel it
      // came from.
      const auto block = i & ~15u;
      std::cerr << name << " " << test_case.label << ": byte 0x" << std::hex
                << expected.modified_address + i << " differs\n  GPU:";
      for (std::uint32_t b = block; b < block + 16u && b < expected.modified_size; ++b)
        std::cerr << ' ' << std::to_integer<unsigned>(gpu[b]);
      std::cerr << "\n  CPU:";
      for (std::uint32_t b = block; b < block + 16u && b < expected.modified_size; ++b)
        std::cerr << ' ' << std::to_integer<unsigned>(cpu[b]);
      std::cerr << std::dec << '\n';
      for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kRight; ++x) {
          const auto offset = tiled_offset_2d(x, y, (copy.destination_pitch + 31u) & ~31u,
                                              texture_format_info(copy.destination_format)
                                                  .bytes_per_block());
          if (std::uint64_t(kDestination) + offset > expected.modified_address + i ||
              std::uint64_t(kDestination) + offset + 8u <= expected.modified_address + i)
            continue;
          std::cerr << "  pixel (" << x << ',' << y << ") source host bytes:" << std::hex;
          for (std::uint32_t b = 0; b < host_bytes; ++b)
            std::cerr << ' '
                      << std::to_integer<unsigned>(host[(std::size_t(y) * kRight + x) * host_bytes + b]);
          std::cerr << std::dec << '\n';
        }
      assert(false);
    }
    backend.end_submission();
    std::cout << name << " GPU resolve " << test_case.label << " matches the CPU path: ok\n";
  }
}

// A shader load of a hand-built decoded shader; the program words only give
// it a hash of its own.
ir::ShaderLoad decoded_shader_load(ShaderStage stage, std::uint32_t tag, DecodedShader decoded) {
  const std::vector<std::uint32_t> words{0xC1EA0000u | tag, 0u, 0u};
  ShaderProgram program(stage, words, 0);
  decoded.stage = stage;
  decoded.source_hash = program.hash();
  decoded.complete = true;
  return ir::ShaderLoad{Type3Opcode::ImLoadImmediate, true, 0u, program,
                        std::make_shared<const DecodedShader>(std::move(decoded)), {}};
}

// vfetch r1 = float4 at fetch constant 0; e62 = max(r1, r1).
DecodedShader fetch_position_vertex_shader() {
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
  position.alu.vector_opcode = 2;
  position.alu.scalar_opcode = 50;
  position.alu.vector_destination = 62;
  position.alu.vector_write_mask = 0xF;
  position.alu.export_data = true;
  for (auto& source : position.alu.sources) source = {1u, 0xE4u, true};
  vertex.instructions = {fetch, position};
  vertex.reflection.vertex_fetch_constants = {0};
  vertex.reflection.vertex_fetches = {0};
  vertex.reflection.exports = {62};
  vertex.reflection.position_exports = 1;
  vertex.reflection.temporary_register_count = 2;
  return vertex;
}

// oC0 = c0.
DecodedShader constant_color_pixel_shader() {
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

// GPU V3 shader-variant prefetch: a draw over D24FS8 depth with the depth
// test on samples its pixel shader's float20e4 depth-output variant, which
// the frontend predicts from the IR it hands over (Backend::
// prefetch_shaders()) and starts on a compile worker - the draw finds it
// prepared instead of lowering and compiling it on the backend thread. A
// draw state the frontend has seen is not predicted again, and without the
// prediction a draw still compiles its variant itself.
template <typename Backend>
void run_shader_variant_prefetch(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kPitch = 240, kHeight = 64, kVertices = 0x80000;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 12> triangle{-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f,
                                       0.0f,  1.0f,  0.0f, 0.5f, 0.0f, 1.0f};
  write_guest_bytes(memory, kVertices, triangle.data(), sizeof(triangle));
  Edram edram;
  edram.reset();
  const auto vertex_load =
      decoded_shader_load(ShaderStage::Vertex, 41, fetch_position_vertex_shader());
  const auto pixel_load =
      decoded_shader_load(ShaderStage::Pixel, 42, constant_color_pixel_shader());
  const auto make_draw = [&] {
    ir::DrawPacket packet{};
    packet.opcode = Type3Opcode::DrawIndx2;
    packet.primitive_type = PrimitiveType::TriangleList;
    packet.source = DrawSource::AutoIndex;
    packet.index_count = 3;
    packet.vertex_shader = {true, vertex_load.program.hash(), 0};
    packet.pixel_shader = {true, pixel_load.program.hash(), 0};
    return packet;
  };

  std::vector<ir::Command> commands;
  const auto reg = [&](std::uint32_t index, std::uint32_t value) {
    commands.push_back(ir::RegisterWrite{index, value});
  };
  commands.push_back(vertex_load);
  commands.push_back(pixel_load);
  reg(0x2000, kPitch);
  reg(0x200E, 0u);
  reg(0x200F, kPitch | (kHeight << 16u));
  reg(0x2081, 1u << 31u);
  reg(0x2082, kPitch | (kHeight << 16u));
  reg(0x2001, 0u | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  reg(0x2002, 200u | (static_cast<std::uint32_t>(DepthRenderTargetFormat::D24FS8) << 16u));
  reg(0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
  reg(0x2104, 0xFu);
  // Z_ENABLE, Z_WRITE_ENABLE, function ALWAYS.
  reg(0x2200, (1u << 1u) | (1u << 2u) | (7u << 4u));
  reg(0x2201, 0x00010001u);
  reg(0x2204, 1u << 16u);
  reg(0x2206, 1u << 10u);
  reg(0x2100, 0xFFFFFFu);
  const auto fetch_constant = ResourceStateTracker::kFetchConstantBase;
  reg(fetch_constant, 3u | kVertices);
  reg(fetch_constant + 1u, static_cast<std::uint32_t>(Endian::None) | (12u << 2u));
  const std::array<float, 4> red{1.0f, 0.0f, 0.0f, 1.0f};
  for (std::uint32_t i = 0; i < 4; ++i) reg(0x4400u + i, std::bit_cast<std::uint32_t>(red[i]));
  commands.push_back(make_draw());
  commands.push_back(make_draw());  // the same state: nothing more to predict

  // Counts since this backend was created: earlier tests ran on it.
  const auto before = backend.shader_prefetch_statistics();
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");
  backend.prefetch_shaders(commands);
  const auto predicted = backend.shader_prefetch_statistics();
  if (predicted.variants_predicted - before.variants_predicted != 1u) {
    std::cerr << name << ": the frontend predicted " << predicted.variants_predicted
              << " shader variants for one D24FS8 draw state" << std::endl;
    assert(false);
  }
  for (const auto& command : commands) backend.consume(command);
  assert_backend_ok(backend, "draws with a prefetched variant");
  const auto used = backend.shader_prefetch_statistics();
  if (used.variants_prefetched_at_draw - before.variants_prefetched_at_draw != 1u ||
      used.variants_compiled_at_draw != before.variants_compiled_at_draw) {
    std::cerr << name << ": the D24FS8 variant was prefetched for "
              << used.variants_prefetched_at_draw - before.variants_prefetched_at_draw << " draw(s) and compiled at "
              << used.variants_compiled_at_draw - before.variants_compiled_at_draw << std::endl;
    assert(false);
  }

  // The same state with multisampling, never handed to prefetch_shaders():
  // the draw lowers and compiles its per-sample variant itself.
  backend.consume(ir::Command{ir::RegisterWrite{0x2000, kPitch | (2u << 16u)}});
  backend.consume(ir::Command{make_draw()});
  assert_backend_ok(backend, "draw with an unpredicted variant");
  assert(backend.shader_prefetch_statistics().variants_compiled_at_draw ==
         before.variants_compiled_at_draw + 1u);
  backend.end_submission();
  assert_backend_ok(backend, "end_submission");
  std::cout << name << " shader-variant prefetch: ok" << std::endl;
}

// Regression (AC6 cutscene hitches and mission frame drops): Direct3D clears
// EDRAM by drawing a screen-space rectangle - clipping and the viewport
// transform off - under an open 8192-row scissor. Its render target was
// sized to the scissor, every row EDRAM holds at that pitch, and each clear
// moved the ownership of every tile (55-990 ms each in AC6). The rectangle's
// vertex shader now runs on the CPU: the target is as tall as the rectangle,
// and everything the rectangle covers resolves.
template <typename Backend>
void run_unclipped_clear_is_sized_to_its_vertices(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kPitch = 240, kWidth = 208, kHeight = 144;
  constexpr std::uint32_t kVertices = 0x80000, kDestination = 0x00500000u;
  constexpr std::uint32_t kDestinationRowPitch = 224;  // 208 to whole 32-texel tiles
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  // Rectangle-list corners (x, y, z, w) in render-target pixels, little-endian
  // (fetch endian None), offset by -0.5 as Direct3D's clears are so every
  // sample of the edge pixels is inside; and the resolve's rectangle.
  const std::array<float, 12> corners{-0.5f, -0.5f, 0.0f, 1.0f, float(kWidth) - 0.5f, -0.5f, 0.0f,
                                      1.0f, -0.5f, float(kHeight) - 0.5f, 0.0f, 1.0f};
  write_guest_bytes(memory, kVertices, corners.data(), sizeof(corners));
  const std::array<float, 6> resolve_rectangle{-0.5f, -0.5f, float(kWidth) - 0.5f,
                                               -0.5f, -0.5f, float(kHeight) - 0.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_rectangle.data(), sizeof(resolve_rectangle));
  Edram edram;
  edram.reset();

  // vfetch r1 = float4 at fetch constant 0; e62 = max(r1, r1).
  DecodedShader vertex{};
  {
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
    position.alu.vector_opcode = 2;
    position.alu.scalar_opcode = 50;
    position.alu.vector_destination = 62;
    position.alu.vector_write_mask = 0xF;
    position.alu.export_data = true;
    for (auto& source : position.alu.sources) source = {1u, 0xE4u, true};
    vertex.instructions = {fetch, position};
    vertex.reflection.vertex_fetch_constants = {0};
    vertex.reflection.vertex_fetches = {0};
    vertex.reflection.exports = {62};
    vertex.reflection.position_exports = 1;
    vertex.reflection.temporary_register_count = 2;
  }
  // oC0 = c0.
  DecodedShader pixel{};
  {
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
  }
  const auto vertex_load = decoded_shader_load(ShaderStage::Vertex, 1, vertex);
  const auto pixel_load = decoded_shader_load(ShaderStage::Pixel, 2, pixel);

  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");
  backend.consume(ir::Command{vertex_load});
  backend.consume(ir::Command{pixel_load});
  // 4x MSAA at pitch 240 under Direct3D's open scissor, clipping disabled,
  // no viewport transform, W as exported.
  write_register(backend, 0x2000, kPitch | (2u << 16u));
  write_register(backend, 0x200E, 0u);
  write_register(backend, 0x200F, kPitch | (8192u << 16u));
  write_register(backend, 0x2081, 1u << 31u);
  write_register(backend, 0x2082, kPitch | (8192u << 16u));
  write_register(backend, 0x2001, 0u | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
  write_register(backend, 0x2104, 0xFu);
  write_register(backend, 0x2200, 0u);
  // RB_BLENDCONTROL0 ONE/ZERO, what Direct3D sets for "no blending" (Xenos
  // always blends; zero factors would write black).
  write_register(backend, 0x2201, 0x00010001u);
  write_register(backend, 0x2204, 1u << 16u);
  write_register(backend, 0x2206, 1u << 10u);
  write_register(backend, 0x2100, 0xFFFFFFu);
  const auto fetch_constant = ResourceStateTracker::kFetchConstantBase;
  write_register(backend, fetch_constant, 3u | kVertices);
  write_register(backend, fetch_constant + 1u, static_cast<std::uint32_t>(Endian::None) | (12u << 2u));
  const std::array<float, 4> red{1.0f, 0.0f, 0.0f, 1.0f};
  for (std::uint32_t i = 0; i < 4; ++i)
    write_register(backend, 0x4400u + i, std::bit_cast<std::uint32_t>(red[i]));
  ir::DrawPacket draw{};
  draw.opcode = Type3Opcode::DrawIndx2;
  draw.primitive_type = PrimitiveType::RectangleList;
  draw.source = DrawSource::AutoIndex;
  draw.index_count = 3;
  draw.vertex_shader = {true, vertex_load.program.hash(), 0};
  draw.pixel_shader = {true, pixel_load.program.hash(), 0};
  backend.consume(ir::Command{std::move(draw)});
  assert_backend_ok(backend, "unclipped clear");
  const auto rows = backend.tallest_render_target_rows();
  if (rows < kHeight || rows > kHeight + 16u) {
    std::cerr << name << ": the unclipped clear's render target is " << rows
              << " rows for a " << kHeight << "-row rectangle\n";
    assert(false);
  }

  set_resolve_fetch(backend);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318, static_cast<std::uint32_t>(CopyCommand::Raw) << 20u);
  write_register(backend, 0x2319, kDestination);
  write_register(backend, 0x231A, kDestinationRowPitch | (kHeight << 16u));
  write_register(backend, 0x231B, static_cast<std::uint32_t>(Endian128::None) | (6u << 7u));
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
  assert_backend_ok(backend, "unclipped clear resolve");
  // Every pixel of the rectangle holds the colour (EDRAM was zero), first
  // and last rows and columns included.
  const auto word_at = [&](std::uint32_t x, std::uint32_t y) {
    return read_guest_word(memory, kDestination + static_cast<std::uint32_t>(
                                                      tiled_offset_2d(x, y, kDestinationRowPitch, 4)));
  };
  const auto colour = word_at(0, 0);
  std::uint32_t full_bytes = 0, zero_bytes = 0;
  for (unsigned b = 0; b < 4; ++b) {
    const auto lane = (colour >> (b * 8u)) & 0xFFu;
    full_bytes += lane == 0xFFu;
    zero_bytes += lane == 0u;
  }
  if (full_bytes != 2u || zero_bytes != 2u) {
    std::cerr << name << ": unclipped clear resolved 0x" << std::hex << colour << std::dec
              << " at (0,0), not the shader's red; draws " << backend.draw_count()
              << ", translation failures " << backend.shader_coverage().translation_failures
              << ", unsupported " << backend.unsupported_counters().total() << '\n';
    for (const auto& reason : backend.failure_reasons())
      std::cerr << "  failure: " << reason.message << " x" << reason.count << '\n';
    const auto u = backend.unsupported_counters();
    std::cerr << "  unsupported: fetch formats " << u.unsupported_fetch_formats << ", texture formats "
              << u.unsupported_texture_formats << ", samplers " << u.unsupported_sampler_behaviors
              << ", instructions " << u.unsupported_shader_instructions << ", features "
              << u.unsupported_shader_features << ", resolves " << u.unhandled_resolve_modes
              << ", fallback shaders " << u.fallback_shader_uses << ", blends "
              << u.unsupported_blend_states << ", packets " << u.unknown_packets << '\n';
    assert(backend.make_edram_canonical());
    const EdramSurfaceLayout drawn{0, kPitch, kHeight, MsaaSamples::X4, false, false};
    for (const auto [x, y] : {std::pair{0u, 0u}, std::pair{100u, 70u}, std::pair{207u, 143u}})
      std::cerr << "  EDRAM sample 0 at (" << x << ',' << y << "): 0x" << std::hex
                << read_edram_sample(edram, drawn, x, y)[0] << std::dec << '\n';
    assert(false);
  }
  for (std::uint32_t y = 0; y < kHeight; ++y)
    for (std::uint32_t x = 0; x < kWidth; ++x)
      if (word_at(x, y) != colour) {
        std::cerr << name << ": unclipped clear pixel (" << x << ',' << y << ") is 0x" << std::hex
                  << word_at(x, y) << ", not 0x" << colour << std::dec << '\n';
        assert(false);
      }
  backend.end_submission();
  std::cout << name << " unclipped clear sized to its rectangle (" << rows << " rows): ok\n";
}

// Regression (AC6 blocky smoke and missile trails, white blocks, the picture
// turning near-black for seconds): Xenos dot products are sums of Xenos
// multiplications, where 0 * anything is 0 - infinity and NaN included. The
// translation used the host's dot(), whose 0 * inf is NaN; the NaN a 0-weighted
// channel left spread through AC6's 160x96 bloom and luminance reductions (an
// 8-pixel block per NaN texel on screen, the exposure adapting to garbage).
// A pixel shader computes DP4, DP3, DP2ADD and DST over operands where one
// product is 0 * inf; on the GPU every result must be the Xenos one.
template <typename Backend>
void run_dot_products_follow_xenos_zero_rule(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kPitch = 80, kWidth = 32, kHeight = 16;
  constexpr std::uint32_t kVertices = 0x80000, kDestination = 0x00500000u;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 12> corners{-0.5f, -0.5f, 0.0f, 1.0f, float(kWidth) - 0.5f, -0.5f, 0.0f,
                                      1.0f, -0.5f, float(kHeight) - 0.5f, 0.0f, 1.0f};
  write_guest_bytes(memory, kVertices, corners.data(), sizeof(corners));
  const std::array<float, 6> resolve_rectangle{-0.5f, -0.5f, float(kWidth) - 0.5f,
                                               -0.5f, -0.5f, float(kHeight) - 0.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_rectangle.data(), sizeof(resolve_rectangle));
  Edram edram;
  edram.reset();

  // vfetch r1 = float4 at fetch constant 0; e62 = max(r1, r1).
  DecodedShader vertex{};
  {
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
    position.alu.vector_opcode = 2;
    position.alu.scalar_opcode = 50;
    position.alu.vector_destination = 62;
    position.alu.vector_write_mask = 0xF;
    position.alu.export_data = true;
    for (auto& source : position.alu.sources) source = {1u, 0xE4u, true};
    vertex.instructions = {fetch, position};
    vertex.reflection.vertex_fetch_constants = {0};
    vertex.reflection.vertex_fetches = {0};
    vertex.reflection.exports = {62};
    vertex.reflection.position_exports = 1;
    vertex.reflection.temporary_register_count = 2;
  }
  // r0.x = dp4(c0, c1); r0.y = dp3(c0, c1); r0.z = dp2add(c0, c1, c2);
  // r1.y = dst(c3, c4).y; r0.w = r1.y * c2.w + c2.y; oC0 = r0.
  DecodedShader pixel{};
  {
    const auto alu = [](std::uint32_t op, std::uint32_t destination, std::uint32_t mask,
                        AluSource a, AluSource b, AluSource c) {
      DecodedInstruction instruction{};
      instruction.alu.vector_opcode = static_cast<std::uint8_t>(op);
      instruction.alu.scalar_opcode = 50;
      instruction.alu.vector_destination = static_cast<std::uint8_t>(destination);
      instruction.alu.vector_write_mask = static_cast<std::uint8_t>(mask);
      instruction.alu.sources = {a, b, c};
      return instruction;
    };
    const AluSource c0{0u, 0xE4u, false}, c1{1u, 0xE4u, false}, c2{2u, 0xE4u, false};
    const AluSource c3{3u, 0xE4u, false}, c4{4u, 0xE4u, false};
    const AluSource r0{0u, 0xE4u, true}, r1_y{1u, 0x55u, true};
    const AluSource c2_w{2u, 0xFFu, false}, c2_y{2u, 0x55u, false};
    auto out = alu(2, 0, 0xF, r0, r0, r0);
    out.alu.export_data = true;
    pixel.instructions = {alu(15, 0, 0x1, c0, c1, c0), alu(16, 0, 0x2, c0, c1, c0),
                          alu(17, 0, 0x4, c0, c1, c2),  alu(28, 1, 0x2, c3, c4, c3),
                          alu(11, 0, 0x8, r1_y, c2_w, c2_y), out};
    for (std::uint32_t i = 0; i < pixel.instructions.size(); ++i) pixel.instructions[i].alu.address = i;
    DecodedControlFlow cf{};
    cf.opcode = ControlFlowOpcode::ExecEnd;
    cf.count = static_cast<std::uint32_t>(pixel.instructions.size());
    pixel.control_flow = {cf};
    pixel.reflection.float_constants = {0, 1, 2, 3, 4};
    pixel.reflection.exports = {0};
    pixel.reflection.color_exports = 1;
    pixel.reflection.temporary_register_count = 2;
  }
  const auto vertex_load = decoded_shader_load(ShaderStage::Vertex, 3, vertex);
  const auto pixel_load = decoded_shader_load(ShaderStage::Pixel, 4, pixel);

  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");
  backend.consume(ir::Command{vertex_load});
  backend.consume(ir::Command{pixel_load});
  write_register(backend, 0x2000, kPitch);
  write_register(backend, 0x200E, 0u);
  write_register(backend, 0x200F, kPitch | (8192u << 16u));
  write_register(backend, 0x2081, 1u << 31u);
  write_register(backend, 0x2082, kPitch | (8192u << 16u));
  write_register(backend, 0x2001, 0u | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
  write_register(backend, 0x2104, 0xFu);
  write_register(backend, 0x2200, 0u);
  write_register(backend, 0x2201, 0x00010001u);
  write_register(backend, 0x2204, 1u << 16u);
  write_register(backend, 0x2206, 1u << 10u);
  write_register(backend, 0x2100, 0xFFFFFFu);
  const auto fetch_constant = ResourceStateTracker::kFetchConstantBase;
  write_register(backend, fetch_constant, 3u | kVertices);
  write_register(backend, fetch_constant + 1u, static_cast<std::uint32_t>(Endian::None) | (12u << 2u));
  constexpr float inf = std::numeric_limits<float>::infinity();
  const std::array<std::array<float, 4>, 5> constants{{
      {inf, 0.5f, 0.25f, 0.25f},  // c0: x meets c1.x = 0
      {0.0f, 1.0f, 1.0f, 1.0f},   // c1
      {0.25f, 0.5f, 0.0f, 1.0f},  // c2
      {0.0f, inf, 0.0f, 0.0f},    // c3: y meets c4.y = 0
      {0.0f, 0.0f, 0.0f, 0.0f},   // c4
  }};
  for (std::uint32_t c = 0; c < constants.size(); ++c)
    for (std::uint32_t i = 0; i < 4; ++i)
      write_register(backend, 0x4400u + c * 4u + i, std::bit_cast<std::uint32_t>(constants[c][i]));
  ir::DrawPacket draw{};
  draw.opcode = Type3Opcode::DrawIndx2;
  draw.primitive_type = PrimitiveType::RectangleList;
  draw.source = DrawSource::AutoIndex;
  draw.index_count = 3;
  draw.vertex_shader = {true, vertex_load.program.hash(), 0};
  draw.pixel_shader = {true, pixel_load.program.hash(), 0};
  backend.consume(ir::Command{std::move(draw)});
  assert_backend_ok(backend, "dot product draw");

  set_resolve_fetch(backend);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318, static_cast<std::uint32_t>(CopyCommand::Raw) << 20u);
  write_register(backend, 0x2319, kDestination);
  write_register(backend, 0x231A, kWidth | (kHeight << 16u));
  write_register(backend, 0x231B, static_cast<std::uint32_t>(Endian128::None) | (6u << 7u));
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
  assert_backend_ok(backend, "dot product resolve");
  // dp4 = 0 + 0.5 + 0.25 + 0.25 = 1; dp3 = 0.75; dp2add = 0 + 0.5 + 0.25 =
  // 0.75; dst.y = 0, so w = 0 * 1 + 0.5. A NaN would have stored 0.
  const auto word = read_guest_word(
      memory, kDestination + static_cast<std::uint32_t>(tiled_offset_2d(5, 7, kWidth, 4)));
  const std::array<std::uint32_t, 4> expected{255u, 191u, 191u, 128u};
  const char* names[] = {"DP4", "DP3", "DP2ADD", "DST"};
  bool ok = true;
  for (unsigned c = 0; c < 4; ++c) {
    const auto lane = (word >> (c * 8u)) & 0xFFu;
    const auto delta = lane > expected[c] ? lane - expected[c] : expected[c] - lane;
    if (delta > 1u) {
      std::cerr << name << ": " << names[c] << " stored " << lane << ", not " << expected[c]
                << " (word 0x" << std::hex << word << std::dec << ")\n";
      ok = false;
    }
  }
  assert(ok);
  backend.end_submission();
  std::cout << name << " dot products follow the Xenos 0 * anything = 0 rule: ok\n";
}

// Indexed draws through a real backend: a DMA index buffer converted once is
// reused - from the processed-index cache and the GPU-resident index buffer -
// while its guest bytes are unchanged, and a CPU write to it is seen by the
// very next draw (the cache must never serve stale indices).
template <typename Backend>
void run_indexed_draws_track_index_buffer_writes(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kPitch = 80, kWidth = 32, kHeight = 16;
  constexpr std::uint32_t kVertices = 0x90000, kIndices = 0x98000;
  constexpr std::uint32_t kFirst = 0x00600000u, kSecond = 0x00680000u;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  // Pixel-space corners: the left half (0, 1, 2, 3) and the right half's far
  // edge (4, 5).
  const std::array<float, 24> vertices{
      -0.5f, -0.5f, 0.0f, 1.0f,  15.5f, -0.5f, 0.0f, 1.0f,  -0.5f, 15.5f, 0.0f, 1.0f,
      15.5f, 15.5f, 0.0f, 1.0f,  31.5f, -0.5f, 0.0f, 1.0f,  31.5f, 15.5f, 0.0f, 1.0f};
  write_guest_bytes(memory, kVertices, vertices.data(), sizeof(vertices));
  const std::array<float, 6> resolve_rectangle{-0.5f, -0.5f, float(kWidth) - 0.5f,
                                               -0.5f, -0.5f, float(kHeight) - 0.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_rectangle.data(), sizeof(resolve_rectangle));
  // Big-endian 16-bit indices (Swap8In16), as a title stores them.
  const auto write_indices = [&](std::array<std::uint16_t, 6> indices) {
    for (auto& index : indices) index = std::uint16_t((index << 8u) | (index >> 8u));
    write_guest_bytes(memory, kIndices, indices.data(), sizeof(indices));
  };
  write_indices({0, 1, 2, 1, 3, 2});
  Edram edram;
  edram.reset();
  const auto vertex_load = decoded_shader_load(ShaderStage::Vertex, 51, fetch_position_vertex_shader());
  const auto pixel_load = decoded_shader_load(ShaderStage::Pixel, 52, constant_color_pixel_shader());

  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");
  backend.consume(ir::Command{vertex_load});
  backend.consume(ir::Command{pixel_load});
  const auto set_draw_state = [&] {
    write_register(backend, 0x2000, kPitch);
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, kPitch | (8192u << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, kPitch | (8192u << 16u));
    write_register(backend, 0x2001,
                   0u | (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
    write_register(backend, 0x2104, 0xFu);
    write_register(backend, 0x2200, 0u);
    write_register(backend, 0x2201, 0x00010001u);
    write_register(backend, 0x2204, 1u << 16u);
    write_register(backend, 0x2206, 1u << 10u);
    write_register(backend, 0x2100, 0xFFFFFFu);
    const auto fetch_constant = ResourceStateTracker::kFetchConstantBase;
    write_register(backend, fetch_constant, 3u | kVertices);
    write_register(backend, fetch_constant + 1u,
                   static_cast<std::uint32_t>(Endian::None) | (24u << 2u));
  };
  const auto set_color = [&](std::array<float, 4> color) {
    for (std::uint32_t i = 0; i < 4; ++i)
      write_register(backend, 0x4400u + i, std::bit_cast<std::uint32_t>(color[i]));
  };
  const auto draw_indexed = [&] {
    ir::DrawPacket draw{};
    draw.opcode = Type3Opcode::DrawIndx2;
    draw.primitive_type = PrimitiveType::TriangleList;
    draw.source = DrawSource::Dma;
    draw.index_format = IndexFormat::UInt16;
    draw.index_count = 6;
    draw.index_buffer.valid = true;
    draw.index_buffer.physical_address = kIndices;
    draw.index_buffer.length_bytes = 12;
    draw.index_buffer.format = IndexFormat::UInt16;
    draw.index_buffer.endian = Endian::Swap8In16;
    draw.vertex_shader = {true, vertex_load.program.hash(), 0};
    draw.pixel_shader = {true, pixel_load.program.hash(), 0};
    backend.consume(ir::Command{std::move(draw)});
    assert_backend_ok(backend, "indexed draw");
  };
  const auto resolve_to = [&](std::uint32_t destination) {
    set_resolve_fetch(backend);
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
    write_register(backend, 0x2318, static_cast<std::uint32_t>(CopyCommand::Raw) << 20u);
    write_register(backend, 0x2319, destination);
    write_register(backend, 0x231A, kWidth | (kHeight << 16u));
    write_register(backend, 0x231B, static_cast<std::uint32_t>(Endian128::None) | (6u << 7u));
    issue_dummy_draw(backend);
    guest_observes_resolve(backend);
    assert_backend_ok(backend, "indexed draw resolve");
  };
  const auto pixel = [&](std::uint32_t destination, std::uint32_t x, std::uint32_t y) {
    return read_guest_word(
        memory, destination + static_cast<std::uint32_t>(tiled_offset_2d(x, y, kWidth, 4)));
  };
  const auto expect = [&](const char* what, std::uint32_t got, std::uint32_t want) {
    if (got == want) return;
    std::cerr << name << ": " << what << " is 0x" << std::hex << got << ", not 0x" << want
              << std::dec << '\n';
    assert(false);
  };

  // The left half, twice: the second draw reuses the converted indices.
  set_draw_state();
  set_color({1.0f, 0.0f, 0.0f, 1.0f});
  const auto hits_before = xenon::gpu::perf::peek(xenon::gpu::perf::Counter::IndexCacheHits);
  const auto resident_before =
      xenon::gpu::perf::peek(xenon::gpu::perf::Counter::ResidentIndexDraws);
  draw_indexed();
  draw_indexed();
  assert(xenon::gpu::perf::peek(xenon::gpu::perf::Counter::IndexCacheHits) > hits_before);
  assert(xenon::gpu::perf::peek(xenon::gpu::perf::Counter::ResidentIndexDraws) >=
         resident_before + 2u);
  resolve_to(kFirst);
  expect("left pixel after the first draws", pixel(kFirst, 5, 7), 0xFF0000FFu);
  expect("right pixel after the first draws", pixel(kFirst, 25, 7), 0u);

  // The title rewrites its index buffer in place: the next draw covers the
  // right half, in green; the left half keeps its red.
  write_indices({1, 4, 3, 4, 5, 3});
  set_draw_state();
  set_color({0.0f, 1.0f, 0.0f, 1.0f});
  draw_indexed();
  resolve_to(kSecond);
  expect("right pixel after the rewrite", pixel(kSecond, 25, 7), 0xFF00FF00u);
  expect("left pixel after the rewrite", pixel(kSecond, 5, 7), 0xFF0000FFu);
  backend.end_submission();
  std::cout << name << " indexed draws track their index buffer: ok\n";
}

// Two native color images of one format over the same EDRAM tile: the second
// takes the tile from the first on the GPU (EdramColorTransfer) rather than
// through canonical EDRAM, and resolving it yields exactly the EDRAM words the
// first imported.
template <typename Backend>
void run_direct_color_transfer(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kDestination = 0xC0000;
  constexpr std::uint32_t kTileWide = 80;
  constexpr std::uint32_t kTwoTilesWide = 160;
  constexpr std::uint32_t kRows = 16;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 6> resolve_vertices{
      -0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_vertices.data(),
                    sizeof(resolve_vertices));
  Edram edram;
  edram.reset();
  const EdramSurfaceLayout wide{kSurfaceBaseTile, kTwoTilesWide, kRows,
                                MsaaSamples::X1, false, false};
  for (std::uint32_t y = 0; y < kRows; ++y)
    for (std::uint32_t x = 0; x < kTwoTilesWide; ++x)
      assert(write_edram_sample(edram, wide, x, y, {0xC3000000u | (y << 8u) | x, 0u}));
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");

  const auto set_surface = [&](std::uint32_t pitch) {
    write_register(backend, 0x2000, pitch);
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, pitch | (kRows << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, pitch | (kRows << 16u));
    write_register(backend, 0x2001,
                   kSurfaceBaseTile |
                       (static_cast<std::uint32_t>(ColorRenderTargetFormat::R8G8B8A8) << 16u));
  };
  const auto draw_color = [&](std::uint32_t pitch) {
    set_surface(pitch);
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::ColorDepth));
    write_register(backend, 0x2104, 0xFu);
    write_register(backend, 0x2200, 0u);
    issue_dummy_draw(backend);
  };
  draw_color(kTileWide);
  assert_backend_ok(backend, "one-tile color owner");
  const auto before = backend.performance_counters().direct_edram_transfers;
  draw_color(kTwoTilesWide);
  assert_backend_ok(backend, "two-tile color owner");
  assert(backend.realized_render_target_count() == 2);
  if (backend.performance_counters().direct_edram_transfers <= before) {
    std::cerr << name << ": the tile went through canonical EDRAM, not a direct transfer\n";
    assert(false);
  }

  set_surface(kTwoTilesWide);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318, static_cast<std::uint32_t>(CopyCommand::Raw) << 20u);
  set_resolve_destination(backend, kDestination, 6);
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
  assert_backend_ok(backend, "resolve of the transferred tile");
  for (const auto [x, y] : {std::pair{0u, 0u}, std::pair{15u, 15u}, std::pair{7u, 3u}}) {
    const auto word = resolved_word_at(memory, kDestination, x, y);
    if (word != (0xC3000000u | (y << 8u) | x)) {
      std::cerr << name << " direct transfer resolved 0x" << std::hex << word
                << " at (" << std::dec << x << ',' << y << ")\n";
      assert(false);
    }
  }
  backend.end_submission();
  std::cout << name << " direct EDRAM color transfer: ok\n";
}

// Regression (AC6's first cutscene ran at about 8 fps): a depth image taking
// EDRAM tiles from another depth image went through a readback, a CPU repack
// and an upload that drained the queue, several times a frame. The second
// image now takes the tile on the GPU (EdramDepthTransfer), and resolving it
// yields exactly the packed D24S8 words - depth and every stencil bit - the
// first imported.
template <typename Backend>
void run_direct_depth_transfer(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kDestination = 0xE0000;
  constexpr std::uint32_t kTileWide = 80;
  constexpr std::uint32_t kTwoTilesWide = 160;
  constexpr std::uint32_t kRows = 16;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 6> resolve_vertices{
      -0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_vertices.data(),
                    sizeof(resolve_vertices));
  Edram edram;
  edram.reset();
  const EdramSurfaceLayout wide{kSurfaceBaseTile, kTwoTilesWide, kRows,
                                MsaaSamples::X1, false, true};
  // Distinct depth per pixel; stencil runs through every bit pattern.
  const auto seeded = [](std::uint32_t x, std::uint32_t y) {
    const auto depth = 0x400000u + (y << 8u) + x;
    const auto stencil = (x * 7u + y * 37u) & 0xFFu;
    return (depth << 8u) | stencil;
  };
  for (std::uint32_t y = 0; y < kRows; ++y)
    for (std::uint32_t x = 0; x < kTwoTilesWide; ++x)
      assert(write_edram_sample(edram, wide, x, y, {seeded(x, y), 0u}));
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");

  const auto set_surface = [&](std::uint32_t pitch) {
    write_register(backend, 0x2000, pitch);
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, pitch | (kRows << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, pitch | (kRows << 16u));
    write_register(backend, 0x2002,
                   kSurfaceBaseTile |
                       (static_cast<std::uint32_t>(DepthRenderTargetFormat::D24S8) << 16u));
  };
  const auto draw_depth = [&](std::uint32_t pitch) {
    set_surface(pitch);
    write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::DepthOnly));
    write_register(backend, 0x2104, 0u);
    write_register(backend, 0x2200, (1u << 1u) | (1u << 2u));
    issue_dummy_draw(backend);
  };
  draw_depth(kTileWide);
  assert_backend_ok(backend, "one-tile depth owner");
  const auto before = backend.performance_counters().direct_edram_transfers;
  draw_depth(kTwoTilesWide);
  assert_backend_ok(backend, "two-tile depth owner");
  if (backend.performance_counters().direct_edram_transfers <= before) {
    std::cerr << name << ": the depth tile went through canonical EDRAM, not a direct transfer\n";
    assert(false);
  }

  set_surface(kTwoTilesWide);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318, 4u | (static_cast<std::uint32_t>(CopyCommand::Raw) << 20u));
  set_resolve_destination(backend, kDestination, 6);
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
  assert_backend_ok(backend, "resolve of the transferred depth tile");
  for (std::uint32_t y = 0; y < 16u; ++y) {
    for (std::uint32_t x = 0; x < 16u; ++x) {
      const auto word = resolved_word_at(memory, kDestination, x, y);
      if (word != seeded(x, y)) {
        std::cerr << name << " direct depth transfer resolved 0x" << std::hex << word
                  << " at (" << std::dec << x << ',' << y << "), expected 0x" << std::hex
                  << seeded(x, y) << std::dec << '\n';
        assert(false);
      }
    }
  }
  backend.end_submission();
  std::cout << name << " direct EDRAM depth transfer: ok\n";
}

struct TransferView {
  bool depth{};
  std::uint8_t format{};
  std::string_view label{};
  // The texture format a raw resolve of this view copies into (same size and
  // layout; depth resolves as a 32-bit word).
  std::uint8_t resolve_format{6};
};

constexpr TransferView kD24S8View{true, std::uint8_t(DepthRenderTargetFormat::D24S8), "D24S8"};
constexpr TransferView kD24FS8View{true, std::uint8_t(DepthRenderTargetFormat::D24FS8),
                                   "D24FS8"};
constexpr TransferView kRgba8View{false, std::uint8_t(ColorRenderTargetFormat::R8G8B8A8),
                                  "8_8_8_8"};
constexpr TransferView kR32FloatView{false, std::uint8_t(ColorRenderTargetFormat::R32Float),
                                     "32_FLOAT", 36};
constexpr TransferView kR16G16FloatView{
    false, std::uint8_t(ColorRenderTargetFormat::R16G16Float), "16_16_FLOAT", 31};

// Regression (AC6's first cutscene ran below one frame a second): an EDRAM
// tile changing between two different views - D24S8 and D24FS8 depth, depth
// and a color format, two color formats - went through a readback, a CPU
// repack and an upload with a queue drain, several times a frame, because
// only same-format images had a GPU transfer. The tiles now move on the GPU
// through the converting transfer, both ways (`first` -> `second` ->
// `first`), and resolving `first` yields exactly the EDRAM words it
// imported: the Xbox keeps the word, whatever view reads it.
template <typename Backend>
void run_converting_transfer(Backend& backend, std::string_view name, TransferView first,
                             TransferView second) {
  constexpr std::uint32_t kDestination = 0x100000;
  constexpr std::uint32_t kTileWide = 80;
  constexpr std::uint32_t kTwoTilesWide = 160;
  constexpr std::uint32_t kRows = 16;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 6> resolve_vertices{-0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_vertices.data(), sizeof(resolve_vertices));
  Edram edram;
  edram.reset();
  const EdramSurfaceLayout wide{kSurfaceBaseTile, kTwoTilesWide, kRows,
                                MsaaSamples::X1,  false,         first.depth};
  // Distinct words that are ordinary values in every view (their 16-bit
  // halves are finite), plus a cleared depth/stencil (0xFFFFFF00: a NaN as a
  // float, two NaN halves), zero, 1.0f, and opaque 8_8_8_8 colors whose high
  // half is a NaN with a payload (0xFFxx) or an infinity (0x7C00).
  const auto seeded = [](std::uint32_t x, std::uint32_t y) -> std::uint32_t {
    if (y == 0 && x == 0) return 0xFFFFFF00u;
    if (y == 0 && x == 1) return 0u;
    if (y == 0 && x == 2) return 0x3F800000u;
    if (y == 0 && x == 3) return 0xFF204080u;
    if (y == 0 && x == 4) return 0x7C00FE01u;
    return 0x40000000u | (((x * 7u + y * 37u) & 0xFFu) << 16u) | (y << 8u) | x;
  };
  const auto special = [](std::uint32_t x, std::uint32_t y) { return y == 0 && x < 5; };
  for (std::uint32_t y = 0; y < kRows; ++y)
    for (std::uint32_t x = 0; x < kTwoTilesWide; ++x)
      assert(write_edram_sample(edram, wide, x, y, {seeded(x, y), 0u}));
  backend.begin_submission(memory, edram);
  assert_backend_ok(backend, "begin_submission");

  const auto set_surface = [&](TransferView view, std::uint32_t pitch) {
    write_register(backend, 0x2000, pitch);
    write_register(backend, 0x200E, 0u);
    write_register(backend, 0x200F, pitch | (kRows << 16u));
    write_register(backend, 0x2081, 1u << 31u);
    write_register(backend, 0x2082, pitch | (kRows << 16u));
    write_register(backend, view.depth ? 0x2002 : 0x2001,
                   kSurfaceBaseTile | (std::uint32_t(view.format) << 16u));
  };
  const auto draw = [&](TransferView view, std::uint32_t pitch) {
    set_surface(view, pitch);
    write_register(backend, 0x2208,
                   static_cast<std::uint32_t>(view.depth ? EdramMode::DepthOnly
                                                         : EdramMode::ColorDepth));
    write_register(backend, 0x2104, view.depth ? 0u : 0xFu);
    write_register(backend, 0x2200, view.depth ? (1u << 1u) | (1u << 2u) : 0u);
    issue_dummy_draw(backend);
  };
  const auto expect_direct = [&](std::uint64_t before, std::string_view step) {
    if (backend.performance_counters().direct_edram_transfers > before) return;
    std::cerr << name << ' ' << first.label << " <-> " << second.label << ": " << step
              << " went through canonical EDRAM, not a GPU transfer\n";
    assert(false);
  };

  draw(first, kTileWide);
  assert_backend_ok(backend, "first view owner");
  auto before = backend.performance_counters().direct_edram_transfers;
  draw(second, kTwoTilesWide);
  assert_backend_ok(backend, "second view owner");
  expect_direct(before, "first -> second");
  before = backend.performance_counters().direct_edram_transfers;
  draw(first, kTwoTilesWide);
  assert_backend_ok(backend, "first view again");
  expect_direct(before, "second -> first");

  set_surface(first, kTwoTilesWide);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208, static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318,
                 (first.depth ? 4u : 0u) | (static_cast<std::uint32_t>(CopyCommand::Raw) << 20u));
  set_resolve_destination(backend, kDestination, first.resolve_format);
  issue_dummy_draw(backend);
  guest_observes_resolve(backend);
  assert_backend_ok(backend, "resolve after the round trip");
  std::uint32_t mismatches = 0;
  for (std::uint32_t y = 0; y < 16u; ++y) {
    for (std::uint32_t x = 0; x < 16u; ++x) {
      const auto word = resolved_word_at(memory, kDestination, x, y);
      if (word == seeded(x, y)) continue;
      if (++mismatches <= 8u)
        std::cerr << name << ' ' << first.label << " <-> " << second.label << " resolved 0x"
                  << std::hex << word << " at (" << std::dec << x << ',' << y
                  << "), expected 0x" << std::hex << seeded(x, y) << std::dec
                  << (special(x, y) ? " (special word)" : "") << '\n';
    }
  }
  assert(mismatches == 0);
  backend.end_submission();
  std::cout << name << " converting EDRAM transfer " << first.label << " <-> " << second.label
            << ": ok\n";
}

template <typename Backend, typename Make>
void run_converting_transfers(Make make, std::string_view name) {
  // 16_16_FLOAT: AC6 flips its scene tiles between 8_8_8_8 and 16_16_FLOAT
  // every frame, which went through a CPU repack and held its missions near
  // 6 fps; the words carry NaN halves (an opaque 8_8_8_8 color), which the
  // integer view keeps.
  const std::array<std::pair<TransferView, TransferView>, 8> pairs{{
      {kD24S8View, kD24FS8View},
      {kD24S8View, kR32FloatView},
      {kD24FS8View, kRgba8View},
      {kRgba8View, kR32FloatView},
      {kRgba8View, kD24S8View},
      {kRgba8View, kR16G16FloatView},
      {kR16G16FloatView, kR32FloatView},
      {kD24S8View, kR16G16FloatView},
  }};
  for (const auto& [first, second] : pairs) {
    Backend backend;
    if (!make(backend)) {
      std::cerr << name << " backend initialization failed: " << backend.error() << '\n';
      assert(false);
    }
    run_converting_transfer(backend, name, first, second);
  }
}

// Canonical EDRAM loads on the GPU (GPU-resident canonical storage): a
// native image taking tiles that canonical EDRAM holds - first use, or after
// native state was discarded - gets every guest sample's word from the
// uploaded EDRAM image, unpacked in a shader, for each format the converting
// transfer covers and each MSAA mode. Returned to canonical storage (a
// readback and the CPU's exact pack), every word comes back unchanged.
template <typename Backend>
void run_canonical_gpu_loads(Backend& backend, std::string_view name) {
  constexpr std::uint32_t kPitch = 80;
  constexpr std::uint32_t kRows = 16;
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  Edram edram;
  edram.reset();
  const std::array views{kD24S8View, kD24FS8View, kRgba8View, kR32FloatView, kR16G16FloatView};
  const std::array<MsaaSamples, 3> modes{MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4};
  for (const auto& view : views) {
    for (const auto msaa : modes) {
      const EdramSurfaceLayout layout{kSurfaceBaseTile, kPitch, kRows, msaa, false, view.depth};
      const auto samples = 1u << static_cast<unsigned>(msaa);
      // Words every view holds exactly (finite 16-bit halves), distinct per
      // sample.
      const auto seeded = [](std::uint32_t x, std::uint32_t y, std::uint32_t s) -> std::uint32_t {
        return 0x40000000u | (((x * 7u + y * 37u + s * 91u) & 0xFFu) << 16u) | (y << 8u) | x;
      };
      for (std::uint32_t s = 0; s < samples; ++s)
        for (std::uint32_t y = 0; y < kRows; ++y)
          for (std::uint32_t x = 0; x < kPitch; ++x)
            assert(write_edram_sample(edram, layout, x, y, {seeded(x, y, s), 0u}, s));
      backend.begin_submission(memory, edram);
      assert_backend_ok(backend, "begin_submission");
      // Everything canonical: the draw below loads the tiles.
      assert(backend.invalidate_edram_native_state());
      const auto loads_before = backend.performance_counters().gpu_canonical_edram_loads;
      write_register(backend, 0x2000, kPitch | (static_cast<std::uint32_t>(msaa) << 16u));
      write_register(backend, 0x200E, 0u);
      write_register(backend, 0x200F, kPitch | (kRows << 16u));
      write_register(backend, 0x2081, 1u << 31u);
      write_register(backend, 0x2082, kPitch | (kRows << 16u));
      write_register(backend, view.depth ? 0x2002 : 0x2001,
                     kSurfaceBaseTile | (std::uint32_t(view.format) << 16u));
      write_register(backend, 0x2208,
                     static_cast<std::uint32_t>(view.depth ? EdramMode::DepthOnly
                                                           : EdramMode::ColorDepth));
      write_register(backend, 0x2104, view.depth ? 0u : 0xFu);
      write_register(backend, 0x2200, view.depth ? (1u << 1u) | (1u << 2u) : 0u);
      issue_dummy_draw(backend);
      assert_backend_ok(backend, "draw loading canonical tiles");
      if (backend.performance_counters().gpu_canonical_edram_loads <= loads_before) {
        std::cerr << name << ' ' << view.label << " x" << samples
                  << ": the canonical tiles were not loaded on the GPU\n";
        assert(false);
      }
      // The CPU copy is overwritten; only the native image holds the words.
      for (std::uint32_t s = 0; s < samples; ++s)
        for (std::uint32_t y = 0; y < kRows; ++y)
          for (std::uint32_t x = 0; x < kPitch; ++x)
            assert(write_edram_sample(edram, layout, x, y, {0xDEADBEEFu, 0u}, s));
      assert(backend.make_edram_canonical());
      assert_backend_ok(backend, "returning the tiles to canonical storage");
      std::uint32_t mismatches = 0;
      for (std::uint32_t s = 0; s < samples; ++s) {
        for (std::uint32_t y = 0; y < kRows; ++y) {
          for (std::uint32_t x = 0; x < kPitch; ++x) {
            const auto word = read_edram_sample(edram, layout, x, y, s)[0];
            if (word == seeded(x, y, s)) continue;
            if (++mismatches <= 6u)
              std::cerr << name << ' ' << view.label << " x" << samples << " sample " << s
                        << " at (" << x << ',' << y << "): 0x" << std::hex << word
                        << ", loaded 0x" << seeded(x, y, s) << std::dec << '\n';
          }
        }
      }
      assert(mismatches == 0);
      backend.end_submission();
    }
  }
  std::cout << name << " canonical EDRAM loads on the GPU: ok\n";
}

}  // namespace

int main() {
  bool ran_backend = false;

#if defined(XENON_TEST_VULKAN)
  if (!xenon::test::vulkan_device_present()) {
    std::cout << "Vulkan: no Vulkan device on this machine; skipped\n";
  } else {
    xenon::gpu::vulkan::Backend backend;
    if (!backend.initialize({.enable_validation = false})) {
      std::cerr << "Vulkan backend initialization failed: " << backend.error()
                << '\n';
      assert(false);
    }
    run_backend_ownership_alias_resolve_clear(backend, "Vulkan");
    xenon::gpu::vulkan::Backend far_clear;
    assert(far_clear.initialize({.enable_validation = false}));
    run_backend_ownership_alias_resolve_clear(far_clear, "Vulkan (far clear)", kFarDepthClear);
    xenon::gpu::vulkan::Backend fresh;
    assert(fresh.initialize({.enable_validation = false}));
    run_resolve_without_a_matching_render_target(fresh, "Vulkan");
    xenon::gpu::vulkan::Backend transfer;
    assert(transfer.initialize({.enable_validation = false}));
    run_direct_color_transfer(transfer, "Vulkan");
    xenon::gpu::vulkan::Backend depth_transfer;
    assert(depth_transfer.initialize({.enable_validation = false}));
    run_direct_depth_transfer(depth_transfer, "Vulkan");
    xenon::gpu::vulkan::Backend alias;
    assert(alias.initialize({.enable_validation = false}));
    run_gpu_resolve_of_msaa_alias(alias, "Vulkan");
    xenon::gpu::vulkan::Backend matching;
    assert(matching.initialize({.enable_validation = false}));
    run_gpu_resolve_matches_the_cpu_path(matching, "Vulkan");
    // Each in its own scope: the backend's device and heaps go before the
    // next one is created (all of them alive at once exhausted the adapter).
    {
      xenon::gpu::vulkan::Backend unclipped;
      assert(unclipped.initialize({.enable_validation = false}));
      run_unclipped_clear_is_sized_to_its_vertices(unclipped, "Vulkan");
    }
    {
      xenon::gpu::vulkan::Backend layouts;
      assert(layouts.initialize({.enable_validation = false}));
      run_tiles_round_trip_between_layouts(layouts, "Vulkan");
    }
    {
      xenon::gpu::vulkan::Backend dots;
      assert(dots.initialize({.enable_validation = false}));
      run_dot_products_follow_xenos_zero_rule(dots, "Vulkan");
    }
    {
      xenon::gpu::vulkan::Backend indexed;
      assert(indexed.initialize({.enable_validation = false}));
      run_indexed_draws_track_index_buffer_writes(indexed, "Vulkan");
    }
    {
      xenon::gpu::vulkan::Backend canonical;
      assert(canonical.initialize({.enable_validation = false}));
      run_canonical_gpu_loads(canonical, "Vulkan");
    }
    // GPU V3 parallel recording: the same draws, every run recorded on a
    // worker into a command buffer of its own and submitted in order, give
    // the same pixels.
    // The pipelines built go to the persistent pipeline cache, written next
    // to the shader cache when the backend is destroyed.
    const auto cache_directory =
        std::filesystem::temp_directory_path() / "xenon_vulkan_pipeline_cache_test";
    std::error_code cache_error;
    std::filesystem::remove_all(cache_directory, cache_error);
    {
      xenon::gpu::vulkan::Backend parallel;
      assert(parallel.initialize({.enable_validation = false}));
      parallel.set_shader_cache_directory(cache_directory.string());
      assert(parallel.set_parallel_recording(2, 1));
      run_unclipped_clear_is_sized_to_its_vertices(parallel, "Vulkan (parallel recording)");
      run_dot_products_follow_xenos_zero_rule(parallel, "Vulkan (parallel recording)");
      run_shader_variant_prefetch(parallel, "Vulkan");
      assert(parallel.parallel_runs_recorded() > 0u);
      // Every submitted batch is timed on the GPU.
      const auto timing = parallel.gpu_timing();
      assert(timing.batches > 0u && timing.gpu_ns > 0u);
      std::cout << "Vulkan GPU timestamps: " << timing.batches << " batches, "
                << timing.gpu_ns / 1000u << " us" << std::endl;
    }
    const auto pipeline_cache = cache_directory / "vulkan_pipelines.bin";
    assert(std::filesystem::exists(pipeline_cache) &&
           std::filesystem::file_size(pipeline_cache) > 0u);
    std::cout << "Vulkan persistent pipeline cache: "
              << std::filesystem::file_size(pipeline_cache) << " bytes" << std::endl;
    {
      // A later backend loads it (an incompatible cache would be ignored).
      xenon::gpu::vulkan::Backend reloaded;
      assert(reloaded.initialize({.enable_validation = false}));
      reloaded.set_shader_cache_directory(cache_directory.string());
      run_dot_products_follow_xenos_zero_rule(reloaded, "Vulkan (reloaded pipeline cache)");
    }
    std::filesystem::remove_all(cache_directory, cache_error);
    run_converting_transfers<xenon::gpu::vulkan::Backend>(
        [](auto& b) { return b.initialize({.enable_validation = false}); }, "Vulkan");
    ran_backend = true;
  }
#endif

#if defined(XENON_TEST_D3D12)
  // These checks use a hardware adapter only; GPU-less CI runners have none.
  if (!xenon::gpu::discover_backend_capabilities()[1].runtime_available) {
    std::cout << "D3D12: no hardware adapter on this machine; skipped\n";
  } else {
    xenon::gpu::d3d12::Backend backend;
    if (!backend.initialize({.enable_debug_layer = false,
                             .allow_software_adapter = false})) {
      std::cerr << "D3D12 backend initialization failed: " << backend.error()
                << '\n';
      assert(false);
    }
    run_backend_ownership_alias_resolve_clear(backend, "D3D12");
    xenon::gpu::d3d12::Backend far_clear;
    assert(far_clear.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_backend_ownership_alias_resolve_clear(far_clear, "D3D12 (far clear)", kFarDepthClear);
    xenon::gpu::d3d12::Backend fresh;
    assert(fresh.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_resolve_without_a_matching_render_target(fresh, "D3D12");
    xenon::gpu::d3d12::Backend transfer;
    assert(transfer.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_direct_color_transfer(transfer, "D3D12");
    xenon::gpu::d3d12::Backend depth_transfer;
    assert(depth_transfer.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_direct_depth_transfer(depth_transfer, "D3D12");
    xenon::gpu::d3d12::Backend alias;
    assert(alias.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_gpu_resolve_of_msaa_alias(alias, "D3D12");
    xenon::gpu::d3d12::Backend matching;
    assert(matching.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_gpu_resolve_matches_the_cpu_path(matching, "D3D12");
    // Each in its own scope: the backend's device and heaps go before the
    // next one is created (all of them alive at once exhausted the adapter).
    {
      xenon::gpu::d3d12::Backend unclipped;
      assert(unclipped.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
      run_unclipped_clear_is_sized_to_its_vertices(unclipped, "D3D12");
    }
    {
      xenon::gpu::d3d12::Backend layouts;
      assert(layouts.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
      run_tiles_round_trip_between_layouts(layouts, "D3D12");
    }
    {
      xenon::gpu::d3d12::Backend dots;
      assert(dots.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
      run_dot_products_follow_xenos_zero_rule(dots, "D3D12");
    }
    {
      xenon::gpu::d3d12::Backend indexed;
      assert(indexed.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
      run_indexed_draws_track_index_buffer_writes(indexed, "D3D12");
    }
    {
      xenon::gpu::d3d12::Backend canonical;
      assert(canonical.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
      run_canonical_gpu_loads(canonical, "D3D12");
    }
    // GPU V3 parallel recording: the same draws, every run recorded on a
    // worker into a command list of its own and submitted in order, give the
    // same pixels.
    {
      xenon::gpu::d3d12::Backend parallel;
      assert(parallel.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
      assert(parallel.set_parallel_recording(2, 1));
      run_unclipped_clear_is_sized_to_its_vertices(parallel, "D3D12 (parallel recording)");
      run_dot_products_follow_xenos_zero_rule(parallel, "D3D12 (parallel recording)");
      run_shader_variant_prefetch(parallel, "D3D12");
      assert(parallel.parallel_runs_recorded() > 0u);
      // Every submitted batch is timed on the GPU.
      const auto timing = parallel.gpu_timing();
      assert(timing.batches > 0u && timing.gpu_ns > 0u);
      std::cout << "D3D12 GPU timestamps: " << timing.batches << " batches, "
                << timing.gpu_ns / 1000u << " us" << std::endl;
    }
    // GPU V3 persistent pipeline library: the pipelines one backend creates
    // are stored beside the shader cache and saved when it is destroyed; a
    // later backend loads every one of them instead of compiling it, and
    // draws the same pixels.
    {
      const auto library_directory =
          std::filesystem::temp_directory_path() / "xenon_d3d12_pipeline_library_test";
      std::error_code library_error;
      std::filesystem::remove_all(library_directory, library_error);
      std::uint64_t stored = 0;
      {
        xenon::gpu::d3d12::Backend first;
        assert(first.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
        first.set_shader_cache_directory(library_directory.string());
        run_dot_products_follow_xenos_zero_rule(first, "D3D12 (new pipeline library)");
        const auto created = first.pipeline_library_statistics();
        assert(created.open && created.loaded == 0u && created.stored > 0u);
        stored = created.stored;
      }
      const auto library_file = library_directory / "d3d12_pipelines.bin";
      assert(std::filesystem::exists(library_file) &&
             std::filesystem::file_size(library_file) > 0u);
      {
        xenon::gpu::d3d12::Backend reloaded;
        assert(reloaded.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
        reloaded.set_shader_cache_directory(library_directory.string());
        run_dot_products_follow_xenos_zero_rule(reloaded, "D3D12 (reloaded pipeline library)");
        const auto loaded = reloaded.pipeline_library_statistics();
        if (loaded.loaded != stored || loaded.stored != 0u) {
          std::cerr << "D3D12 pipeline library: " << stored << " stored, then " << loaded.loaded
                    << " loaded and " << loaded.stored << " stored again" << std::endl;
          assert(false);
        }
      }
      std::cout << "D3D12 persistent pipeline library: " << stored << " pipelines, "
                << std::filesystem::file_size(library_file) << " bytes" << std::endl;
      // A damaged library is refused and replaced, not trusted.
      {
        std::ofstream damaged(library_file, std::ios::binary | std::ios::trunc);
        damaged << "not a pipeline library";
      }
      {
        xenon::gpu::d3d12::Backend damaged;
        assert(damaged.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
        damaged.set_shader_cache_directory(library_directory.string());
        run_dot_products_follow_xenos_zero_rule(damaged, "D3D12 (damaged pipeline library)");
        const auto replaced = damaged.pipeline_library_statistics();
        assert(replaced.open && replaced.loaded == 0u && replaced.stored == stored);
      }
      std::filesystem::remove_all(library_directory, library_error);
    }
    run_converting_transfers<xenon::gpu::d3d12::Backend>(
        [](auto& b) {
          return b.initialize({.enable_debug_layer = false, .allow_software_adapter = false});
        },
        "D3D12");
    ran_backend = true;
  }
#endif

  if (!ran_backend) {
    std::cout << "xenon_backend_ownership_integration: no native backend built; "
                 "skipped\n";
  }
  return 0;
}
