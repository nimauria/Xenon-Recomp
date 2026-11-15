#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "xenon/gpu/primitive_processor.hpp"

using namespace xenon::gpu;

int main() {
  std::array<std::byte, 64> memory{};
  const std::array<std::uint16_t, 4> dma_indices{0x0100, 0x0200, 0x0300, 0x0400};
  std::memcpy(memory.data() + 8, dma_indices.data(), sizeof(dma_indices));
  ir::DrawPacket draw{};
  draw.source = DrawSource::Dma;
  draw.primitive_type = PrimitiveType::TriangleStrip;
  draw.index_count = 4;
  draw.index_buffer = {true, 8, 8, 4, IndexFormat::UInt16, Endian::Swap8In16};
  auto batch = process_primitives(draw, memory);
  assert(batch.valid && batch.indexed);
  assert((batch.indices == std::vector<std::uint32_t>{1, 2, 3, 3, 2, 4}));
  const auto snapshot_batch = process_primitives(
      draw, std::span<const std::byte>(memory).subspan(8, 8), {}, 8);
  assert(snapshot_batch.valid && snapshot_batch.indexed);
  assert(snapshot_batch.indices == batch.indices);

  draw = {};
  draw.source = DrawSource::Immediate;
  draw.primitive_type = PrimitiveType::QuadList;
  draw.index_format = IndexFormat::UInt16;
  draw.index_count = 4;
  draw.immediate_index_dwords = {1u | (2u << 16u), 3u | (4u << 16u)};
  batch = process_primitives(draw, memory);
  assert(batch.valid);
  assert((batch.indices == std::vector<std::uint32_t>{1, 2, 3, 1, 3, 4}));

  draw = {};
  draw.source = DrawSource::AutoIndex;
  draw.primitive_type = PrimitiveType::LineLoop;
  draw.index_count = 3;
  batch = process_primitives(draw, memory);
  assert(batch.valid && batch.indexed);
  assert((batch.indices == std::vector<std::uint32_t>{0, 1, 1, 2, 2, 0}));

  draw = {};
  draw.source = DrawSource::Immediate;
  draw.primitive_type = PrimitiveType::TriangleStrip;
  draw.index_format = IndexFormat::UInt16;
  draw.index_count = 7;
  draw.immediate_index_dwords = {
      0u | (1u << 16u), 2u | (0xFFFFu << 16u),
      4u | (5u << 16u), 6u | (0x7777u << 16u)};
  batch = process_primitives(draw, memory, {true, 0xFFFFu});
  assert(batch.valid && batch.indexed);
  // Restart begins a fresh strip, including a fresh winding-parity sequence.
  assert((batch.indices == std::vector<std::uint32_t>{0, 1, 2, 4, 5, 6}));

  // Primitive reset is ignored for list topologies even if the register enable
  // is set, matching Xenos/Vulkan-safe semantics.
  draw.primitive_type = PrimitiveType::LineList;
  batch = process_primitives(draw, memory, {true, 0xFFFFu});
  assert(batch.valid);
  assert((batch.indices ==
          std::vector<std::uint32_t>{0, 1, 2, 0xFFFFu, 4, 5}));

  draw = {};
  draw.source = DrawSource::AutoIndex;
  draw.primitive_type = PrimitiveType::RectangleList;
  draw.index_count = 3;
  batch = process_primitives(draw, memory);
  assert(batch.valid && !batch.indexed && batch.requires_rectangle_expansion);
  assert(batch.topology == HostPrimitiveTopology::TriangleList);
  assert((batch.indices == std::vector<std::uint32_t>{0, 1, 2}));
  const std::vector<std::uint32_t> rectangle_indices{0, 1, 2};
  for (const auto primitive : {PrimitiveType::CopyRectList0,
                               PrimitiveType::CopyRectList1,
                               PrimitiveType::CopyRectList2,
                               PrimitiveType::CopyRectList3,
                               PrimitiveType::FillRectList}) {
    draw.primitive_type = primitive;
    batch = process_primitives(draw, memory);
    assert(batch.valid && batch.requires_rectangle_expansion);
    assert(batch.indices == rectangle_indices);
  }

  // 32-bit DMA indices, byte-swapped and masked to 24 bits, as a strip.
  {
    std::array<std::byte, 32> words{};
    const std::array<std::uint32_t, 4> big_endian{0x07000000u, 0x08000000u, 0x090000FFu,
                                                  0x0A000000u};
    std::memcpy(words.data(), big_endian.data(), sizeof(big_endian));
    draw = {};
    draw.source = DrawSource::Dma;
    draw.primitive_type = PrimitiveType::TriangleStrip;
    draw.index_count = 4;
    draw.index_buffer = {true, 0, 16, 4, IndexFormat::UInt32, Endian::Swap8In32};
    batch = process_primitives(draw, words);
    assert(batch.valid && batch.indexed);
    assert((batch.indices == std::vector<std::uint32_t>{7, 8, 0xFF000009u & 0x00FFFFFFu, 0xFF000009u & 0x00FFFFFFu, 8, 10}));
  }

  // A long strip cut by many primitive resets: every run converts in place
  // and the output is the runs' triangles in order.
  {
    std::vector<std::uint32_t> dwords;
    std::vector<std::uint32_t> expected;
    std::uint32_t next = 0;
    for (int run = 0; run < 300; ++run) {
      // Four indices, then the reset marker 0xFFFF, packed two per dword.
      const std::uint32_t a = next, b = next + 1, c = next + 2, d = next + 3;
      next += 4;
      dwords.push_back(a | (b << 16u));
      dwords.push_back(c | (d << 16u));
      dwords.push_back(0xFFFFu | (0xFFFFu << 16u));
      expected.insert(expected.end(), {a, b, c, c, b, d});
    }
    draw = {};
    draw.source = DrawSource::Immediate;
    draw.primitive_type = PrimitiveType::TriangleStrip;
    draw.index_format = IndexFormat::UInt16;
    draw.index_count = static_cast<std::uint32_t>(dwords.size() * 2u);
    draw.immediate_index_dwords = dwords;
    batch = process_primitives(draw, memory, {true, 0xFFFFu});
    assert(batch.valid && batch.indexed);
    assert(batch.indices == expected);
  }
  std::cout << "xenon_primitive_processor_tests: ok\n";
}
