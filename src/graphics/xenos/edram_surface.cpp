#include "xenon/gpu/edram_surface.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>
#include <vector>

namespace xenon::gpu {
namespace {

std::uint32_t read_u32(const Edram& edram, std::uint32_t address) noexcept {
  std::array<std::byte, 4> bytes{};
  edram.read(address, bytes);
  std::uint32_t value{};
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

void write_u32(Edram& edram, std::uint32_t address,
               std::uint32_t value) noexcept {
  std::array<std::byte, 4> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  edram.write(address, bytes);
}

float half_to_float(std::uint16_t value) noexcept {
  const auto sign = std::uint32_t(value & 0x8000u) << 16u;
  auto exponent = std::int32_t((value >> 10u) & 0x1Fu);
  auto mantissa = std::uint32_t(value & 0x03FFu);
  if (!exponent) {
    if (!mantissa) return std::bit_cast<float>(sign);
    const auto shift = std::countl_zero(mantissa) - 21u;
    exponent = 1 - static_cast<std::int32_t>(shift);
    mantissa = (mantissa << shift) & 0x03FFu;
  } else if (exponent == 31) {
    return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13u));
  }
  return std::bit_cast<float>(sign | (std::uint32_t(exponent + 112) << 23u) |
                              (mantissa << 13u));
}

std::uint16_t float_to_half(float value) noexcept {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  const auto sign = std::uint16_t((bits >> 16u) & 0x8000u);
  const auto magnitude = bits & 0x7FFFFFFFu;
  if (magnitude >= 0x7F800000u) {
    return std::uint16_t(sign | (magnitude == 0x7F800000u ? 0x7C00u : 0x7E00u));
  }
  if (magnitude >= 0x477FF000u) return std::uint16_t(sign | 0x7C00u);
  if (magnitude < 0x33000000u) return sign;
  std::uint32_t rounded{};
  if (magnitude < 0x38800000u) {
    const auto mantissa = (magnitude & 0x7FFFFFu) | 0x800000u;
    const auto shift = 113u - (magnitude >> 23u);
    rounded = mantissa >> shift;
    const auto remainder = mantissa & ((1u << shift) - 1u);
    const auto halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (rounded & 1u))) ++rounded;
  } else {
    rounded = (magnitude - 0x38000000u) >> 13u;
    const auto remainder = magnitude & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (rounded & 1u))) ++rounded;
  }
  return std::uint16_t(sign | rounded);
}

float fixed16_to_float(std::uint16_t value) noexcept {
  return float(static_cast<std::int16_t>(value)) * (1.0f / 1024.0f);
}

std::uint16_t float_to_fixed16(float value) noexcept {
  value = std::clamp(value, -32.0f, 32767.0f / 1024.0f);
  return static_cast<std::uint16_t>(static_cast<std::int16_t>(
      std::nearbyint(value * 1024.0f)));
}

}  // namespace

std::uint32_t EdramSurfaceLayout::sample_width() const noexcept {
  return pitch_pixels << (msaa == MsaaSamples::X4 ? 1u : 0u);
}

std::uint32_t EdramSurfaceLayout::sample_height() const noexcept {
  return height_pixels << (msaa == MsaaSamples::X1 ? 0u : 1u);
}

std::uint32_t EdramSurfaceLayout::pitch_tiles() const noexcept {
  const auto tiles = (sample_width() + 79u) / 80u;
  return is_64bpp ? tiles * 2u : tiles;
}

std::uint32_t EdramSurfaceLayout::row_tiles() const noexcept {
  return pitch_tiles();
}

std::uint32_t EdramSurfaceLayout::tile_rows() const noexcept {
  return (sample_height() + 15u) / 16u;
}

std::uint32_t EdramSurfaceLayout::tile_count() const noexcept {
  return pitch_tiles() * tile_rows();
}

bool EdramSurfaceLayout::valid() const noexcept {
  return pitch_pixels && height_pixels &&
         static_cast<unsigned>(msaa) <= static_cast<unsigned>(MsaaSamples::X4) &&
         pitch_tiles() && tile_count();
}

