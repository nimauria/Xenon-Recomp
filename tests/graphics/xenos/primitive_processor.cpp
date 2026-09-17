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

  draw.primitive_type = PrimitiveType::RectangleList;
  batch = process_primitives(draw, memory);
  assert(!batch.valid && !batch.error.empty());
  std::cout << "xenon_primitive_processor_tests: ok\n";
}
