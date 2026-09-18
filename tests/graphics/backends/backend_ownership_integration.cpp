#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/memory/address_space.hpp"

#if defined(XENON_TEST_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
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
constexpr std::uint32_t kDestinationPitch = 32;
constexpr std::uint32_t kDestinationHeight = 32;
constexpr std::uint32_t kColorClear = 0x11223344u;
constexpr std::uint32_t kDepthClear = 0x55667788u;

void write_guest_bytes(xenon::memory::AddressSpace& memory,
                       std::uint32_t address, const void* source,
                       std::size_t size) {
  auto* destination = memory.physical_data(address);
  assert(destination);
  std::memcpy(destination, source, size);
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

  // Resolve from the real depth owner. The destination deliberately asks for a
  // color conversion, but depth resolves must write exact packed D24S8 bits.
  // Then clear depth in canonical EDRAM.
  issue_depth_resolve(backend, kDepthResolveDestination, true);
  assert_backend_ok(backend, "depth resolve and clear");
  assert(resolved_word_at(memory, kDepthResolveDestination, 0, 0) ==
         kColorClear);

  const EdramSurfaceLayout depth_surface{
      kSurfaceBaseTile, kSurfacePitch, kGrownHeight, MsaaSamples::X1, false,
      true};
  assert(read_edram_sample(edram, depth_surface, 0, 0)[0] == kDepthClear);
  assert(read_edram_sample(edram, depth_surface, 15, 15)[0] == kDepthClear);
  assert(read_edram_sample(edram, depth_surface, 16, 0)[0] == 0xA5000010u);

  // Transfer the modified depth alias back into the existing color target and
  // resolve it again. Seeing kDepthClear at the guest-memory boundary proves
  // depth -> canonical EDRAM -> color ownership preserved the exact bits.
  set_color_draw_state(backend, kGrownHeight);
  issue_dummy_draw(backend);
  assert_backend_ok(backend, "depth to color alias transfer");

  issue_color_resolve(backend, kAliasResolveDestination, false);
  assert_backend_ok(backend, "color resolve after depth alias");
  assert(resolved_word_at(memory, kAliasResolveDestination, 0, 0) ==
         kDepthClear);
  assert(resolved_word_at(memory, kAliasResolveDestination, 15, 15) ==
         kDepthClear);

  backend.end_submission();
  std::cout << name
            << " Backend::consume ownership/alias/resolve integration: ok\n";
}

}  // namespace

int main() {
  bool ran_backend = false;

#if defined(XENON_TEST_VULKAN)
  {
    xenon::gpu::vulkan::Backend backend;
    if (!backend.initialize({.enable_validation = false})) {
      std::cerr << "Vulkan backend initialization failed: " << backend.error()
                << '\n';
      assert(false);
    }
    run_backend_ownership_alias_resolve_clear(backend, "Vulkan");
    ran_backend = true;
  }
#endif

#if defined(XENON_TEST_D3D12)
  {
    xenon::gpu::d3d12::Backend backend;
    if (!backend.initialize({.enable_debug_layer = false,
                             .allow_software_adapter = false})) {
      std::cerr << "D3D12 backend initialization failed: " << backend.error()
                << '\n';
      assert(false);
    }
    run_backend_ownership_alias_resolve_clear(backend, "D3D12");
    ran_backend = true;
  }
#endif

  if (!ran_backend) {
    std::cout << "xenon_backend_ownership_integration: no native backend built; "
                 "skipped\n";
  }
  return 0;
}