namespace {
std::uint64_t hash_surface(const EdramSurfaceLayout& surface,
                           bool include_height) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  auto add = [&](std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
      hash ^= static_cast<std::uint8_t>(value >> (i * 8u));
      hash *= 1099511628211ull;
    }
  };
  add(surface.base_tile);
  add(surface.pitch_pixels);
  if (include_height) add(surface.height_pixels);
  add(static_cast<std::uint8_t>(surface.msaa));
  add(surface.is_64bpp);
  add(surface.depth);
  return hash;
}
}  // namespace

std::uint64_t EdramSurfaceLayout::identity_hash() const noexcept {
  return hash_surface(*this, false);
}

std::uint64_t EdramSurfaceLayout::hash() const noexcept {
  return hash_surface(*this, true);
}

bool color_render_target_is_64bpp(ColorRenderTargetFormat format) noexcept {
  format = storage_color_format(format);
  return format == ColorRenderTargetFormat::R16G16B16A16Fixed ||
         format == ColorRenderTargetFormat::R16G16B16A16Float ||
         format == ColorRenderTargetFormat::R32G32Float;
}

ColorRenderTargetFormat storage_color_format(
    ColorRenderTargetFormat format) noexcept {
  switch (format) {
    case ColorRenderTargetFormat::R10G10B10A2As10:
      return ColorRenderTargetFormat::R10G10B10A2;
    case ColorRenderTargetFormat::R10G10B10A2FloatAs16:
      return ColorRenderTargetFormat::R10G10B10A2Float;
    default:
      return format;
  }
}

float xenos_pwl_gamma_to_linear(float gamma) noexcept {
  gamma = std::isnan(gamma) ? 0.0f : std::clamp(gamma, 0.0f, 1.0f);
  float scale{}, offset{};
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) { scale = 8.0f / 1024.0f; offset = -1024.0f; }
    else { scale = 4.0f / 1024.0f; offset = -256.0f; }
  } else if (gamma >= 64.0f / 255.0f) {
    scale = 2.0f / 1024.0f; offset = -64.0f;
  } else {
    scale = 1.0f / 1024.0f; offset = 0.0f;
  }
  auto linear = gamma * ((255.0f * 1024.0f) * scale) + offset;
  linear += std::trunc(linear * scale);
  return linear * (1.0f / 1023.0f);
}

float xenos_linear_to_pwl_gamma(float linear) noexcept {
  linear = std::isnan(linear) ? 0.0f : std::clamp(linear, 0.0f, 1.0f);
  float scale{}, offset{};
  if (linear >= 128.0f / 1023.0f) {
    if (linear >= 512.0f / 1023.0f) { scale = 1023.0f / 8.0f; offset = 128.0f / 255.0f; }
    else { scale = 1023.0f / 4.0f; offset = 64.0f / 255.0f; }
  } else if (linear >= 64.0f / 1023.0f) {
    scale = 1023.0f / 2.0f; offset = 32.0f / 255.0f;
  } else {
    scale = 1023.0f; offset = 0.0f;
  }
  return std::trunc(linear * scale) * (1.0f / 255.0f) + offset;
}

float float7e3_to_float32(std::uint32_t value) noexcept {
  value &= 0x3FFu;
  if (!value) return 0.0f;
  auto mantissa = value & 0x7Fu;
  std::int32_t exponent = static_cast<std::int32_t>(value >> 7u);
  if (!exponent) {
    const auto shift = std::countl_zero(mantissa) - 24u;
    exponent = 1 - static_cast<std::int32_t>(shift);
    mantissa = (mantissa << shift) & 0x7Fu;
  }
  return std::bit_cast<float>(std::uint32_t(exponent + 124) << 23u |
                              mantissa << 16u);
}

std::uint32_t float32_to_7e3(float value) noexcept {
  if (!(value > 0.0f)) return 0;
  if (value >= float7e3_to_float32(0x3FFu)) return 0x3FFu;
  std::uint32_t best{};
  float best_error = value;
  for (std::uint32_t code = 1; code < 0x400u; ++code) {
    const auto error = std::abs(float7e3_to_float32(code) - value);
    if (error < best_error || (error == best_error && !(code & 1u))) {
      best = code; best_error = error;
    }
  }
  return best;
}

