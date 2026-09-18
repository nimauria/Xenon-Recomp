#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

#include "xenon/gpu/depth_format.hpp"
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
constexpr std::uint32_t kSurfacePitch = 64;
constexpr std::uint32_t kSurfaceHeight = 16;
constexpr std::uint32_t kResolveExtent = 16;
constexpr std::uint32_t kDestinationPitch = 64;
constexpr std::uint32_t kDestinationHeight = 32;
constexpr std::uint32_t kColorResolveBase = 0x00100000;
constexpr std::uint32_t kDepthResolveBase = 0x00300000;
constexpr std::uint32_t kAliasResolveBase = 0x00500000;
constexpr std::uint32_t kResolveStride = 0x00010000;
constexpr std::uint32_t kColorClear = 0x4C3B2A19u;
constexpr std::uint32_t kDepthClear = 0xA7D4C321u;

unsigned sample_count(MsaaSamples samples) {
  return 1u << static_cast<unsigned>(samples);
}

void write_guest_bytes(xenon::memory::AddressSpace& memory,
                       std::uint32_t address, const void* source,
                       std::size_t size) {
  const auto bytes = std::span{
      static_cast<const std::byte*>(source), size};
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
void set_common_surface(Backend& backend, MsaaSamples samples) {
  write_register(backend, 0x2000,
                 kSurfacePitch |
                     (static_cast<std::uint32_t>(samples) << 16u));
  write_register(backend, 0x200E, 0u);
  write_register(backend, 0x200F,
                 kSurfacePitch | (kSurfaceHeight << 16u));
}

template <typename Backend>
void set_color_draw_state(
    Backend& backend, std::uint32_t base_tile, MsaaSamples samples,
    ColorRenderTargetFormat format = ColorRenderTargetFormat::R8G8B8A8) {
  set_common_surface(backend, samples);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::ColorDepth));
  write_register(backend, 0x2104, 0xFu);
  write_register(backend, 0x2001,
                 base_tile | (static_cast<std::uint32_t>(format) << 16u));
  write_register(backend, 0x2200, 0u);
}

template <typename Backend>
void set_depth_draw_state(Backend& backend, std::uint32_t base_tile,
                          MsaaSamples samples,
                          DepthRenderTargetFormat format) {
  set_common_surface(backend, samples);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::DepthOnly));
  write_register(backend, 0x2104, 0u);
  write_register(backend, 0x2002,
                 base_tile | (static_cast<std::uint32_t>(format) << 16u));
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
void set_resolve_destination(Backend& backend, std::uint32_t address) {
  write_register(backend, 0x2319, address);
  write_register(backend, 0x231A,
                 kDestinationPitch | (kDestinationHeight << 16u));
  write_register(backend, 0x231B, 6u << 7u);
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
void issue_color_resolve(Backend& backend, MsaaSamples samples,
                         CopySampleSelect selection,
                         std::uint32_t destination, bool clear_after = false) {
  set_common_surface(backend, samples);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318,
                 (static_cast<std::uint32_t>(selection) << 4u) |
                     (clear_after ? (1u << 8u) : 0u) |
                     (static_cast<std::uint32_t>(CopyCommand::Raw) << 20u));
  set_resolve_destination(backend, destination);
  if (clear_after) {
    write_register(backend, 0x231E, kColorClear);
    write_register(backend, 0x231F, 0u);
  }
  issue_dummy_draw(backend);
}

