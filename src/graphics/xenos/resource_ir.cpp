#include "xenon/gpu/resource_ir.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace xenon::gpu {
namespace {

std::uint32_t bits(std::uint32_t value, unsigned first, unsigned count) noexcept {
  return (value >> first) & ((std::uint32_t{1} << count) - 1u);
}

std::int32_t signed_bits(std::uint32_t value, unsigned first,
                         unsigned count) noexcept {
  const auto raw = bits(value, first, count);
  const auto sign = std::uint32_t{1} << (count - 1u);
  return static_cast<std::int32_t>((raw ^ sign) - sign);
}

std::uint64_t hash_value(std::uint64_t hash, std::uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    hash ^= static_cast<std::uint8_t>(value >> (i * 8u));
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

std::uint64_t VertexBufferDescriptor::hash() const noexcept {
  auto hash = hash_value(14695981039346656037ull, physical_address);
  hash = hash_value(hash, size_dwords);
  hash = hash_value(hash, static_cast<std::uint64_t>(endian));
  return hash_value(hash, valid);
}

std::uint64_t TextureDescriptor::hash() const noexcept {
  auto hash = hash_value(14695981039346656037ull, base_address);
  hash = hash_value(hash, mip_address);
  hash = hash_value(hash, width);
  hash = hash_value(hash, height);
  hash = hash_value(hash, depth);
  hash = hash_value(hash, pitch);
  hash = hash_value(hash, format);
  hash = hash_value(hash, static_cast<std::uint64_t>(endian));
  hash = hash_value(hash, static_cast<std::uint64_t>(dimension));
  hash = hash_value(hash, mip_min_level);
  hash = hash_value(hash, mip_max_level);
  hash = hash_value(hash, swizzle);
  for (auto value : signs) hash = hash_value(hash, value);
  for (auto value : clamps) hash = hash_value(hash, value);
  hash = hash_value(hash, mag_filter);
  hash = hash_value(hash, min_filter);
  hash = hash_value(hash, mip_filter);
  hash = hash_value(hash, aniso_filter);
  hash = hash_value(hash, border_color);
  hash = hash_value(hash, static_cast<std::uint16_t>(lod_bias));
  hash = hash_value(hash, stacked);
  hash = hash_value(hash, tiled);
  hash = hash_value(hash, packed_mips);
  return hash_value(hash, valid);
}

std::uint64_t DrawResourceState::pipeline_hash(
    const ir::DrawPacket& draw) const noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  hash = hash_value(hash, static_cast<std::uint64_t>(edram_mode));
  hash = hash_value(hash, draw.vertex_shader.hash);
  hash = hash_value(hash, draw.pixel_shader.hash);
  hash = hash_value(hash, static_cast<std::uint64_t>(draw.primitive_type));
  hash = hash_value(hash, static_cast<std::uint64_t>(draw.index_format));
  for (const auto& target : color_targets) {
    hash = hash_value(hash, target.base_tile);
    hash = hash_value(hash, target.format);
    hash = hash_value(hash, static_cast<std::uint8_t>(target.exponent_bias));
    hash = hash_value(hash, target.write_mask);
    hash = hash_value(hash, static_cast<std::uint64_t>(target.blend.color_source));
    hash = hash_value(hash, static_cast<std::uint64_t>(target.blend.color_destination));
    hash = hash_value(hash, static_cast<std::uint64_t>(target.blend.color_operation));
    hash = hash_value(hash, static_cast<std::uint64_t>(target.blend.alpha_source));
    hash = hash_value(hash, static_cast<std::uint64_t>(target.blend.alpha_destination));
    hash = hash_value(hash, static_cast<std::uint64_t>(target.blend.alpha_operation));
    hash = hash_value(hash, target.blend.enabled);
    hash = hash_value(hash, target.enabled);
  }
  for (const auto value : blend_constant)
    hash = hash_value(hash, std::bit_cast<std::uint32_t>(value));
  hash = hash_value(hash, depth_target.base_tile);
  hash = hash_value(hash, depth_target.format);
  hash = hash_value(hash, depth_target.test_enabled);
  hash = hash_value(hash, depth_target.write_enabled);
  hash = hash_value(hash, static_cast<std::uint64_t>(depth_target.function));
  hash = hash_value(hash, depth_target.stencil_enabled);
  hash = hash_value(hash, depth_target.backface_stencil_enabled);
  hash = hash_value(hash, depth_target.stencil_reference);
  hash = hash_value(hash, depth_target.stencil_read_mask);
  hash = hash_value(hash, depth_target.stencil_write_mask);
  hash = hash_value(hash, depth_target.stencil_back_reference);
  hash = hash_value(hash, depth_target.stencil_back_read_mask);
  hash = hash_value(hash, depth_target.stencil_back_write_mask);
  const auto hash_stencil = [&](const StencilFaceState& stencil) {
    hash = hash_value(hash, static_cast<std::uint64_t>(stencil.function));
    hash = hash_value(hash, static_cast<std::uint64_t>(stencil.fail));
    hash = hash_value(hash, static_cast<std::uint64_t>(stencil.depth_pass));
    hash = hash_value(hash, static_cast<std::uint64_t>(stencil.depth_fail));
  };
  hash_stencil(depth_target.stencil_front);
  hash_stencil(depth_target.stencil_back);
  hash = hash_value(hash, raster.surface_pitch);
  hash = hash_value(hash, raster.msaa_samples_log2);
  hash = hash_value(hash, static_cast<std::uint32_t>(raster.scissor_left));
  hash = hash_value(hash, static_cast<std::uint32_t>(raster.scissor_top));
  hash = hash_value(hash, static_cast<std::uint32_t>(raster.scissor_right));
  hash = hash_value(hash, static_cast<std::uint32_t>(raster.scissor_bottom));
  hash = hash_value(hash, raster.cull_front);
  hash = hash_value(hash, raster.cull_back);
  hash = hash_value(hash, raster.front_face_clockwise);
  hash = hash_value(hash, raster.multisample_enabled);
  hash = hash_value(hash, raster.polygon_mode);
  hash = hash_value(hash, raster.front_polygon_type);
  hash = hash_value(hash, raster.back_polygon_type);
  hash = hash_value(hash, std::bit_cast<std::uint32_t>(raster.viewport.x_scale));
  hash = hash_value(hash, std::bit_cast<std::uint32_t>(raster.viewport.x_offset));
  hash = hash_value(hash, std::bit_cast<std::uint32_t>(raster.viewport.y_scale));
  hash = hash_value(hash, std::bit_cast<std::uint32_t>(raster.viewport.y_offset));
  hash = hash_value(hash, std::bit_cast<std::uint32_t>(raster.viewport.z_scale));
  hash = hash_value(hash, std::bit_cast<std::uint32_t>(raster.viewport.z_offset));
  return hash;
}

void ResourceStateTracker::reset() noexcept {
  registers_.fill(0);
  generation_ = 0;
}

void ResourceStateTracker::apply(const ir::RegisterWrite& write) noexcept {
  if (write.index >= registers_.size()) return;
  registers_[write.index] = write.value;
  ++generation_;
}

std::optional<VertexBufferDescriptor> ResourceStateTracker::vertex_buffer(
    std::uint32_t fetch_constant, std::uint32_t select) const noexcept {
  if (fetch_constant >= 32 || select >= 3) return std::nullopt;
  const auto offset = kFetchConstantBase + fetch_constant * 6u + select * 2u;
  const auto w0 = registers_[offset];
  const auto w1 = registers_[offset + 1u];
  if ((w0 & 3u) != 3u) return std::nullopt;
  VertexBufferDescriptor result{};
  result.physical_address = (w0 >> 2) << 2;
  result.endian = static_cast<Endian>(w1 & 3u);
  result.size_dwords = bits(w1, 2, 24);
  result.valid = result.size_dwords != 0;
  return result.valid ? std::optional{result} : std::nullopt;
}

std::optional<TextureDescriptor> ResourceStateTracker::texture(
    std::uint32_t fetch_constant) const noexcept {
  if (fetch_constant >= 32) return std::nullopt;
  const auto offset = kFetchConstantBase + fetch_constant * 6u;
  const auto w0 = registers_[offset + 0u];
  const auto w1 = registers_[offset + 1u];
  const auto w2 = registers_[offset + 2u];
  const auto w3 = registers_[offset + 3u];
  const auto w4 = registers_[offset + 4u];
  const auto w5 = registers_[offset + 5u];
  if ((w0 & 3u) != 2u) return std::nullopt;

  TextureDescriptor result{};
  result.signs = {static_cast<std::uint8_t>(bits(w0, 2, 2)),
                  static_cast<std::uint8_t>(bits(w0, 4, 2)),
                  static_cast<std::uint8_t>(bits(w0, 6, 2)),
                  static_cast<std::uint8_t>(bits(w0, 8, 2))};
  result.clamps = {static_cast<std::uint8_t>(bits(w0, 10, 3)),
                   static_cast<std::uint8_t>(bits(w0, 13, 3)),
                   static_cast<std::uint8_t>(bits(w0, 16, 3))};
  result.pitch = bits(w0, 22, 9) << 5;
  result.tiled = bits(w0, 31, 1) != 0;
  result.format = static_cast<std::uint8_t>(bits(w1, 0, 6));
  result.endian = static_cast<Endian>(bits(w1, 6, 2));
  result.stacked = bits(w1, 10, 1) != 0;
  result.base_address = bits(w1, 12, 20) << 12;
  result.dimension = static_cast<TextureDimension>(bits(w5, 9, 2));
  switch (result.dimension) {
    case TextureDimension::OneD:
      result.width = bits(w2, 0, 24) + 1u;
      break;
    case TextureDimension::TwoDOrStacked:
    case TextureDimension::Cube:
      result.width = bits(w2, 0, 13) + 1u;
      result.height = bits(w2, 13, 13) + 1u;
      result.depth = bits(w2, 26, 6) + 1u;
      break;
    case TextureDimension::ThreeD:
      result.width = bits(w2, 0, 11) + 1u;
      result.height = bits(w2, 11, 11) + 1u;
      result.depth = bits(w2, 22, 10) + 1u;
      break;
  }
  result.swizzle = static_cast<std::uint16_t>(bits(w3, 1, 12));
  result.mag_filter = static_cast<std::uint8_t>(bits(w3, 19, 2));
  result.min_filter = static_cast<std::uint8_t>(bits(w3, 21, 2));
  result.mip_filter = static_cast<std::uint8_t>(bits(w3, 23, 2));
  result.aniso_filter = static_cast<std::uint8_t>(bits(w3, 25, 3));
  result.mip_min_level = static_cast<std::uint8_t>(bits(w4, 2, 4));
  result.mip_max_level = static_cast<std::uint8_t>(
      std::max(bits(w4, 2, 4), bits(w4, 6, 4)));
  result.lod_bias = static_cast<std::int16_t>(signed_bits(w4, 12, 10));
  result.border_color = static_cast<std::uint8_t>(bits(w5, 0, 2));
  result.packed_mips = bits(w5, 11, 1) != 0;
  result.mip_address = bits(w5, 12, 20) << 12;
  result.valid = result.width != 0;
  return result.valid ? std::optional{result} : std::nullopt;
}

DrawResourceState ResourceStateTracker::snapshot() const noexcept {
  DrawResourceState result{};
  result.generation = generation_;
  result.edram_mode = static_cast<EdramMode>(bits(registers_[0x2208], 0, 3));
  const auto surface = registers_[0x2000];
  result.raster.surface_pitch = static_cast<std::uint16_t>(bits(surface, 0, 14));
  result.raster.msaa_samples_log2 = static_cast<std::uint8_t>(bits(surface, 16, 2));
  const auto scissor_tl = registers_[0x200E];
  const auto scissor_br = registers_[0x200F];
  result.raster.scissor_left = signed_bits(scissor_tl, 0, 15);
  result.raster.scissor_top = signed_bits(scissor_tl, 16, 15);
  result.raster.scissor_right = signed_bits(scissor_br, 0, 15);
  result.raster.scissor_bottom = signed_bits(scissor_br, 16, 15);
  const auto mode = registers_[0x2205];
  result.raster.cull_front = bits(mode, 0, 2) == 1 || bits(mode, 0, 2) == 3;
  result.raster.cull_back = bits(mode, 0, 2) == 2 || bits(mode, 0, 2) == 3;
  result.raster.front_face_clockwise = bits(mode, 2, 1) != 0;
  result.raster.polygon_mode = static_cast<std::uint8_t>(bits(mode, 3, 2));
  result.raster.front_polygon_type = static_cast<std::uint8_t>(bits(mode, 5, 3));
  result.raster.back_polygon_type = static_cast<std::uint8_t>(bits(mode, 8, 3));
  result.raster.multisample_enabled = bits(mode, 15, 1) != 0;
  result.raster.viewport.x_scale = std::bit_cast<float>(registers_[0x210F]);
  result.raster.viewport.x_offset = std::bit_cast<float>(registers_[0x2110]);
  result.raster.viewport.y_scale = std::bit_cast<float>(registers_[0x2111]);
  result.raster.viewport.y_offset = std::bit_cast<float>(registers_[0x2112]);
  result.raster.viewport.z_scale = std::bit_cast<float>(registers_[0x2113]);
  result.raster.viewport.z_offset = std::bit_cast<float>(registers_[0x2114]);

  const auto color_mask = registers_[0x2104];
  for (unsigned i = 0; i < result.blend_constant.size(); ++i)
    result.blend_constant[i] = std::bit_cast<float>(registers_[0x2105 + i]);
  constexpr std::array<std::uint32_t, 4> color_info_registers{0x2001, 0x2003,
                                                              0x2004, 0x2005};
  constexpr std::array<std::uint32_t, 4> blend_registers{0x2201, 0x2209,
                                                         0x220A, 0x220B};
  for (unsigned i = 0; i < 4; ++i) {
    const auto info = registers_[color_info_registers[i]];
    auto& target = result.color_targets[i];
    target.base_tile = static_cast<std::uint16_t>(bits(info, 0, 12) & 0x7FFu);
    target.format = static_cast<std::uint8_t>(bits(info, 16, 4));
    target.exponent_bias = static_cast<std::int8_t>(signed_bits(info, 20, 6));
    target.write_mask = static_cast<std::uint8_t>(bits(color_mask, i * 4u, 4));
    const auto blend = registers_[blend_registers[i]];
    target.blend.color_source = static_cast<BlendFactor>(bits(blend, 0, 5));
    target.blend.color_operation = static_cast<BlendOperation>(bits(blend, 5, 3));
    target.blend.color_destination = static_cast<BlendFactor>(bits(blend, 8, 5));
    target.blend.alpha_source = static_cast<BlendFactor>(bits(blend, 16, 5));
    target.blend.alpha_operation = static_cast<BlendOperation>(bits(blend, 21, 3));
    target.blend.alpha_destination = static_cast<BlendFactor>(bits(blend, 24, 5));
    target.blend.enabled =
        target.blend.color_source != BlendFactor::One ||
        target.blend.color_destination != BlendFactor::Zero ||
        target.blend.color_operation != BlendOperation::Add ||
        target.blend.alpha_source != BlendFactor::One ||
        target.blend.alpha_destination != BlendFactor::Zero ||
        target.blend.alpha_operation != BlendOperation::Add;
    target.enabled = target.write_mask != 0;
  }
  const auto depth_info = registers_[0x2002];
  const auto depth_control = registers_[0x2200];
  result.depth_target.base_tile =
      static_cast<std::uint16_t>(bits(depth_info, 0, 12) & 0x7FFu);
  result.depth_target.format = static_cast<std::uint8_t>(bits(depth_info, 16, 1));
  result.depth_target.test_enabled = bits(depth_control, 1, 1) != 0;
  result.depth_target.write_enabled = bits(depth_control, 2, 1) != 0;
  result.depth_target.stencil_enabled = bits(depth_control, 0, 1) != 0;
  result.depth_target.function = static_cast<CompareFunction>(bits(depth_control, 4, 3));
  result.depth_target.backface_stencil_enabled = bits(depth_control, 7, 1) != 0;
  result.depth_target.stencil_front.function =
      static_cast<CompareFunction>(bits(depth_control, 8, 3));
  result.depth_target.stencil_front.fail =
      static_cast<StencilOperation>(bits(depth_control, 11, 3));
  result.depth_target.stencil_front.depth_pass =
      static_cast<StencilOperation>(bits(depth_control, 14, 3));
  result.depth_target.stencil_front.depth_fail =
      static_cast<StencilOperation>(bits(depth_control, 17, 3));
  result.depth_target.stencil_back.function =
      static_cast<CompareFunction>(bits(depth_control, 20, 3));
  result.depth_target.stencil_back.fail =
      static_cast<StencilOperation>(bits(depth_control, 23, 3));
  result.depth_target.stencil_back.depth_pass =
      static_cast<StencilOperation>(bits(depth_control, 26, 3));
  result.depth_target.stencil_back.depth_fail =
      static_cast<StencilOperation>(bits(depth_control, 29, 3));
  const auto stencil_front = registers_[0x210D];
  result.depth_target.stencil_reference = static_cast<std::uint8_t>(stencil_front);
  result.depth_target.stencil_read_mask = static_cast<std::uint8_t>(stencil_front >> 8u);
  result.depth_target.stencil_write_mask = static_cast<std::uint8_t>(stencil_front >> 16u);
  const auto stencil_back = registers_[0x210C];
  result.depth_target.stencil_back_reference = static_cast<std::uint8_t>(stencil_back);
  result.depth_target.stencil_back_read_mask = static_cast<std::uint8_t>(stencil_back >> 8u);
  result.depth_target.stencil_back_write_mask = static_cast<std::uint8_t>(stencil_back >> 16u);
  for (unsigned i = 0; i < 32; ++i) {
    result.textures[i] = texture(i);
    for (unsigned j = 0; j < 3; ++j) result.vertex_buffers[i][j] = vertex_buffer(i, j);
  }
  return result;
}

bool ResourceStateTracker::write_constant_buffer(
    std::span<std::byte> destination) const noexcept {
  constexpr std::size_t kSize = 9472;
  if (destination.size() < kSize) return false;
  std::fill_n(destination.begin(), kSize, std::byte{});
  // float4[512]
  std::memcpy(destination.data(), registers_.data() + 0x4000, 8192);
  // uint4[8], with the eight architectural boolean words packed into the
  // first two vectors exactly as xenon_bool indexes them.
  std::memcpy(destination.data() + 8192, registers_.data() + 0x4900, 32);
  // uint4[32] loop constants. Each architectural scalar occupies .x.
  for (std::size_t i = 0; i < 32; ++i) {
    std::memcpy(destination.data() + 8320 + i * 16,
                registers_.data() + 0x4908 + i, sizeof(std::uint32_t));
  }
  // GPU 07 currently consumes the base dword of each six-dword vertex fetch
  // descriptor. The full descriptors remain available via DrawResourceState.
  for (std::size_t i = 0; i < 32; ++i) {
    std::memcpy(destination.data() + 8832 + i * 16,
                registers_.data() + kFetchConstantBase + i * 6,
                sizeof(std::uint32_t));
  }
  return true;
}

}  // namespace xenon::gpu