ColorSample unpack_color_sample(ColorRenderTargetFormat format,
                                std::array<std::uint32_t, 2> packed) noexcept {
  format = storage_color_format(format);
  ColorSample result{{0.0f, 0.0f, 0.0f, 1.0f}};
  auto& c = result.components;
  switch (format) {
    case ColorRenderTargetFormat::R8G8B8A8:
    case ColorRenderTargetFormat::R8G8B8A8Gamma:
      for (unsigned i = 0; i < 4; ++i) c[i] = float((packed[0] >> (i * 8u)) & 0xFFu) / 255.0f;
      if (format == ColorRenderTargetFormat::R8G8B8A8Gamma)
        for (unsigned i = 0; i < 3; ++i) c[i] = xenos_pwl_gamma_to_linear(c[i]);
      break;
    case ColorRenderTargetFormat::R10G10B10A2:
      for (unsigned i = 0; i < 3; ++i) c[i] = float((packed[0] >> (i * 10u)) & 0x3FFu) / 1023.0f;
      c[3] = float(packed[0] >> 30u) / 3.0f;
      break;
    case ColorRenderTargetFormat::R10G10B10A2Float:
      for (unsigned i = 0; i < 3; ++i) c[i] = float7e3_to_float32(packed[0] >> (i * 10u));
      c[3] = float(packed[0] >> 30u) / 3.0f;
      break;
    case ColorRenderTargetFormat::R16G16Fixed:
    case ColorRenderTargetFormat::R16G16B16A16Fixed:
      c[0] = fixed16_to_float(std::uint16_t(packed[0])); c[1] = fixed16_to_float(std::uint16_t(packed[0] >> 16u));
      if (format == ColorRenderTargetFormat::R16G16B16A16Fixed) { c[2] = fixed16_to_float(std::uint16_t(packed[1])); c[3] = fixed16_to_float(std::uint16_t(packed[1] >> 16u)); }
      break;
    case ColorRenderTargetFormat::R16G16Float:
    case ColorRenderTargetFormat::R16G16B16A16Float:
      c[0] = half_to_float(std::uint16_t(packed[0])); c[1] = half_to_float(std::uint16_t(packed[0] >> 16u));
      if (format == ColorRenderTargetFormat::R16G16B16A16Float) { c[2] = half_to_float(std::uint16_t(packed[1])); c[3] = half_to_float(std::uint16_t(packed[1] >> 16u)); }
      break;
    case ColorRenderTargetFormat::R32Float: c[0] = std::bit_cast<float>(packed[0]); break;
    case ColorRenderTargetFormat::R32G32Float: c[0] = std::bit_cast<float>(packed[0]); c[1] = std::bit_cast<float>(packed[1]); break;
    default: break;
  }
  return result;
}

std::array<std::uint32_t, 2> pack_color_sample(
    ColorRenderTargetFormat format, const ColorSample& sample) noexcept {
  format = storage_color_format(format);
  auto c = sample.components;
  std::array<std::uint32_t, 2> result{};
  switch (format) {
    case ColorRenderTargetFormat::R8G8B8A8:
    case ColorRenderTargetFormat::R8G8B8A8Gamma:
      if (format == ColorRenderTargetFormat::R8G8B8A8Gamma)
        for (unsigned i = 0; i < 3; ++i) c[i] = xenos_linear_to_pwl_gamma(c[i]);
      for (unsigned i = 0; i < 4; ++i) result[0] |= std::uint32_t(std::nearbyint(std::clamp(c[i], 0.0f, 1.0f) * 255.0f)) << (i * 8u);
      break;
    case ColorRenderTargetFormat::R10G10B10A2:
      for (unsigned i = 0; i < 3; ++i) result[0] |= std::uint32_t(std::nearbyint(std::clamp(c[i], 0.0f, 1.0f) * 1023.0f)) << (i * 10u);
      result[0] |= std::uint32_t(std::nearbyint(std::clamp(c[3], 0.0f, 1.0f) * 3.0f)) << 30u;
      break;
    case ColorRenderTargetFormat::R10G10B10A2Float:
      for (unsigned i = 0; i < 3; ++i) result[0] |= float32_to_7e3(c[i]) << (i * 10u);
      result[0] |= std::uint32_t(std::nearbyint(std::clamp(c[3], 0.0f, 1.0f) * 3.0f)) << 30u;
      break;
    case ColorRenderTargetFormat::R16G16Fixed:
    case ColorRenderTargetFormat::R16G16B16A16Fixed:
      result[0] = std::uint32_t(float_to_fixed16(c[0])) | std::uint32_t(float_to_fixed16(c[1])) << 16u;
      if (format == ColorRenderTargetFormat::R16G16B16A16Fixed) result[1] = std::uint32_t(float_to_fixed16(c[2])) | std::uint32_t(float_to_fixed16(c[3])) << 16u;
      break;
    case ColorRenderTargetFormat::R16G16Float:
    case ColorRenderTargetFormat::R16G16B16A16Float:
      result[0] = std::uint32_t(float_to_half(c[0])) | std::uint32_t(float_to_half(c[1])) << 16u;
      if (format == ColorRenderTargetFormat::R16G16B16A16Float) result[1] = std::uint32_t(float_to_half(c[2])) | std::uint32_t(float_to_half(c[3])) << 16u;
      break;
    case ColorRenderTargetFormat::R32Float: result[0] = std::bit_cast<std::uint32_t>(c[0]); break;
    case ColorRenderTargetFormat::R32G32Float: result[0] = std::bit_cast<std::uint32_t>(c[0]); result[1] = std::bit_cast<std::uint32_t>(c[1]); break;
    default: break;
  }
  return result;
}

