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
  // Nonzero for a batch the processed-index cache keeps: the backend may then
  // hold its indices in a GPU-resident buffer under this key until the cache
  // drops it (ProcessedIndexCache::set_erase_listener).
  std::uint64_t resident_key{};
};

// Converts Xenos draw sources and primitive types once in the common layer.
// Backends receive only native point/line/triangle lists and never reinterpret
// PM4 immediate packing or guest endian modes independently.
[[nodiscard]] ProcessedPrimitiveBatch process_primitives(
    const ir::DrawPacket& draw, std::span<const std::byte> physical_memory,
    const PrimitiveProcessingOptions& options = {},
    std::uint32_t physical_base = 0u);

// The same conversion into `result`, reusing its index storage and `scratch`
// (the intermediate the strip, fan, quad and reset expansions need) across
// calls, so a steady stream of draws allocates nothing. List topologies are
// converted in one pass; an auto-indexed list is a non-indexed draw and
// produces no indices at all. False (with result.error) when the draw cannot
// be converted.
bool process_primitives_into(const ir::DrawPacket& draw,
                             std::span<const std::byte> physical_memory,
                             const PrimitiveProcessingOptions& options,
                             std::uint32_t physical_base, ProcessedPrimitiveBatch& result,
                             std::vector<std::uint32_t>& scratch);

}  // namespace xenon::gpu
