#include "xenon/gpu/primitive_processor.hpp"

#include <cstring>

namespace xenon::gpu {
namespace {

std::uint16_t swap16(std::uint16_t value, Endian endian) {
  switch (endian) {
    case Endian::None: return value;
    case Endian::Swap8In16:
    case Endian::Swap8In32: return std::uint16_t((value << 8) | (value >> 8));
    case Endian::Swap16In32: return value;
  }
  return value;
}

void append_triangle(std::vector<std::uint32_t>& out, std::uint32_t a,
                     std::uint32_t b, std::uint32_t c) {
  if (a == b || b == c || a == c) return;
  out.push_back(a); out.push_back(b); out.push_back(c);
}

bool convert_topology(PrimitiveType type, std::span<const std::uint32_t> input,
                      ProcessedPrimitiveBatch& result) {
  auto& output = result.indices;
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

}  // namespace

ProcessedPrimitiveBatch process_primitives(
    const ir::DrawPacket& draw, std::span<const std::byte> physical_memory,
    const PrimitiveProcessingOptions& options) {
  ProcessedPrimitiveBatch result{};
  std::vector<std::uint32_t> source;
  source.reserve(draw.index_count);
  if (draw.source == DrawSource::AutoIndex) {
    for (std::uint32_t i = 0; i < draw.index_count; ++i) source.push_back(i);
  } else if (draw.source == DrawSource::Dma) {
    const auto& buffer = draw.index_buffer;
    const auto bytes_per_index = buffer.format == IndexFormat::UInt32 ? 4u : 2u;
    const auto count = std::min(draw.index_count,
                                buffer.length_bytes / bytes_per_index);
    if (!buffer.valid || std::uint64_t(buffer.physical_address) +
            std::uint64_t(count) * bytes_per_index > physical_memory.size()) {
      result.error = "Xenos DMA index buffer is outside physical memory";
      return result;
    }
    const auto* data = physical_memory.data() + buffer.physical_address;
    for (std::uint32_t i = 0; i < count; ++i) {
      if (buffer.format == IndexFormat::UInt32) {
        std::uint32_t value{};
        std::memcpy(&value, data + i * 4u, 4);
        source.push_back(gpu_swap(value, buffer.endian) & 0x00FFFFFFu);
      } else {
        std::uint16_t value{};
        std::memcpy(&value, data + i * 2u, 2);
        source.push_back(swap16(value, buffer.endian));
      }
    }
  } else if (draw.source == DrawSource::Immediate) {
    for (const auto dword : draw.immediate_index_dwords) {
      if (draw.index_format == IndexFormat::UInt32) source.push_back(dword & 0x00FFFFFFu);
      else {
        source.push_back(dword & 0xFFFFu);
        source.push_back(dword >> 16u);
      }
    }
    if (source.size() > draw.index_count) source.resize(draw.index_count);
  } else {
    result.error = "reserved Xenos draw source";
    return result;
  }
  bool reset_enabled = options.reset_enabled &&
                       draw.source != DrawSource::AutoIndex &&
                       supports_primitive_reset(draw.primitive_type);
  std::uint32_t reset_index = options.reset_index & 0x00FFFFFFu;
  if (reset_enabled && draw.index_format == IndexFormat::UInt16) {
    // Xenos doesn't truncate a >16-bit reset value for a 16-bit index buffer;
    // such a value simply cannot match and primitive reset is effectively off.
    if (reset_index > 0xFFFFu) reset_enabled = false;
  }
  if (reset_enabled) {
    if (!convert_with_primitive_reset(draw.primitive_type, source, reset_index,
                                      result))
      return result;
  } else if (!convert_topology(draw.primitive_type, source, result)) {
    return result;
  }
  result.vertex_count = static_cast<std::uint32_t>(result.indices.size());
  result.indexed = draw.source != DrawSource::AutoIndex ||
                   result.indices.size() != draw.index_count;
  result.valid = true;
  return result;
}

}  // namespace xenon::gpu