ColorHostStorage color_host_storage(ColorRenderTargetFormat format) noexcept {
  switch (storage_color_format(format)) {
    case ColorRenderTargetFormat::R8G8B8A8:
      return ColorHostStorage::R8G8B8A8Unorm;
    case ColorRenderTargetFormat::R8G8B8A8Gamma:
      return ColorHostStorage::R16G16B16A16Unorm;
    case ColorRenderTargetFormat::R10G10B10A2:
      return ColorHostStorage::R10G10B10A2Unorm;
    case ColorRenderTargetFormat::R16G16Fixed:
    case ColorRenderTargetFormat::R16G16Float:
      return ColorHostStorage::R16G16Float;
    case ColorRenderTargetFormat::R10G10B10A2Float:
    case ColorRenderTargetFormat::R16G16B16A16Fixed:
    case ColorRenderTargetFormat::R16G16B16A16Float:
      return ColorHostStorage::R16G16B16A16Float;
    case ColorRenderTargetFormat::R32Float:
      return ColorHostStorage::R32Float;
    case ColorRenderTargetFormat::R32G32Float:
      return ColorHostStorage::R32G32Float;
    default:
      return ColorHostStorage::Unsupported;
  }
}

std::uint32_t color_host_bytes_per_pixel(
    ColorRenderTargetFormat format) noexcept {
  switch (color_host_storage(format)) {
    case ColorHostStorage::R8G8B8A8Unorm:
    case ColorHostStorage::R10G10B10A2Unorm:
    case ColorHostStorage::R16G16Float:
    case ColorHostStorage::R32Float:
      return 4;
    case ColorHostStorage::R16G16B16A16Unorm:
    case ColorHostStorage::R16G16B16A16Float:
    case ColorHostStorage::R32G32Float:
      return 8;
    default:
      return 0;
  }
}

bool color_host_requires_conversion(ColorRenderTargetFormat format) noexcept {
  switch (storage_color_format(format)) {
    case ColorRenderTargetFormat::R8G8B8A8Gamma:
    case ColorRenderTargetFormat::R10G10B10A2Float:
    case ColorRenderTargetFormat::R16G16Fixed:
    case ColorRenderTargetFormat::R16G16B16A16Fixed:
      return true;
    default:
      return false;
  }
}

