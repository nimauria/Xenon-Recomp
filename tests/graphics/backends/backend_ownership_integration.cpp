#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

#include "xenon/gpu/backend_capabilities.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/resource_ir.hpp"
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
}

template <typename Backend>
void issue_depth_resolve(Backend& backend, std::uint32_t destination,
                         bool clear_after) {
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
  if (clear_after) write_register(backend, 0x231D, kDepthClear);
  issue_dummy_draw(backend);
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
                                                std::string_view name) {
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
  // This forces the native color owner back through canonical EDRAM, then
  // modifies only the resolve region and leaves that region canonical.
  issue_color_resolve(backend, kColorResolveDestination, true);
  assert_backend_ok(backend, "color resolve and clear");
  assert(resolved_word_at(memory, kColorResolveDestination, 0, 0) ==
         0xA5000000u);
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
  // resolves must still write the exact packed D24S8 bits. Then clear depth in
  // canonical EDRAM.
  issue_depth_resolve(backend, kDepthResolveDestination, true);
  assert_backend_ok(backend, "depth resolve and clear");
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
  assert(read_edram_sample(edram, depth_surface, 0, 0)[0] == kDepthClear);
  assert(read_edram_sample(edram, depth_surface, 15, 15)[0] == kDepthClear);
  assert(read_edram_sample(edram, depth_surface, 16, 0)[0] == 0xA5000038u);
  assert(read_edram_sample(edram, color_surface, 40, 0)[0] == kDepthClear);

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
          kDepthClear);
  assert(resolved_word_at(memory, kAliasResolveDestination, 55, 15) ==
          kDepthClear);

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
    xenon::gpu::vulkan::Backend fresh;
    assert(fresh.initialize({.enable_validation = false}));
    run_resolve_without_a_matching_render_target(fresh, "Vulkan");
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
    xenon::gpu::d3d12::Backend fresh;
    assert(fresh.initialize({.enable_debug_layer = false, .allow_software_adapter = false}));
    run_resolve_without_a_matching_render_target(fresh, "D3D12");
    ran_backend = true;
  }
#endif

  if (!ran_backend) {
    std::cout << "xenon_backend_ownership_integration: no native backend built; "
                 "skipped\n";
  }
  return 0;
}
