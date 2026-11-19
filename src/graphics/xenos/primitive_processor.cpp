#include "xenon/gpu/primitive_processor.hpp"

#include <algorithm>
#include <cstring>

namespace xenon::gpu {
namespace {

void append_triangle(std::vector<std::uint32_t>& out, std::uint32_t a,
                     std::uint32_t b, std::uint32_t c) {
  if (a == b || b == c || a == c) return;
  out.push_back(a); out.push_back(b); out.push_back(c);
}

// Room for `more` indices on top of what `out` holds, growing geometrically:
// a primitive-reset batch converts many short runs into the same vector.
void reserve_more(std::vector<std::uint32_t>& out, std::size_t more) {
  const auto need = out.size() + more;
  if (out.capacity() < need) out.reserve((std::max)(need, out.capacity() * 2u));
}

bool convert_topology(PrimitiveType type, std::span<const std::uint32_t> input,
                      ProcessedPrimitiveBatch& result) {
  auto& output = result.indices;
  // The most each expansion can emit (degenerate triangles are dropped).
  const auto runs = input.size() > 2u ? input.size() - 2u : 0u;
  switch (type) {
    case PrimitiveType::LineStrip:
    case PrimitiveType::TwoDLineStrip:
    case PrimitiveType::LineLoop: reserve_more(output, input.size() * 2u + 2u); break;
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TwoDTriStrip:
    case PrimitiveType::TriangleFan:
    case PrimitiveType::Polygon: reserve_more(output, runs * 3u); break;
    case PrimitiveType::QuadList:
    case PrimitiveType::QuadStrip: reserve_more(output, input.size() / 2u * 6u + 6u); break;
    default: break;
  }
  switch (type) {
    case PrimitiveType::PointList:
      result.topology = HostPrimitiveTopology::PointList;
      output.assign(input.begin(), input.end());
      return true;
    case PrimitiveType::LineList:
      result.topology = HostPrimitiveTopology::LineList;
      output.assign(input.begin(), input.begin() + (input.size() & ~std::size_t{1}));
      return true;
    case PrimitiveType::LineStrip:
    case PrimitiveType::TwoDLineStrip:
      result.topology = HostPrimitiveTopology::LineList;
      for (std::size_t i = 1; i < input.size(); ++i) {
        output.push_back(input[i - 1]); output.push_back(input[i]);
      }
      return true;
    case PrimitiveType::LineLoop:
      result.topology = HostPrimitiveTopology::LineList;
      for (std::size_t i = 1; i < input.size(); ++i) {
        output.push_back(input[i - 1]); output.push_back(input[i]);
      }
      if (input.size() > 1) { output.push_back(input.back()); output.push_back(input.front()); }
      return true;
    case PrimitiveType::TriangleList:
      result.topology = HostPrimitiveTopology::TriangleList;
      output.assign(input.begin(), input.begin() + input.size() / 3u * 3u);
      return true;
    case PrimitiveType::RectangleList:
    case PrimitiveType::CopyRectList0:
    case PrimitiveType::CopyRectList1:
    case PrimitiveType::CopyRectList2:
    case PrimitiveType::CopyRectList3:
    case PrimitiveType::FillRectList:
      result.topology = HostPrimitiveTopology::TriangleList;
      result.requires_rectangle_expansion = true;
      output.assign(input.begin(), input.begin() + input.size() / 3u * 3u);
      return true;
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TwoDTriStrip:
      result.topology = HostPrimitiveTopology::TriangleList;
      for (std::size_t i = 2; i < input.size(); ++i) {
        if (i & 1u) append_triangle(output, input[i - 1], input[i - 2], input[i]);
        else append_triangle(output, input[i - 2], input[i - 1], input[i]);
      }
      return true;
    case PrimitiveType::TriangleFan:
    case PrimitiveType::Polygon:
      result.topology = HostPrimitiveTopology::TriangleList;
      for (std::size_t i = 2; i < input.size(); ++i)
        append_triangle(output, input[0], input[i - 1], input[i]);
      return true;
    case PrimitiveType::QuadList:
      result.topology = HostPrimitiveTopology::TriangleList;
      for (std::size_t i = 0; i + 3 < input.size(); i += 4) {
        append_triangle(output, input[i], input[i + 1], input[i + 2]);
        append_triangle(output, input[i], input[i + 2], input[i + 3]);
      }
      return true;
    case PrimitiveType::QuadStrip:
      result.topology = HostPrimitiveTopology::TriangleList;
      for (std::size_t i = 0; i + 3 < input.size(); i += 2) {
        append_triangle(output, input[i], input[i + 1], input[i + 2]);
        append_triangle(output, input[i + 2], input[i + 1], input[i + 3]);
      }
      return true;
    default:
      result.error = "Xenos primitive requires a dedicated expansion path";
      return false;
  }
}

bool supports_primitive_reset(PrimitiveType type) noexcept {
  switch (type) {
    case PrimitiveType::LineStrip:
    case PrimitiveType::TriangleFan:
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::LineLoop:
    case PrimitiveType::QuadStrip:
    case PrimitiveType::Polygon:
    case PrimitiveType::TwoDLineStrip:
    case PrimitiveType::TwoDTriStrip:
      return true;
    default:
      return false;
  }
}

bool convert_with_primitive_reset(
    PrimitiveType type, std::span<const std::uint32_t> input,
    std::uint32_t reset_index, ProcessedPrimitiveBatch& result) {
  std::size_t begin = 0;
  bool emitted_range = false;
  for (std::size_t i = 0; i <= input.size(); ++i) {
    if (i != input.size() && input[i] != reset_index) continue;
    if (i > begin) {
      if (!convert_topology(type, input.subspan(begin, i - begin), result))
        return false;
      emitted_range = true;
    }
    begin = i + 1;
  }
  // Preserve the output topology even when the guest index buffer contains
  // only restart markers (or is otherwise empty).
  if (!emitted_range)
    return convert_topology(type, std::span<const std::uint32_t>{}, result);
  return true;
}

// The topologies the host draws as they are: each index is used once, in
// order, so the converted list is the source trimmed to whole primitives.
bool direct_list_topology(PrimitiveType type, std::size_t count, HostPrimitiveTopology& topology,
                          bool& rectangle_expansion, std::size_t& kept) noexcept {
  rectangle_expansion = false;
  switch (type) {
    case PrimitiveType::PointList:
      topology = HostPrimitiveTopology::PointList;
      kept = count;
      return true;
    case PrimitiveType::LineList:
      topology = HostPrimitiveTopology::LineList;
      kept = count & ~std::size_t{1};
      return true;
    case PrimitiveType::TriangleList:
      topology = HostPrimitiveTopology::TriangleList;
      kept = count / 3u * 3u;
      return true;
    case PrimitiveType::RectangleList:
    case PrimitiveType::CopyRectList0:
    case PrimitiveType::CopyRectList1:
    case PrimitiveType::CopyRectList2:
    case PrimitiveType::CopyRectList3:
    case PrimitiveType::FillRectList:
      topology = HostPrimitiveTopology::TriangleList;
      rectangle_expansion = true;
      kept = count / 3u * 3u;
      return true;
    default:
      return false;
  }
}

// Reads `count` guest indices from `data` into `out`, endian-swapped and (for
// 32-bit indices) masked to the 24 bits Xenos uses.
void read_guest_indices(const std::byte* data, std::size_t count, IndexFormat format,
                        Endian endian, std::uint32_t* out) noexcept {
  if (format == IndexFormat::UInt32) {
    for (std::size_t i = 0; i < count; ++i) {
      std::uint32_t value{};
      std::memcpy(&value, data + i * 4u, 4);
      out[i] = gpu_swap(value, endian) & 0x00FFFFFFu;
    }
  } else if (endian == Endian::Swap8In16 || endian == Endian::Swap8In32) {
    for (std::size_t i = 0; i < count; ++i) {
      std::uint16_t value{};
      std::memcpy(&value, data + i * 2u, 2);
      out[i] = std::uint16_t((value << 8) | (value >> 8));
    }
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      std::uint16_t value{};
      std::memcpy(&value, data + i * 2u, 2);
      out[i] = value;
    }
  }
}

}  // namespace