bool encode_host_color_sample(ColorRenderTargetFormat format,
                              const ColorSample& sample,
                              std::span<std::byte> destination) noexcept {
  const auto size = color_host_bytes_per_pixel(format);
  if (!size || destination.size() < size) return false;
  const auto& c = sample.components;
  std::array<std::uint16_t, 4> words{};
  switch (color_host_storage(format)) {
    case ColorHostStorage::R8G8B8A8Unorm:
    case ColorHostStorage::R10G10B10A2Unorm:
    case ColorHostStorage::R32Float:
    case ColorHostStorage::R32G32Float: {
      const auto packed = pack_color_sample(storage_color_format(format), sample);
      std::memcpy(destination.data(), packed.data(), size);
      return true;
    }
    case ColorHostStorage::R16G16B16A16Unorm:
      for (unsigned i = 0; i < 4; ++i)
        words[i] = static_cast<std::uint16_t>(std::nearbyint(
            std::clamp(c[i], 0.0f, 1.0f) * 65535.0f));
      break;
    case ColorHostStorage::R16G16Float:
      words[0] = float_to_half(c[0]); words[1] = float_to_half(c[1]);
      break;
    case ColorHostStorage::R16G16B16A16Float:
      for (unsigned i = 0; i < 4; ++i) words[i] = float_to_half(c[i]);
      break;
    default:
      return false;
  }
  std::memcpy(destination.data(), words.data(), size);
  return true;
}

bool decode_host_color_sample(ColorRenderTargetFormat format,
                              std::span<const std::byte> source,
                              ColorSample& sample) noexcept {
  const auto size = color_host_bytes_per_pixel(format);
  if (!size || source.size() < size) return false;
  std::array<std::uint32_t, 2> packed{};
  switch (color_host_storage(format)) {
    case ColorHostStorage::R8G8B8A8Unorm:
    case ColorHostStorage::R10G10B10A2Unorm:
    case ColorHostStorage::R32Float:
    case ColorHostStorage::R32G32Float:
      std::memcpy(packed.data(), source.data(), size);
      sample = unpack_color_sample(storage_color_format(format), packed);
      return true;
    case ColorHostStorage::R16G16B16A16Unorm: {
      std::array<std::uint16_t, 4> words{};
      std::memcpy(words.data(), source.data(), size);
      for (unsigned i = 0; i < 4; ++i)
        sample.components[i] = float(words[i]) / 65535.0f;
      return true;
    }
    case ColorHostStorage::R16G16Float:
    case ColorHostStorage::R16G16B16A16Float: {
      std::array<std::uint16_t, 4> words{};
      std::memcpy(words.data(), source.data(), size);
      const auto count = color_host_storage(format) ==
                                 ColorHostStorage::R16G16Float ? 2u : 4u;
      sample.components = {0.0f, 0.0f, 0.0f, 1.0f};
      for (unsigned i = 0; i < count; ++i)
        sample.components[i] = half_to_float(words[i]);
      return true;
    }
    default:
      return false;
  }
}

bool edram_color_to_host(ColorRenderTargetFormat format, std::uint32_t width,
                         std::uint32_t height,
                         std::span<const std::byte> source,
                         std::uint32_t source_pitch,
                         std::span<std::byte> destination,
                         std::uint32_t destination_pitch) noexcept {
  const auto canonical_bytes = color_render_target_is_64bpp(format) ? 8u : 4u;
  const auto host_bytes = color_host_bytes_per_pixel(format);
  if (!width || !height || !host_bytes || source_pitch < width * canonical_bytes ||
      destination_pitch < width * host_bytes ||
      std::uint64_t(source_pitch) * height > source.size() ||
      std::uint64_t(destination_pitch) * height > destination.size()) return false;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::array<std::uint32_t, 2> packed{};
      std::memcpy(packed.data(), source.data() + std::uint64_t(y) * source_pitch +
                                    std::uint64_t(x) * canonical_bytes,
                  canonical_bytes);
      if (!encode_host_color_sample(
              format, unpack_color_sample(format, packed),
              destination.subspan(std::uint64_t(y) * destination_pitch +
                                       std::uint64_t(x) * host_bytes,
                                   host_bytes))) return false;
    }
  }
  return true;
}

bool host_color_to_edram(ColorRenderTargetFormat format, std::uint32_t width,
                         std::uint32_t height,
                         std::span<const std::byte> source,
                         std::uint32_t source_pitch,
                         std::span<std::byte> destination,
                         std::uint32_t destination_pitch) noexcept {
  const auto canonical_bytes = color_render_target_is_64bpp(format) ? 8u : 4u;
  const auto host_bytes = color_host_bytes_per_pixel(format);
  if (!width || !height || !host_bytes || source_pitch < width * host_bytes ||
      destination_pitch < width * canonical_bytes ||
      std::uint64_t(source_pitch) * height > source.size() ||
      std::uint64_t(destination_pitch) * height > destination.size()) return false;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      ColorSample sample{};
      if (!decode_host_color_sample(
              format,
              source.subspan(std::uint64_t(y) * source_pitch +
                                  std::uint64_t(x) * host_bytes,
                              host_bytes),
              sample)) return false;
      const auto packed = pack_color_sample(format, sample);
      std::memcpy(destination.data() + std::uint64_t(y) * destination_pitch +
                      std::uint64_t(x) * canonical_bytes,
                  packed.data(), canonical_bytes);
    }
  }
  return true;
}