template <typename Backend>
void issue_depth_resolve(Backend& backend, MsaaSamples samples,
                         CopySampleSelect selection,
                         std::uint32_t destination, bool clear_after = false) {
  set_common_surface(backend, samples);
  set_resolve_fetch(backend);
  write_register(backend, 0x2208,
                 static_cast<std::uint32_t>(EdramMode::Copy));
  write_register(backend, 0x2318,
                 4u | (static_cast<std::uint32_t>(selection) << 4u) |
                     (clear_after ? (1u << 9u) : 0u) |
                     (static_cast<std::uint32_t>(CopyCommand::Convert) << 20u));
  // Deliberately use color-style destination controls. The depth resolve path
  // must use the actual depth format and preserve the canonical packed word.
  write_register(backend, 0x2319, destination);
  write_register(backend, 0x231A,
                 kDestinationPitch | (kDestinationHeight << 16u));
  write_register(backend, 0x231B,
                 (6u << 7u) | (7u << 16u) | (1u << 24u));
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

std::uint32_t color_pattern(std::uint32_t sample, std::uint32_t x,
                            std::uint32_t y) {
  return ((0x90u + sample) << 24u) | ((0x30u + y) << 16u) |
         ((0x50u + x) << 8u) | (0x10u + sample);
}

std::uint32_t averaged_color_pattern(std::uint32_t first_sample,
                                     std::uint32_t second_sample,
                                     std::uint32_t x, std::uint32_t y) {
  const auto first = unpack_color_sample(
      ColorRenderTargetFormat::R8G8B8A8,
      {color_pattern(first_sample, x, y), 0u});
  const auto second = unpack_color_sample(
      ColorRenderTargetFormat::R8G8B8A8,
      {color_pattern(second_sample, x, y), 0u});
  ColorSample average{};
  for (std::size_t component = 0; component < average.components.size();
       ++component)
    average.components[component] =
        (first.components[component] + second.components[component]) * 0.5f;
  return pack_color_sample(ColorRenderTargetFormat::R8G8B8A8, average)[0];
}

std::array<std::uint32_t, 2> wide_color_pattern(
    std::uint32_t sample, std::uint32_t x, std::uint32_t y) {
  const std::uint16_t r = static_cast<std::uint16_t>(0x3C00u + sample);
  const std::uint16_t g = static_cast<std::uint16_t>(0x3800u + (x & 0xFu));
  const std::uint16_t b = static_cast<std::uint16_t>(0x3400u + (y & 0xFu));
  const std::uint16_t a = static_cast<std::uint16_t>(0x4000u + sample);
  return {std::uint32_t(r) | (std::uint32_t(g) << 16u),
          std::uint32_t(b) | (std::uint32_t(a) << 16u)};
}

std::uint32_t depth_pattern(DepthRenderTargetFormat format,
                            std::uint32_t sample, std::uint32_t x,
                            std::uint32_t y) {
  const auto depth =
      format == DepthRenderTargetFormat::D24FS8
          ? (0xF00000u + sample * 0x10000u + y * 0x100u + x)
          : (0x180000u + sample * 0x20000u + y * 0x100u + x);
  const auto stencil = 0x50u + sample;
  return (stencil << 24u) | (depth & 0x00FFFFFFu);
}

template <typename Backend>
void assert_backend_ok(const Backend& backend, std::string_view phase) {
  if (!backend.error().empty())
    std::cerr << phase << ": " << backend.error() << '\n';
  assert(backend.error().empty());
  assert(backend.ready());
}

void seed_color(Edram& edram, std::uint32_t base_tile, MsaaSamples samples) {
  const EdramSurfaceLayout surface{base_tile, kSurfacePitch, kSurfaceHeight,
                                   samples, false, false};
  for (std::uint32_t y = 0; y < kResolveExtent; ++y)
    for (std::uint32_t x = 0; x < kSurfacePitch; ++x)
      for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample)
        assert(write_edram_sample(edram, surface, x, y,
                                  {color_pattern(sample, x, y), 0u}, sample));
}

void seed_depth(Edram& edram, std::uint32_t base_tile, MsaaSamples samples,
                DepthRenderTargetFormat format) {
  const EdramSurfaceLayout surface{base_tile, kSurfacePitch, kSurfaceHeight,
                                   samples, false, true};
  for (std::uint32_t y = 0; y < kResolveExtent; ++y)
    for (std::uint32_t x = 0; x < kResolveExtent; ++x)
      for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample)
        assert(write_edram_sample(edram, surface, x, y,
                                  {depth_pattern(format, sample, x, y), 0u},
                                  sample));
}