bool process_primitives_into(const ir::DrawPacket& draw,
                             std::span<const std::byte> physical_memory,
                             const PrimitiveProcessingOptions& options,
                             std::uint32_t physical_base, ProcessedPrimitiveBatch& result,
                             std::vector<std::uint32_t>& scratch) {
  result.topology = HostPrimitiveTopology::TriangleList;
  result.indices.clear();
  result.vertex_count = 0;
  result.error.clear();
  result.indexed = false;
  result.requires_rectangle_expansion = false;
  result.valid = false;
  scratch.clear();

  HostPrimitiveTopology list_topology{};
  bool rectangle_expansion = false;
  std::size_t kept = 0;
  if (draw.source == DrawSource::AutoIndex) {
    if (direct_list_topology(draw.primitive_type, draw.index_count, list_topology,
                             rectangle_expansion, kept)) {
      // 0, 1, 2... in order: a non-indexed draw of the whole primitives.
      result.topology = list_topology;
      result.requires_rectangle_expansion = rectangle_expansion;
      result.vertex_count = static_cast<std::uint32_t>(kept);
      result.valid = true;
      return true;
    }
    scratch.resize(draw.index_count);
    for (std::uint32_t i = 0; i < draw.index_count; ++i) scratch[i] = i;
  } else if (draw.source == DrawSource::Dma) {
    const auto& buffer = draw.index_buffer;
    const auto bytes_per_index = buffer.format == IndexFormat::UInt32 ? 4u : 2u;
    const auto count = std::min(draw.index_count, buffer.length_bytes / bytes_per_index);
    if (!buffer.valid || buffer.physical_address < physical_base ||
        std::uint64_t(buffer.physical_address) + std::uint64_t(count) * bytes_per_index >
            std::uint64_t{physical_base} + physical_memory.size()) {
      result.error = "Xenos DMA index buffer is outside physical memory";
      return false;
    }
    const auto* data = physical_memory.data() + (buffer.physical_address - physical_base);
    // List topologies take no primitive reset: one pass straight into the
    // result, with no intermediate copy.
    if (direct_list_topology(draw.primitive_type, count, list_topology, rectangle_expansion,
                             kept)) {
      result.topology = list_topology;
      result.requires_rectangle_expansion = rectangle_expansion;
      result.indices.resize(kept);
      read_guest_indices(data, kept, buffer.format, buffer.endian, result.indices.data());
      result.vertex_count = static_cast<std::uint32_t>(kept);
      result.indexed = true;
      result.valid = true;
      return true;
    }
    scratch.resize(count);
    read_guest_indices(data, count, buffer.format, buffer.endian, scratch.data());
  } else if (draw.source == DrawSource::Immediate) {
    scratch.reserve(draw.immediate_index_dwords.size() * 2u);
    for (const auto dword : draw.immediate_index_dwords) {
      if (draw.index_format == IndexFormat::UInt32) scratch.push_back(dword & 0x00FFFFFFu);
      else {
        scratch.push_back(dword & 0xFFFFu);
        scratch.push_back(dword >> 16u);
      }
    }
    if (scratch.size() > draw.index_count) scratch.resize(draw.index_count);
  } else {
    result.error = "reserved Xenos draw source";
    return false;
  }
  bool reset_enabled = options.reset_enabled && draw.source != DrawSource::AutoIndex &&
                       supports_primitive_reset(draw.primitive_type);
  std::uint32_t reset_index = options.reset_index & 0x00FFFFFFu;
  if (reset_enabled && draw.index_format == IndexFormat::UInt16) {
    // Xenos doesn't truncate a >16-bit reset value for a 16-bit index buffer;
    // such a value simply cannot match and primitive reset is effectively off.
    if (reset_index > 0xFFFFu) reset_enabled = false;
  }
  if (reset_enabled) {
    if (!convert_with_primitive_reset(draw.primitive_type, scratch, reset_index, result))
      return false;
  } else if (!convert_topology(draw.primitive_type, scratch, result)) {
    return false;
  }
  result.vertex_count = static_cast<std::uint32_t>(result.indices.size());
  result.indexed = draw.source != DrawSource::AutoIndex ||
                   result.indices.size() != draw.index_count;
  result.valid = true;
  return true;
}

ProcessedPrimitiveBatch process_primitives(
    const ir::DrawPacket& draw, std::span<const std::byte> physical_memory,
    const PrimitiveProcessingOptions& options, std::uint32_t physical_base) {
  ProcessedPrimitiveBatch result{};
  std::vector<std::uint32_t> scratch;
  (void)process_primitives_into(draw, physical_memory, options, physical_base, result, scratch);
  return result;
}

}  // namespace xenon::gpu