std::optional<HostSampleMapping> map_guest_sample_to_host(
    MsaaSamples samples, std::uint32_t guest_sample,
    bool native_2x_supported) noexcept {
  const auto sample_count = 1u << static_cast<unsigned>(samples);
  if (samples > MsaaSamples::X4 || guest_sample >= sample_count) {
    return std::nullopt;
  }
  if (samples == MsaaSamples::X2) {
    if (native_2x_supported) {
      return HostSampleMapping{guest_sample ? 0u : 1u, 2u};
    }
    return HostSampleMapping{guest_sample ? 3u : 0u, 4u};
  }
  return HostSampleMapping{guest_sample, sample_count};
}

std::optional<EdramSampleAddress> edram_sample_address(
    const EdramSurfaceLayout& layout, std::uint32_t x, std::uint32_t y,
    std::uint32_t sample) noexcept {
  if (!layout.valid() || x >= layout.pitch_pixels || y >= layout.height_pixels)
    return std::nullopt;
  const auto sample_count = 1u << static_cast<unsigned>(layout.msaa);
  if (sample >= sample_count) return std::nullopt;

  const auto sample_x = (x << (layout.msaa == MsaaSamples::X4 ? 1u : 0u)) |
                        (layout.msaa == MsaaSamples::X4 ? (sample & 1u) : 0u);
  const auto sample_y = (y << (layout.msaa == MsaaSamples::X1 ? 0u : 1u)) |
                        (layout.msaa == MsaaSamples::X1 ? 0u :
                         layout.msaa == MsaaSamples::X2 ? sample : sample >> 1u);
  const auto tile_x = sample_x / 80u;
  const auto tile_y = sample_y / 16u;
  auto in_tile_x = sample_x % 80u;
  // Xenos depth tiles exchange their two 40-sample halves.
  if (layout.depth) in_tile_x = (in_tile_x + 40u) % 80u;
  const auto word = (sample_y % 16u) * 80u + in_tile_x;
  const auto tile_stride = layout.is_64bpp ? 2u : 1u;
  const auto tile = (layout.base_tile + tile_y * layout.pitch_tiles() +
                     tile_x * tile_stride) % Edram::kTileCount;
  EdramSampleAddress result{};
  result.low = tile * Edram::kTileBytes + word * 4u;
  result.has_high = layout.is_64bpp;
  if (result.has_high) {
    const auto high_tile = (tile + 1u) % Edram::kTileCount;
    result.high = high_tile * Edram::kTileBytes + word * 4u;
  }
  return result;
}