template <typename Producer, typename Consumer>
void run_color_handoff(Producer& producer, Consumer& consumer,
                       xenon::memory::AddressSpace& memory, Edram& edram,
                       std::uint32_t base_tile, MsaaSamples samples,
                       std::uint32_t resolve_base, std::string_view direction) {
  seed_color(edram, base_tile, samples);

  producer.begin_submission(memory, edram);
  set_color_draw_state(producer, base_tile, samples);
  issue_dummy_draw(producer);
  assert_backend_ok(producer, "producer color acquire");
  assert(producer.make_edram_canonical());
  assert_backend_ok(producer, "producer color canonicalize");
  producer.end_submission();

  consumer.begin_submission(memory, edram);
  set_color_draw_state(consumer, base_tile, samples);
  issue_dummy_draw(consumer);
  assert_backend_ok(consumer, "consumer color acquire");
  for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample) {
    const auto destination = resolve_base + sample * kResolveStride;
    issue_color_resolve(consumer, samples,
                        static_cast<CopySampleSelect>(sample), destination);
    assert_backend_ok(consumer, "consumer selected color resolve");
    assert(resolved_word_at(memory, destination, 0, 0) ==
           color_pattern(sample, 0, 0));
    assert(resolved_word_at(memory, destination, 15, 15) ==
           color_pattern(sample, 15, 15));
  }
  if (samples != MsaaSamples::X1) {
    const auto pair01_destination = resolve_base + 4u * kResolveStride;
    issue_color_resolve(consumer, samples, CopySampleSelect::Samples01,
                        pair01_destination);
    assert_backend_ok(consumer, "consumer Samples01 color resolve");
    assert(resolved_word_at(memory, pair01_destination, 0, 0) ==
           averaged_color_pattern(0, 1, 0, 0));
    assert(resolved_word_at(memory, pair01_destination, 15, 15) ==
           averaged_color_pattern(0, 1, 15, 15));
  }
  if (samples == MsaaSamples::X4) {
    const auto pair23_destination = resolve_base + 5u * kResolveStride;
    issue_color_resolve(consumer, samples, CopySampleSelect::Samples23,
                        pair23_destination);
    assert_backend_ok(consumer, "consumer Samples23 color resolve");
    assert(resolved_word_at(memory, pair23_destination, 0, 0) ==
           averaged_color_pattern(2, 3, 0, 0));
    assert(resolved_word_at(memory, pair23_destination, 15, 15) ==
           averaged_color_pattern(2, 3, 15, 15));
  }

  assert(consumer.make_edram_canonical());
  consumer.end_submission();

  std::cout << direction << " color " << sample_count(samples)
            << "x canonical handoff: ok\n";
}

template <typename Producer, typename Consumer>
void run_depth_handoff(Producer& producer, Consumer& consumer,
                       xenon::memory::AddressSpace& memory, Edram& edram,
                       std::uint32_t base_tile, MsaaSamples samples,
                       DepthRenderTargetFormat format,
                       std::uint32_t resolve_base,
                       std::string_view direction) {
  seed_depth(edram, base_tile, samples, format);

  producer.begin_submission(memory, edram);
  set_depth_draw_state(producer, base_tile, samples, format);
  issue_dummy_draw(producer);
  assert_backend_ok(producer, "producer depth acquire");
  assert(producer.make_edram_canonical());
  assert_backend_ok(producer, "producer depth canonicalize");
  producer.end_submission();

  consumer.begin_submission(memory, edram);
  set_depth_draw_state(consumer, base_tile, samples, format);
  issue_dummy_draw(consumer);
  assert_backend_ok(consumer, "consumer depth acquire");
  for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample) {
    const auto destination = resolve_base + sample * kResolveStride;
    issue_depth_resolve(consumer, samples,
                        static_cast<CopySampleSelect>(sample), destination);
    assert_backend_ok(consumer, "consumer selected depth resolve");
    assert(resolved_word_at(memory, destination, 0, 0) ==
           depth_pattern(format, sample, 0, 0));
    assert(resolved_word_at(memory, destination, 15, 15) ==
           depth_pattern(format, sample, 15, 15));
  }
  if (samples == MsaaSamples::X2) {
    const auto combined_destination = resolve_base + 4u * kResolveStride;
    issue_depth_resolve(consumer, samples, CopySampleSelect::Samples01,
                        combined_destination);
    assert_backend_ok(consumer, "consumer sanitized X2 depth resolve");
    assert(resolved_word_at(memory, combined_destination, 0, 0) ==
           depth_pattern(format, 0, 0, 0));
  } else if (samples == MsaaSamples::X4) {
    const auto all_destination = resolve_base + 4u * kResolveStride;
    issue_depth_resolve(consumer, samples, CopySampleSelect::Samples0123,
                        all_destination);
    assert_backend_ok(consumer, "consumer sanitized X4 all-sample depth resolve");
    assert(resolved_word_at(memory, all_destination, 0, 0) ==
           depth_pattern(format, 0, 0, 0));
    const auto pair_destination = resolve_base + 5u * kResolveStride;
    issue_depth_resolve(consumer, samples, CopySampleSelect::Samples23,
                        pair_destination);
    assert_backend_ok(consumer, "consumer sanitized X4 pair depth resolve");
    assert(resolved_word_at(memory, pair_destination, 0, 0) ==
           depth_pattern(format, 2, 0, 0));
  }

  assert(consumer.make_edram_canonical());
  consumer.end_submission();

  std::cout << direction << " "
            << (format == DepthRenderTargetFormat::D24FS8 ? "D24FS8 "
                                                           : "D24S8 ")
            << sample_count(samples) << "x canonical handoff: ok\n";
}

