// Textures are loaded on the GPU (GpuTextureLoader): untiled, endian-swapped
// and converted by a compute pass straight from the guest-memory mirror, or
// from a sync-point copy of guest memory. Before, every refresh copied guest
// memory, decoded it on the CPU and uploaded it through a new image and a
// queue drain - in AC6's cutscenes that was most of each draw's cost.
//
// The GPU path must produce exactly what decode_texture() does. For every
// case below, on each backend, each subresource of the loaded image is read
// back and compared with the CPU decode: loaded from the mirror, from a copy,
// and reloaded in place after guest memory changed.
//
// Also covered: a resolve readback queued without waiting
// (GuestMemoryMirror::queue_cpu_visibility) keeps a CPU write that lands
// while it is in flight.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/gpu/texture.hpp"
#include "xenon/memory/address_space.hpp"

#if defined(XENON_TEST_D3D12)
#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/context.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/memory.hpp"
#include "xenon/gpu/d3d12/texture.hpp"
#endif

#if defined(XENON_TEST_VULKAN)
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/context.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/memory.hpp"
#include "xenon/gpu/vulkan/texture.hpp"
#endif

namespace {

using xenon::gpu::Endian;
using xenon::gpu::TextureDescriptor;
using xenon::gpu::TextureDimension;
using xenon::gpu::TextureLayout;
using xenon::gpu::TextureSubresourceLayout;

struct Case {
  std::string_view name;
  TextureDescriptor descriptor;
};

TextureDescriptor texture(std::uint32_t base, std::uint8_t format, std::uint32_t width,
                          std::uint32_t height, Endian endian, bool tiled) {
  TextureDescriptor d{};
  d.base_address = base;
  d.width = width;
  d.height = height;
  d.depth = 1;
  d.format = format;
  d.endian = endian;
  d.tiled = tiled;
  d.valid = true;
  return d;
}

std::vector<Case> cases() {
  std::vector<Case> result;
  auto rgba = texture(0x01000000u, 6, 64, 64, Endian::Swap8In32, true);
  rgba.mip_address = 0x01100000u;
  rgba.mip_max_level = 6;
  rgba.packed_mips = true;
  result.push_back({"tiled RGBA8, packed mips, 8in32", rgba});
  auto dxt = texture(0x01200000u, 18, 64, 64, Endian::Swap8In16, true);
  dxt.mip_address = 0x01300000u;
  dxt.mip_max_level = 4;
  dxt.packed_mips = true;
  result.push_back({"tiled DXT1, mips, 8in16", dxt});
  auto r8 = texture(0x01400000u, 2, 40, 24, Endian::None, false);
  r8.pitch = 64;
  result.push_back({"linear 8-bit, odd width", r8});
  result.push_back({"tiled 16-bit, 8in16", texture(0x01500000u, 24, 30, 20, Endian::Swap8In16, true)});
  result.push_back({"tiled 24_8 depth", texture(0x01600000u, 22, 32, 32, Endian::Swap8In32, true)});
  result.push_back({"tiled 24_8_FLOAT depth", texture(0x01700000u, 23, 32, 32, Endian::Swap8In32, true)});
  auto volume = texture(0x01800000u, 6, 16, 16, Endian::Swap8In32, true);
  volume.depth = 8;
  volume.dimension = TextureDimension::ThreeD;
  result.push_back({"tiled 3D RGBA8", volume});
  auto wide = texture(0x01900000u, 57, 8, 8, Endian::Swap8In32, false);
  wide.pitch = 32;
  result.push_back({"linear 32_32_32_FLOAT", wide});
  return result;
}

// Deterministic bytes over every subresource's guest range. Depth formats
// keep their 24-bit depth in [0, 1) territory loosely; any word decodes.
void fill(xenon::memory::AddressSpace& memory, const TextureLayout& layout, std::uint32_t seed) {
  for (const auto& sub : layout.subresources) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(sub.guest_size_bytes));
    std::uint32_t state = seed ^ sub.guest_address ^ 0x9E3779B9u;
    for (auto& byte : bytes) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      byte = static_cast<std::byte>(state & 0xFFu);
    }
    assert(memory.write_physical(sub.guest_address, bytes));
  }
}

// The guest bytes [base, end) the layout covers, as a sync point copies them.
std::vector<std::byte> copy_of(const xenon::memory::AddressSpace& memory,
                               const TextureLayout& layout, std::uint32_t& base) {
  base = UINT32_MAX;
  std::uint64_t end = 0;
  for (const auto& sub : layout.subresources) {
    base = (std::min)(base, sub.guest_address);
    end = (std::max)(end, std::uint64_t(sub.guest_address) + sub.guest_size_bytes);
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end - base));
  assert(memory.copy_physical_range(base, bytes));
  return bytes;
}