std::vector<std::uint16_t> covered_edram_tiles(
    const EdramSurfaceLayout& layout, EdramSurfaceRegion region) {
  std::vector<std::uint16_t> result;
  if (!layout.valid()) return result;
  region.left = std::clamp(region.left, 0, int(layout.pitch_pixels));
  region.top = std::clamp(region.top, 0, int(layout.height_pixels));
  region.right = std::clamp(region.right, 0, int(layout.pitch_pixels));
  region.bottom = std::clamp(region.bottom, 0, int(layout.height_pixels));
  if (region.left >= region.right || region.top >= region.bottom) return result;
  std::array<bool, Edram::kTileCount> seen{};
  const auto sample_count = 1u << static_cast<unsigned>(layout.msaa);
  for (auto y = region.top; y < region.bottom; ++y) {
    for (auto x = region.left; x < region.right; ++x) {
      for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        const auto address = edram_sample_address(
            layout, std::uint32_t(x), std::uint32_t(y), sample);
        if (!address) continue;
        const auto add = [&](std::uint32_t byte_address) {
          const auto tile = static_cast<std::uint16_t>(
              byte_address / Edram::kTileBytes);
          if (!seen[tile]) { seen[tile] = true; result.push_back(tile); }
        };
        add(address->low);
        if (address->has_high) add(address->high);
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::array<std::uint32_t, 2> read_edram_sample(
    const Edram& edram, const EdramSurfaceLayout& layout, std::uint32_t x,
    std::uint32_t y, std::uint32_t sample) noexcept {
  const auto address = edram_sample_address(layout, x, y, sample);
  if (!address) return {};
  return {read_u32(edram, address->low),
          address->has_high ? read_u32(edram, address->high) : 0u};
}

bool write_edram_sample(Edram& edram, const EdramSurfaceLayout& layout,
                        std::uint32_t x, std::uint32_t y,
                        std::array<std::uint32_t, 2> value,
                        std::uint32_t sample) noexcept {
  const auto address = edram_sample_address(layout, x, y, sample);
  if (!address) return false;
  write_u32(edram, address->low, value[0]);
  if (address->has_high) write_u32(edram, address->high, value[1]);
  return true;
}

bool clear_edram_surface_region(Edram& edram,
                                const EdramSurfaceLayout& layout,
                                std::int32_t left, std::int32_t top,
                                std::int32_t right, std::int32_t bottom,
                                std::array<std::uint32_t, 2> value) noexcept {
  if (!layout.valid()) return false;
  const auto clipped_left = std::clamp(left, 0, int(layout.pitch_pixels));
  const auto clipped_top = std::clamp(top, 0, int(layout.height_pixels));
  const auto clipped_right = std::clamp(right, 0, int(layout.pitch_pixels));
  const auto clipped_bottom = std::clamp(bottom, 0, int(layout.height_pixels));
  if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) return true;
  const auto sample_count = 1u << static_cast<unsigned>(layout.msaa);
  for (auto y = clipped_top; y < clipped_bottom; ++y) {
    for (auto x = clipped_left; x < clipped_right; ++x) {
      for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        if (!write_edram_sample(edram, layout, std::uint32_t(x),
                                std::uint32_t(y), value, sample)) return false;
      }
    }
  }
  return true;
}

bool resolve_edram_raw(const Edram& edram, const EdramSurfaceLayout& layout,
                       std::uint32_t left, std::uint32_t top,
                       std::uint32_t width, std::uint32_t height,
                       std::uint32_t sample, std::span<std::byte> destination,
                       std::uint32_t destination_pitch) noexcept {
  const auto pixel_bytes = layout.is_64bpp ? 8u : 4u;
  if (!width || !height || left + width > layout.pitch_pixels ||
      top + height > layout.height_pixels ||
      destination_pitch < width * pixel_bytes ||
      std::uint64_t(destination_pitch) * height > destination.size()) return false;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto value = read_edram_sample(edram, layout, left + x, top + y, sample);
      auto* out = destination.data() + std::uint64_t(y) * destination_pitch +
                  std::uint64_t(x) * pixel_bytes;
      std::memcpy(out, &value[0], 4);
      if (layout.is_64bpp) std::memcpy(out + 4, &value[1], 4);
    }
  }
  return true;
}

bool store_edram_raw(Edram& edram, const EdramSurfaceLayout& layout,
                     std::uint32_t left, std::uint32_t top,
                     std::uint32_t width, std::uint32_t height,
                     std::uint32_t sample,
                     std::span<const std::byte> source,
                     std::uint32_t source_pitch) noexcept {
  const auto pixel_bytes = layout.is_64bpp ? 8u : 4u;
  if (!width || !height || left + width > layout.pitch_pixels ||
      top + height > layout.height_pixels ||
      source_pitch < width * pixel_bytes ||
      std::uint64_t(source_pitch) * height > source.size()) return false;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::array<std::uint32_t, 2> value{};
      const auto* in = source.data() + std::uint64_t(y) * source_pitch +
                       std::uint64_t(x) * pixel_bytes;
      std::memcpy(&value[0], in, 4);
      if (layout.is_64bpp) std::memcpy(&value[1], in + 4, 4);
      if (!write_edram_sample(edram, layout, left + x, top + y, value,
                              sample)) return false;
    }
  }
  return true;
}

}  // namespace xenon::gpu
