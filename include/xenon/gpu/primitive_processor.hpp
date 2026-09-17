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

struct ProcessedPrimitiveBatch {
  HostPrimitiveTopology topology{HostPrimitiveTopology::TriangleList};
  std::vector<std::uint32_t> indices{};
  std::uint32_t vertex_count{};
  std::string error{};
  bool indexed{};
  bool valid{};
};

// Converts Xenos draw sources and primitive types once in the common layer.
// Backends receive only native point/line/triangle lists and never reinterpret
// PM4 immediate packing or guest endian modes independently.
[[nodiscard]] ProcessedPrimitiveBatch process_primitives(
    const ir::DrawPacket& draw, std::span<const std::byte> physical_memory);

}  // namespace xenon::gpu