void seed_wide_color(Edram& edram, std::uint32_t base_tile,
                     MsaaSamples samples) {
  const EdramSurfaceLayout surface{base_tile, kSurfacePitch, kSurfaceHeight,
                                   samples, true, false};
  for (std::uint32_t y = 0; y < kResolveExtent; ++y)
    for (std::uint32_t x = 0; x < kResolveExtent; ++x)
      for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample)
        assert(write_edram_sample(edram, surface, x, y,
                                  wide_color_pattern(sample, x, y), sample));
}

template <typename Producer, typename Consumer>
void run_wide_color_handoff(Producer& producer, Consumer& consumer,
                            xenon::memory::AddressSpace& memory, Edram& edram,
                            std::uint32_t base_tile, MsaaSamples samples,
                            std::string_view direction) {
  seed_wide_color(edram, base_tile, samples);
  const EdramSurfaceLayout surface{base_tile, kSurfacePitch, kSurfaceHeight,
                                   samples, true, false};

  producer.begin_submission(memory, edram);
  set_color_draw_state(producer, base_tile, samples,
                       ColorRenderTargetFormat::R16G16B16A16Float);
  issue_dummy_draw(producer);
  assert_backend_ok(producer, "producer wide-color acquire");
  assert(producer.make_edram_canonical());
  producer.end_submission();
  for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample) {
    assert(read_edram_sample(edram, surface, 0, 0, sample) ==
           wide_color_pattern(sample, 0, 0));
    assert(read_edram_sample(edram, surface, 15, 15, sample) ==
           wide_color_pattern(sample, 15, 15));
  }

  consumer.begin_submission(memory, edram);
  set_color_draw_state(consumer, base_tile, samples,
                       ColorRenderTargetFormat::R16G16B16A16Float);
  issue_dummy_draw(consumer);
  assert_backend_ok(consumer, "consumer wide-color acquire");
  assert(consumer.make_edram_canonical());
  consumer.end_submission();
  for (std::uint32_t sample = 0; sample < sample_count(samples); ++sample) {
    assert(read_edram_sample(edram, surface, 0, 0, sample) ==
           wide_color_pattern(sample, 0, 0));
    assert(read_edram_sample(edram, surface, 15, 15, sample) ==
           wide_color_pattern(sample, 15, 15));
  }

  std::cout << direction << " 64bpp color " << sample_count(samples)
            << "x canonical handoff: ok\n";
}

template <typename First, typename Second>
void run_alias_clear_ping_pong(First& first, Second& second,
                               xenon::memory::AddressSpace& memory,
                               Edram& edram, std::uint32_t base_tile,
                               std::string_view direction) {
  constexpr auto samples = MsaaSamples::X1;
  seed_color(edram, base_tile, samples);

  first.begin_submission(memory, edram);
  set_color_draw_state(first, base_tile, samples);
  issue_dummy_draw(first);
  issue_color_resolve(first, samples, CopySampleSelect::Sample0,
                      kAliasResolveBase, true);
  assert_backend_ok(first, "first backend color resolve/clear");
  assert(resolved_word_at(memory, kAliasResolveBase, 0, 0) ==
         color_pattern(0, 0, 0));
  assert(first.make_edram_canonical());
  first.end_submission();

  second.begin_submission(memory, edram);
  set_depth_draw_state(second, base_tile, samples,
                       DepthRenderTargetFormat::D24S8);
  issue_dummy_draw(second);
  issue_depth_resolve(second, samples, CopySampleSelect::Sample0,
                      kAliasResolveBase + kResolveStride, false);
  assert_backend_ok(second, "second backend color-to-depth alias");
  // Xenos depth tiles exchange their two 40-sample halves. Depth X=0 aliases
  // color X=40, not the color-cleared X=0 region.
  assert(resolved_word_at(memory, kAliasResolveBase + kResolveStride, 0, 0) ==
         color_pattern(0, 40, 0));
  issue_depth_resolve(second, samples, CopySampleSelect::Sample0,
                      kAliasResolveBase + 2u * kResolveStride, true);
  assert_backend_ok(second, "second backend depth resolve/clear");
  assert(second.make_edram_canonical());
  second.end_submission();

  first.begin_submission(memory, edram);
  set_color_draw_state(first, base_tile, samples);
  issue_dummy_draw(first);
  const std::array<float, 6> depth_alias_vertices{
      39.5f, -0.5f, 55.5f, -0.5f, 39.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, depth_alias_vertices.data(),
                    sizeof(depth_alias_vertices));
  issue_color_resolve(first, samples, CopySampleSelect::Sample0,
                      kAliasResolveBase + 3u * kResolveStride, false);
  assert_backend_ok(first, "first backend depth-to-color alias");
  assert(resolved_word_at(memory, kAliasResolveBase + 3u * kResolveStride,
                          40, 0) == kDepthClear);
  assert(first.make_edram_canonical());
  first.end_submission();

  const std::array<float, 6> default_vertices{
      -0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, default_vertices.data(),
                    sizeof(default_vertices));

  std::cout << direction << " alias/resolve/clear ping-pong: ok\n";
}

