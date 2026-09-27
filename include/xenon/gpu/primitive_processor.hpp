#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/gpu/ir.hpp"

namespace xenon::gpu {

enum class HostPrimitiveTopology : std::uint8_t {
  PointList,
  LineList,
  TriangleList,
};

struct PrimitiveProcessingOptions {
  bool reset_enabled{};
  std::uint32_t reset_index{};
};

struct ProcessedPrimitiveBatch {
  HostPrimitiveTopology topology{HostPrimitiveTopology::TriangleList};
  std::vector<std::uint32_t> indices{};
  std::uint32_t vertex_count{};
  std::string error{};
  bool indexed{};
  // Xenos rectangle lists provide three corners. A native geometry shader
  // selects the diagonal and generates the fourth post-transform corner.
  bool requires_rectangle_expansion{};
  bool valid{};
};

// Converts Xenos draw sources and primitive types once in the common layer.
// Backends receive only native point/line/triangle lists and never reinterpret
// PM4 immediate packing or guest endian modes independently.
[[nodiscard]] ProcessedPrimitiveBatch process_primitives(
    const ir::DrawPacket& draw, std::span<const std::byte> physical_memory,
    const PrimitiveProcessingOptions& options = {},
    std::uint32_t physical_base = 0u);

}  // namespace xenon::gpu