void compare(std::string_view backend, std::string_view name, std::string_view path,
             const xenon::gpu::DecodedTexture& expected, const TextureSubresourceLayout& sub,
             std::span<const std::byte> actual_rows, std::uint32_t actual_row_pitch,
             std::uint32_t actual_slice_pitch) {
  for (std::uint32_t z = 0; z < sub.depth_texels; ++z)
    for (std::uint32_t y = 0; y < sub.height_blocks; ++y) {
      const auto* want = expected.linear_data.data() + sub.linear_offset_bytes +
                         (std::uint64_t(z) * sub.height_blocks + y) * sub.linear_row_pitch_bytes;
      const auto* got = actual_rows.data() + std::uint64_t(z) * actual_slice_pitch +
                        std::uint64_t(y) * actual_row_pitch;
      if (std::memcmp(want, got, sub.linear_row_pitch_bytes) != 0) {
        std::cerr << backend << " " << name << " (" << path << "): mip " << sub.mip_level
                  << " layer " << sub.array_layer << " row " << y << " slice " << z
                  << " differs from decode_texture()\n";
        assert(false);
      }
    }
}

#if defined(XENON_TEST_D3D12)
std::vector<std::byte> read_d3d12(ID3D12Device* device, xenon::gpu::d3d12::CommandQueue& queue,
                                  xenon::gpu::d3d12::TextureImage& image,
                                  const TextureLayout& layout, const TextureSubresourceLayout& sub,
                                  std::uint32_t& row_pitch, std::uint32_t& slice_pitch) {
  const auto index = image.subresource_index(sub);
  auto placed = image.footprint(index);
  placed.Offset = 0;
  row_pitch = placed.Footprint.RowPitch;
  slice_pitch = row_pitch * (placed.Footprint.Height / layout.format.block_height);
  xenon::gpu::d3d12::Buffer readback;
  assert(readback.initialize(device, std::uint64_t(slice_pitch) * placed.Footprint.Depth,
                             D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
  const auto state = image.state();
  assert(queue.execute([&](ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image.resource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.resource();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = placed;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = image.resource();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = index;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
  }));
  std::span<std::byte> mapped;
  assert(readback.map(mapped));
  std::vector<std::byte> bytes(mapped.begin(), mapped.end());
  readback.unmap();
  return bytes;
}

void run_d3d12() {
  xenon::gpu::d3d12::Context context;
  if (!context.initialize({.enable_debug_layer = true})) {
    std::cout << "D3D12 unavailable; skipping its GPU texture load checks\n";
    return;
  }
  xenon::gpu::d3d12::CommandQueue queue;
  assert(queue.initialize(context.device()));
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  xenon::gpu::d3d12::GuestMemoryMirror mirror;
  assert(mirror.initialize(context.device(), queue, memory));
  xenon::gpu::d3d12::GpuTextureLoader loader;
  for (const auto& test : cases()) {
    const auto layout = xenon::gpu::build_texture_layout(test.descriptor);
    assert(layout.valid && layout.format.host_supported());
    const auto check = [&](xenon::gpu::d3d12::TextureImage& image, std::string_view path) {
      const auto expected = xenon::gpu::decode_texture(
          test.descriptor, {memory.physical_data(), xenon::memory::kPhysicalMemorySize});
      assert(expected.valid);
      for (const auto& sub : layout.subresources) {
        std::uint32_t row_pitch{}, slice_pitch{};
        const auto rows = read_d3d12(context.device(), queue, image, layout, sub, row_pitch,
                                     slice_pitch);
        compare("D3D12", test.name, path, expected, sub, rows, row_pitch, slice_pitch);
      }
    };
    fill(memory, layout, 1u);
    for (const auto& sub : layout.subresources)
      assert(mirror.synchronize_range(sub.guest_address,
                                      static_cast<std::uint32_t>(sub.guest_size_bytes)));
    xenon::gpu::d3d12::TextureImage image;
    if (!image.create(context.device(), layout)) {
      // A format the device cannot sample at all is a gap in the format
      // mapping, not in the load path; say so, and check the rest.
      assert(image.error().find("cannot sample") != std::string::npos);
      std::cout << "D3D12 cannot sample " << test.name << "; its load is not checked here\n";
      continue;
    }
    assert(loader.load_from_mirror(context.device(), queue, mirror, image, layout));
    assert(queue.flush() && queue.wait_idle());
    check(image, "mirror");

    // Reloaded in place after the guest rewrote it.
    fill(memory, layout, 2u);
    for (const auto& sub : layout.subresources)
      assert(mirror.synchronize_range(sub.guest_address,
                                      static_cast<std::uint32_t>(sub.guest_size_bytes)));
    assert(image.matches(layout));
    assert(loader.load_from_mirror(context.device(), queue, mirror, image, layout));
    assert(queue.flush() && queue.wait_idle());
    check(image, "mirror reload");

    std::uint32_t base{};
    const auto bytes = copy_of(memory, layout, base);
    xenon::gpu::d3d12::TextureImage copied;
    assert(copied.create(context.device(), layout));
    assert(loader.load_from_bytes(context.device(), queue, copied, layout, bytes, base));
    assert(queue.flush() && queue.wait_idle());
    check(copied, "sync-point copy");
  }

  // A resolve readback in flight keeps a CPU write that lands meanwhile.
  constexpr std::uint32_t kRange = 0x02000000u;
  std::array<std::byte, 4096> gpu_bytes{};
  gpu_bytes.fill(std::byte{0x5A});
  assert(memory.write_physical(kRange, gpu_bytes));
  assert(mirror.synchronize_range(kRange, 4096u));
  std::array<std::byte, 4096> stale{};
  stale.fill(std::byte{0x11});
  assert(memory.write_physical(kRange, stale));
  // The CPU writes before a resolve are accounted for before it runs (a
  // resolve synchronizes its range first); nothing is GPU-owned yet.
  assert(mirror.queue_cpu_visibility(kRange, 4096u));
  assert(!mirror.cpu_visibility_pending());
  mirror.mark_gpu_write(kRange, 4096u);  // the GPU owns the 0x5A bytes now
  assert(mirror.queue_cpu_visibility(kRange, 4096u));
  assert(mirror.cpu_visibility_pending());
  const std::array<std::byte, 4> cpu_write{std::byte{1}, std::byte{2}, std::byte{3},
                                           std::byte{4}};
  assert(memory.write_physical(kRange + 8u, cpu_write));
  assert(mirror.complete_cpu_visibility(true));
  assert(!mirror.cpu_visibility_pending());
  std::array<std::byte, 16> landed{};
  assert(memory.copy_physical_range(kRange, landed));
  assert(landed[0] == std::byte{0x5A} && landed[7] == std::byte{0x5A});
  assert(landed[8] == std::byte{1} && landed[11] == std::byte{4});
  assert(landed[12] == std::byte{0x5A});
  std::cout << "D3D12 GPU texture loads match decode_texture()\n";
}
#endif

#if defined(XENON_TEST_VULKAN)
std::vector<std::byte> read_vulkan(xenon::gpu::vulkan::Context& context,
                                   xenon::gpu::vulkan::CommandQueue& queue,
                                   xenon::gpu::vulkan::TextureImage& image,
                                   const TextureLayout& layout,
                                   const TextureSubresourceLayout& sub) {
  xenon::gpu::vulkan::Buffer readback;
  const auto size = std::uint64_t(sub.linear_row_pitch_bytes) * sub.height_blocks *
                    sub.depth_texels;
  assert(readback.initialize(context.physical_device(), context.device(), size,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
  assert(queue.execute([&](VkCommandBuffer command) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = image.layout();
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image();
    barrier.subresourceRange = {image.aspect(), 0, image.mip_levels(), 0, image.layers()};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
    VkBufferImageCopy copy{};
    copy.bufferRowLength = sub.width_blocks * layout.format.block_width;
    copy.bufferImageHeight = sub.height_blocks * layout.format.block_height;
    copy.imageSubresource = {image.aspect(), sub.mip_level, sub.array_layer, 1u};
    copy.imageExtent = {sub.width_texels, sub.height_texels, sub.depth_texels};
    vkCmdCopyImageToBuffer(command, image.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(), 1, &copy);
    std::swap(barrier.oldLayout, barrier.newLayout);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    vkCmdPipelineBarrier2(command, &dependency);
  }));
  std::span<std::byte> mapped;
  assert(readback.map(mapped));
  std::vector<std::byte> bytes(mapped.begin(), mapped.begin() + size);
  readback.unmap();
  return bytes;
}

void run_vulkan() {
  xenon::gpu::vulkan::Context context;
  if (!context.initialize({.enable_validation = true})) {
    std::cout << "Vulkan unavailable; skipping its GPU texture load checks\n";
    return;
  }
  xenon::gpu::vulkan::CommandQueue queue;
  assert(queue.initialize(context.device(), context.graphics_queue(),
                          context.graphics_queue_family()));
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  xenon::gpu::vulkan::GuestMemoryMirror mirror;
  assert(mirror.initialize(context.physical_device(), context.device(), queue, memory));
  xenon::gpu::vulkan::GpuTextureLoader loader;
  for (const auto& test : cases()) {
    const auto layout = xenon::gpu::build_texture_layout(test.descriptor);
    assert(layout.valid && layout.format.host_supported());
    const auto check = [&](xenon::gpu::vulkan::TextureImage& image, std::string_view path) {
      const auto expected = xenon::gpu::decode_texture(
          test.descriptor, {memory.physical_data(), xenon::memory::kPhysicalMemorySize});
      assert(expected.valid);
      for (const auto& sub : layout.subresources) {
        const auto rows = read_vulkan(context, queue, image, layout, sub);
        compare("Vulkan", test.name, path, expected, sub, rows, sub.linear_row_pitch_bytes,
                sub.linear_row_pitch_bytes * sub.height_blocks);
      }
    };
    fill(memory, layout, 1u);
    for (const auto& sub : layout.subresources)
      assert(mirror.synchronize_range(sub.guest_address,
                                      static_cast<std::uint32_t>(sub.guest_size_bytes)));
    xenon::gpu::vulkan::TextureImage image;
    if (!image.create(context.physical_device(), context.device(), layout)) {
      assert(image.error().find("cannot sample") != std::string::npos);
      std::cout << "Vulkan cannot sample " << test.name << "; its load is not checked here\n";
      continue;
    }
    assert(loader.load_from_mirror(context.physical_device(), context.device(), queue, mirror,
                                   image, layout));
    assert(queue.flush() && queue.wait_idle());
    check(image, "mirror");

    fill(memory, layout, 2u);
    for (const auto& sub : layout.subresources)
      assert(mirror.synchronize_range(sub.guest_address,
                                      static_cast<std::uint32_t>(sub.guest_size_bytes)));
    assert(image.matches(layout));
    assert(loader.load_from_mirror(context.physical_device(), context.device(), queue, mirror,
                                   image, layout));
    assert(queue.flush() && queue.wait_idle());
    check(image, "mirror reload");

    std::uint32_t base{};
    const auto bytes = copy_of(memory, layout, base);
    xenon::gpu::vulkan::TextureImage copied;
    assert(copied.create(context.physical_device(), context.device(), layout));
    assert(loader.load_from_bytes(context.physical_device(), context.device(), queue, copied,
                                  layout, bytes, base));
    assert(queue.flush() && queue.wait_idle());
    check(copied, "sync-point copy");
  }

  constexpr std::uint32_t kRange = 0x02000000u;
  std::array<std::byte, 4096> gpu_bytes{};
  gpu_bytes.fill(std::byte{0x5A});
  assert(memory.write_physical(kRange, gpu_bytes));
  assert(mirror.synchronize_range(kRange, 4096u));
  std::array<std::byte, 4096> stale{};
  stale.fill(std::byte{0x11});
  assert(memory.write_physical(kRange, stale));
  assert(mirror.queue_cpu_visibility(kRange, 4096u));
  assert(!mirror.cpu_visibility_pending());
  mirror.mark_gpu_write(kRange, 4096u);
  assert(mirror.queue_cpu_visibility(kRange, 4096u));
  assert(mirror.cpu_visibility_pending());
  const std::array<std::byte, 4> cpu_write{std::byte{1}, std::byte{2}, std::byte{3},
                                           std::byte{4}};
  assert(memory.write_physical(kRange + 8u, cpu_write));
  assert(mirror.complete_cpu_visibility(true));
  assert(!mirror.cpu_visibility_pending());
  std::array<std::byte, 16> landed{};
  assert(memory.copy_physical_range(kRange, landed));
  assert(landed[0] == std::byte{0x5A} && landed[7] == std::byte{0x5A});
  assert(landed[8] == std::byte{1} && landed[11] == std::byte{4});
  assert(landed[12] == std::byte{0x5A});
  std::cout << "Vulkan GPU texture loads match decode_texture()\n";
}
#endif

}  // namespace

int main() {
#if defined(XENON_TEST_D3D12)
  run_d3d12();
#endif
#if defined(XENON_TEST_VULKAN)
  run_vulkan();
#endif
  return 0;
}