#if defined(XENON_TEST_VULKAN) && defined(XENON_TEST_D3D12)
void run_cross_backend_suite(xenon::gpu::vulkan::Backend& vulkan_backend,
                             xenon::gpu::d3d12::Backend& d3d12_backend) {
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  const std::array<float, 6> resolve_vertices{
      -0.5f, -0.5f, 15.5f, -0.5f, -0.5f, 15.5f};
  write_guest_bytes(memory, kResolveVertices, resolve_vertices.data(),
                    sizeof(resolve_vertices));

  Edram edram;
  edram.reset();

  std::uint32_t color_base = 64;
  std::uint32_t color_resolve = kColorResolveBase;
  for (const auto samples : {MsaaSamples::X1, MsaaSamples::X2,
                             MsaaSamples::X4}) {
    run_color_handoff(vulkan_backend, d3d12_backend, memory, edram,
                      color_base, samples, color_resolve,
                      "Vulkan -> D3D12");
    color_base += 32;
    color_resolve += 8u * kResolveStride;
    run_color_handoff(d3d12_backend, vulkan_backend, memory, edram,
                      color_base, samples, color_resolve,
                      "D3D12 -> Vulkan");
    color_base += 32;
    color_resolve += 8u * kResolveStride;
  }

  std::uint32_t depth_base = 320;
  std::uint32_t depth_resolve = kDepthResolveBase;
  for (const auto format : {DepthRenderTargetFormat::D24S8,
                            DepthRenderTargetFormat::D24FS8}) {
    for (const auto samples : {MsaaSamples::X1, MsaaSamples::X2,
                               MsaaSamples::X4}) {
      run_depth_handoff(vulkan_backend, d3d12_backend, memory, edram,
                        depth_base, samples, format, depth_resolve,
                        "Vulkan -> D3D12");
      depth_base += 32;
      depth_resolve += 8u * kResolveStride;
      run_depth_handoff(d3d12_backend, vulkan_backend, memory, edram,
                        depth_base, samples, format, depth_resolve,
                        "D3D12 -> Vulkan");
      depth_base += 32;
      depth_resolve += 8u * kResolveStride;
    }
  }

  run_wide_color_handoff(vulkan_backend, d3d12_backend, memory, edram, 1024,
                         MsaaSamples::X4, "Vulkan -> D3D12");
  run_wide_color_handoff(d3d12_backend, vulkan_backend, memory, edram, 1088,
                         MsaaSamples::X4, "D3D12 -> Vulkan");

  run_alias_clear_ping_pong(vulkan_backend, d3d12_backend, memory, edram, 768,
                            "Vulkan -> D3D12 -> Vulkan");
  run_alias_clear_ping_pong(d3d12_backend, vulkan_backend, memory, edram, 832,
                            "D3D12 -> Vulkan -> D3D12");
}
#endif

}  // namespace

int main() {
#if defined(XENON_TEST_VULKAN) && defined(XENON_TEST_D3D12)
  xenon::gpu::vulkan::Backend vulkan_backend;
  if (!vulkan_backend.initialize({.enable_validation = false})) {
    std::cerr << "Vulkan backend initialization failed: "
              << vulkan_backend.error() << '\n';
    assert(false);
  }
  xenon::gpu::d3d12::Backend d3d12_backend;
  if (!d3d12_backend.initialize({.enable_debug_layer = false,
                                 .allow_software_adapter = false})) {
    std::cerr << "D3D12 backend initialization failed: "
              << d3d12_backend.error() << '\n';
    assert(false);
  }
  run_cross_backend_suite(vulkan_backend, d3d12_backend);
  std::cout << "xenon_cross_backend_canonical_tests: ok\n";
#else
  std::cout << "xenon_cross_backend_canonical_tests: both Vulkan and D3D12 "
               "are required; skipped\n";
#endif
  return 0;
}
