#include "xenon/gpu/texture_replacement.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace xenon::gpu {

namespace {

struct HostBlock {
  std::uint32_t block{1};  // texels per block side
  std::uint32_t bytes{};   // bytes per block
};

HostBlock host_block(TextureHostFormat format) noexcept {
  switch (format) {
    case TextureHostFormat::R8Unorm: return {1, 1};
    case TextureHostFormat::R8G8Unorm:
    case TextureHostFormat::B5G5R5A1Unorm:
    case TextureHostFormat::B5G6R5Unorm:
    case TextureHostFormat::B4G4R4A4Unorm:
    case TextureHostFormat::R16Unorm:
    case TextureHostFormat::R16Float: return {1, 2};
    case TextureHostFormat::R8G8B8A8Unorm:
    case TextureHostFormat::R8G8B8A8Srgb:
    case TextureHostFormat::R10G10B10A2Unorm:
    case TextureHostFormat::R11G11B10Float:
    case TextureHostFormat::R16G16Unorm:
    case TextureHostFormat::R16G16Float:
    case TextureHostFormat::R32Uint:
    case TextureHostFormat::R32Float:
    case TextureHostFormat::D24UnormS8Uint: return {1, 4};
    case TextureHostFormat::R16G16B16A16Unorm:
    case TextureHostFormat::R16G16B16A16Float:
    case TextureHostFormat::R32G32Uint:
    case TextureHostFormat::R32G32Float: return {1, 8};
    case TextureHostFormat::R32G32B32Float: return {1, 12};
    case TextureHostFormat::R32G32B32A32Uint:
    case TextureHostFormat::R32G32B32A32Float: return {1, 16};
    case TextureHostFormat::BC1Unorm:
    case TextureHostFormat::BC4Unorm: return {4, 8};
    case TextureHostFormat::BC2Unorm:
    case TextureHostFormat::BC3Unorm:
    case TextureHostFormat::BC5Unorm: return {4, 16};
    case TextureHostFormat::Unsupported: break;
  }
  return {1, 0};
}

bool block_compressed(TextureHostFormat format) noexcept { return host_block(format).block == 4u; }

// Formats that sample as the same four colour channels, so a replacement may
// trade one for another (a BC1 asset replaced by an uncompressed RGBA8 one).
bool four_channel_colour(TextureHostFormat format) noexcept {
  switch (format) {
    case TextureHostFormat::R8G8B8A8Unorm:
    case TextureHostFormat::R8G8B8A8Srgb:
    case TextureHostFormat::BC1Unorm:
    case TextureHostFormat::BC2Unorm:
    case TextureHostFormat::BC3Unorm: return true;
    default: return false;
  }
}

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
  return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8u) |
         (std::uint32_t(std::uint8_t(c)) << 16u) | (std::uint32_t(std::uint8_t(d)) << 24u);
}

std::uint32_t load32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

void store32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

constexpr std::size_t kHeaderSize = 4u + 124u;
constexpr std::size_t kDx10HeaderSize = 20u;
constexpr std::uint32_t kDdsdCaps = 0x1u, kDdsdHeight = 0x2u, kDdsdWidth = 0x4u,
                        kDdsdPixelFormat = 0x1000u, kDdsdMipMapCount = 0x20000u,
                        kDdsdLinearSize = 0x80000u;
constexpr std::uint32_t kDdpfAlphaPixels = 0x1u, kDdpfFourCC = 0x4u, kDdpfRgb = 0x40u;
constexpr std::uint32_t kCapsComplex = 0x8u, kCapsTexture = 0x1000u, kCapsMipMap = 0x400000u;
constexpr std::uint32_t kCaps2Cubemap = 0x200u, kCaps2Volume = 0x200000u;
constexpr std::uint32_t kDimensionTexture2D = 3u;

void set_error(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

}  // namespace

std::uint32_t dds_dxgi_format(TextureHostFormat format) noexcept {
  switch (format) {
    case TextureHostFormat::R8Unorm: return 61u;
    case TextureHostFormat::R8G8Unorm: return 49u;
    case TextureHostFormat::R8G8B8A8Unorm: return 28u;
    case TextureHostFormat::R8G8B8A8Srgb: return 29u;
    case TextureHostFormat::B5G5R5A1Unorm: return 86u;
    case TextureHostFormat::B5G6R5Unorm: return 85u;
    case TextureHostFormat::B4G4R4A4Unorm: return 115u;
    case TextureHostFormat::R10G10B10A2Unorm: return 24u;
    case TextureHostFormat::R11G11B10Float: return 26u;
    case TextureHostFormat::R16Unorm: return 56u;
    case TextureHostFormat::R16G16Unorm: return 35u;
    case TextureHostFormat::R16G16B16A16Unorm: return 11u;
    case TextureHostFormat::R16Float: return 54u;
    case TextureHostFormat::R16G16Float: return 34u;
    case TextureHostFormat::R16G16B16A16Float: return 10u;
    case TextureHostFormat::R32Uint: return 42u;
    case TextureHostFormat::R32G32Uint: return 17u;
    case TextureHostFormat::R32G32B32A32Uint: return 3u;
    case TextureHostFormat::R32Float: return 41u;
    case TextureHostFormat::R32G32Float: return 16u;
    case TextureHostFormat::R32G32B32Float: return 6u;
    case TextureHostFormat::R32G32B32A32Float: return 2u;
    case TextureHostFormat::BC1Unorm: return 71u;
    case TextureHostFormat::BC2Unorm: return 74u;
    case TextureHostFormat::BC3Unorm: return 77u;
    case TextureHostFormat::BC4Unorm: return 80u;
    case TextureHostFormat::BC5Unorm: return 83u;
    // Depth has no colour DDS form; it is never an authored asset.
    case TextureHostFormat::D24UnormS8Uint:
    case TextureHostFormat::Unsupported: break;
  }
  return 0u;
}

TextureHostFormat host_format_from_dds_dxgi(std::uint32_t dxgi) noexcept {
  switch (dxgi) {
    case 61u: return TextureHostFormat::R8Unorm;
    case 49u: return TextureHostFormat::R8G8Unorm;
    case 28u: return TextureHostFormat::R8G8B8A8Unorm;
    case 29u: return TextureHostFormat::R8G8B8A8Srgb;
    case 86u: return TextureHostFormat::B5G5R5A1Unorm;
    case 85u: return TextureHostFormat::B5G6R5Unorm;
    case 115u: return TextureHostFormat::B4G4R4A4Unorm;
    case 24u: return TextureHostFormat::R10G10B10A2Unorm;
    case 26u: return TextureHostFormat::R11G11B10Float;
    case 56u: return TextureHostFormat::R16Unorm;
    case 35u: return TextureHostFormat::R16G16Unorm;
    case 11u: return TextureHostFormat::R16G16B16A16Unorm;
    case 54u: return TextureHostFormat::R16Float;
    case 34u: return TextureHostFormat::R16G16Float;
    case 10u: return TextureHostFormat::R16G16B16A16Float;
    case 42u: return TextureHostFormat::R32Uint;
    case 17u: return TextureHostFormat::R32G32Uint;
    case 3u: return TextureHostFormat::R32G32B32A32Uint;
    case 41u: return TextureHostFormat::R32Float;
    case 16u: return TextureHostFormat::R32G32Float;
    case 6u: return TextureHostFormat::R32G32B32Float;
    case 2u: return TextureHostFormat::R32G32B32A32Float;
    // The sRGB BCn variants hold the same bits; the texture they replace
    // decides how they are sampled.
    case 71u: case 72u: return TextureHostFormat::BC1Unorm;
    case 74u: case 75u: return TextureHostFormat::BC2Unorm;
    case 77u: case 78u: return TextureHostFormat::BC3Unorm;
    case 80u: return TextureHostFormat::BC4Unorm;
    case 83u: return TextureHostFormat::BC5Unorm;
    default: break;
  }
  return TextureHostFormat::Unsupported;
}

std::uint64_t dds_level_size(TextureHostFormat format, std::uint32_t width,
                             std::uint32_t height) noexcept {
  const auto block = host_block(format);
  if (!block.bytes) return 0u;
  const auto columns = (std::max(width, 1u) + block.block - 1u) / block.block;
  const auto rows = (std::max(height, 1u) + block.block - 1u) / block.block;
  return std::uint64_t{columns} * rows * block.bytes;
}

std::vector<std::byte> encode_dds(const DdsImage& image) {
  std::vector<std::byte> file(kHeaderSize + kDx10HeaderSize);
  store32(file, 0, fourcc('D', 'D', 'S', ' '));
  store32(file, 4, 124u);
  store32(file, 8, kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat |
                       kDdsdMipMapCount | kDdsdLinearSize);
  store32(file, 12, image.height);
  store32(file, 16, image.width);
  store32(file, 20, static_cast<std::uint32_t>(dds_level_size(image.format, image.width, image.height)));
  store32(file, 28, std::max(image.mip_levels, 1u));
  store32(file, 76, 32u);
  store32(file, 80, kDdpfFourCC);
  store32(file, 84, fourcc('D', 'X', '1', '0'));
  store32(file, 108, kCapsTexture | (image.mip_levels > 1u ? kCapsMipMap | kCapsComplex : 0u));
  store32(file, 128, dds_dxgi_format(image.format));
  store32(file, 132, kDimensionTexture2D);
  store32(file, 140, 1u);
  file.insert(file.end(), image.data.begin(), image.data.end());
  return file;
}

std::optional<DdsImage> decode_dds(std::span<const std::byte> file, std::string* error) {
  if (file.size() < kHeaderSize || load32(file, 0) != fourcc('D', 'D', 'S', ' ') ||
      load32(file, 4) != 124u) {
    set_error(error, "not a DDS file");
    return std::nullopt;
  }
  DdsImage image{};
  image.height = load32(file, 12);
  image.width = load32(file, 16);
  const auto flags = load32(file, 8);
  image.mip_levels = (flags & kDdsdMipMapCount) ? std::max(load32(file, 28), 1u) : 1u;
  if (load32(file, 112) & (kCaps2Cubemap | kCaps2Volume)) {
    set_error(error, "cube and volume DDS files are not 2D textures");
    return std::nullopt;
  }
  const auto pixel_flags = load32(file, 80);
  const auto code = load32(file, 84);
  std::size_t data_offset = kHeaderSize;
  bool swap_red_blue = false;
  bool force_opaque = false;
  if ((pixel_flags & kDdpfFourCC) && code == fourcc('D', 'X', '1', '0')) {
    if (file.size() < kHeaderSize + kDx10HeaderSize) {
      set_error(error, "truncated DDS DX10 header");
      return std::nullopt;
    }
    const auto dxgi = load32(file, 128);
    if (load32(file, 132) != kDimensionTexture2D || load32(file, 140) > 1u) {
      set_error(error, "DDS file is not a single 2D texture");
      return std::nullopt;
    }
    image.format = host_format_from_dds_dxgi(dxgi);
    // B8G8R8A8 (and its sRGB form) as RGBA8.
    if (dxgi == 87u || dxgi == 91u) {
      image.format = dxgi == 91u ? TextureHostFormat::R8G8B8A8Srgb
                                 : TextureHostFormat::R8G8B8A8Unorm;
      swap_red_blue = true;
    }
    data_offset += kDx10HeaderSize;
  } else if (pixel_flags & kDdpfFourCC) {
    if (code == fourcc('D', 'X', 'T', '1')) image.format = TextureHostFormat::BC1Unorm;
    else if (code == fourcc('D', 'X', 'T', '2') || code == fourcc('D', 'X', 'T', '3'))
      image.format = TextureHostFormat::BC2Unorm;
    else if (code == fourcc('D', 'X', 'T', '4') || code == fourcc('D', 'X', 'T', '5'))
      image.format = TextureHostFormat::BC3Unorm;
    else if (code == fourcc('A', 'T', 'I', '1') || code == fourcc('B', 'C', '4', 'U'))
      image.format = TextureHostFormat::BC4Unorm;
    else if (code == fourcc('A', 'T', 'I', '2') || code == fourcc('B', 'C', '5', 'U'))
      image.format = TextureHostFormat::BC5Unorm;
  } else if ((pixel_flags & kDdpfRgb) && load32(file, 88) == 32u) {
    const auto red = load32(file, 92), green = load32(file, 96), blue = load32(file, 100);
    const auto alpha = (pixel_flags & kDdpfAlphaPixels) ? load32(file, 104) : 0u;
    if (green == 0x0000FF00u && (alpha == 0xFF000000u || alpha == 0u)) {
      if (red == 0x000000FFu && blue == 0x00FF0000u) {
        image.format = TextureHostFormat::R8G8B8A8Unorm;
      } else if (red == 0x00FF0000u && blue == 0x000000FFu) {
        image.format = TextureHostFormat::R8G8B8A8Unorm;
        swap_red_blue = true;
      }
      force_opaque = alpha == 0u;
    }
  }
  if (image.format == TextureHostFormat::Unsupported) {
    set_error(error, "DDS pixel format is not one Xenon can sample");
    return std::nullopt;
  }
  if (!image.width || !image.height || image.width > 16384u || image.height > 16384u) {
    set_error(error, "DDS extent is out of range");
    return std::nullopt;
  }
  const auto full_chain = static_cast<std::uint32_t>(
      std::bit_width(std::max(image.width, image.height)));
  image.mip_levels = std::min(image.mip_levels, full_chain);
  std::uint64_t size = 0u;
  for (std::uint32_t level = 0; level < image.mip_levels; ++level)
    size += dds_level_size(image.format, std::max(1u, image.width >> level),
                           std::max(1u, image.height >> level));
  if (data_offset + size > file.size()) {
    set_error(error, "DDS file is shorter than its mip chain");
    return std::nullopt;
  }
  const auto data = file.subspan(data_offset, static_cast<std::size_t>(size));
  image.data.assign(data.begin(), data.end());
  if (swap_red_blue || force_opaque) {
    for (std::size_t i = 0; i + 4u <= image.data.size(); i += 4u) {
      if (swap_red_blue) std::swap(image.data[i], image.data[i + 2u]);
      if (force_opaque) image.data[i + 3u] = std::byte{0xFF};
    }
  }
  return image;
}

std::optional<std::uint64_t> texture_content_hash(const TextureLayout& layout,
                                                  std::span<const std::byte> guest,
                                                  std::uint32_t guest_base) {
  if (!layout.valid || layout.subresources.empty()) return std::nullopt;
  const auto& base = layout.subresources.front();
  if (base.guest_address < guest_base) return std::nullopt;
  // The texels' own footprint, not the 4 KiB-aligned stride after it: the
  // bytes past the footprint may belong to the next allocation.
  const auto slices = layout.descriptor.dimension == TextureDimension::ThreeD
                          ? std::max(base.depth_texels, 1u)
                          : 1u;
  const auto footprint = std::min<std::uint64_t>(
      base.guest_size_bytes, std::uint64_t{base.storage_pitch_blocks} *
                                 layout.format.bytes_per_block() * base.storage_height_blocks *
                                 slices);
  const auto offset = std::uint64_t{base.guest_address - guest_base};
  if (!footprint || offset + footprint > guest.size()) return std::nullopt;

  constexpr std::uint64_t kPrime = 0x100000001B3ull;
  std::uint64_t hash = 0xCBF29CE484222325ull;
  const auto mix = [&](std::uint64_t word) { hash = (hash ^ word) * kPrime; };
  const auto& d = layout.descriptor;
  mix(d.format);
  mix(std::uint64_t{d.width} << 32u | d.height);
  mix(std::uint64_t{d.depth} << 32u | d.pitch);
  mix(std::uint64_t(d.dimension) | std::uint64_t(d.endian) << 8u | std::uint64_t(d.tiled) << 16u |
      std::uint64_t(d.stacked) << 17u | std::uint64_t(d.packed_mips) << 18u);
  const auto bytes = guest.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(footprint));
  std::size_t i = 0;
  for (; i + 8u <= bytes.size(); i += 8u) {
    std::uint64_t word{};
    std::memcpy(&word, bytes.data() + i, 8u);
    mix(word);
  }
  for (; i < bytes.size(); ++i) mix(std::to_integer<std::uint8_t>(bytes[i]));
  // Final avalanche (splitmix64), so names differ in every digit.
  hash ^= hash >> 30u;
  hash *= 0xBF58476D1CE4E5B9ull;
  hash ^= hash >> 27u;
  hash *= 0x94D049BB133111EBull;
  hash ^= hash >> 31u;
  return hash;
}

std::optional<DecodedTexture> replacement_texture(const TextureLayout& original,
                                                  const DdsImage& replacement,
                                                  std::string* error) {
  const auto& source = original.descriptor;
  if (!original.valid || source.dimension != TextureDimension::TwoDOrStacked ||
      (source.stacked && source.depth > 1u)) {
    set_error(error, "only single 2D textures can be replaced");
    return std::nullopt;
  }
  auto target = replacement.format;
  const auto original_host = original.format.host_format;
  if (target != original_host) {
    if (!four_channel_colour(target) || !four_channel_colour(original_host)) {
      set_error(error, "replacement format does not sample like the original");
      return std::nullopt;
    }
    // Uncompressed RGBA8 keeps the original's sRGB-ness.
    if (target == TextureHostFormat::R8G8B8A8Unorm &&
        original_host == TextureHostFormat::R8G8B8A8Srgb)
      target = TextureHostFormat::R8G8B8A8Srgb;
  }
  if (block_compressed(target) && (replacement.width % 4u || replacement.height % 4u)) {
    set_error(error, "block-compressed replacements need a width and height divisible by 4");
    return std::nullopt;
  }
  // The guest format whose host image is the replacement's.
  std::optional<std::uint8_t> format;
  if (texture_format_info(source.format).host_format == target) format = source.format;
  for (std::uint32_t candidate = 0; !format && candidate < 64u; ++candidate) {
    const auto& info = texture_format_info(static_cast<std::uint8_t>(candidate));
    if (info.host_format == target && info.load == TextureLoad::Copy &&
        info.storage != TextureStorage::Packed)
      format = static_cast<std::uint8_t>(candidate);
  }
  if (!format) {
    set_error(error, "no guest format samples as the replacement's format");
    return std::nullopt;
  }
  auto descriptor = source;
  descriptor.format = *format;
  descriptor.width = replacement.width;
  descriptor.height = replacement.height;
  descriptor.depth = 1u;
  descriptor.pitch = 0u;
  descriptor.base_address = 0u;
  descriptor.mip_address = 0u;
  descriptor.mip_min_level = 0u;
  descriptor.mip_max_level = static_cast<std::uint8_t>(replacement.mip_levels - 1u);
  descriptor.tiled = false;
  descriptor.stacked = false;
  descriptor.packed_mips = false;
  descriptor.valid = true;
  DecodedTexture decoded{};
  decoded.layout = build_texture_layout(descriptor);
  if (!decoded.layout.valid || decoded.layout.subresources.size() != replacement.mip_levels ||
      decoded.layout.linear_size_bytes != replacement.data.size()) {
    set_error(error, decoded.layout.valid ? "replacement mip chain does not match its layout"
                                          : decoded.layout.error);
    return std::nullopt;
  }
  decoded.linear_data = replacement.data;
  decoded.valid = true;
  return decoded;
}

TextureReplacements TextureReplacements::from_environment() {
  TextureReplacements replacements;
  const char* dump = std::getenv("XENON_TEXTURE_DUMP");
  const char* mods = std::getenv("XENON_TEXTURE_MODS");
  replacements.configure(dump && *dump ? std::filesystem::path(dump) : std::filesystem::path{},
                         mods && *mods ? std::filesystem::path(mods) : std::filesystem::path{});
  return replacements;
}

void TextureReplacements::configure(std::filesystem::path dump_directory,
                                    std::filesystem::path mods_directory) {
  dump_directory_ = std::move(dump_directory);
  dumped_.clear();
  mods_.clear();
  std::error_code ec;
  if (!dump_directory_.empty()) std::filesystem::create_directories(dump_directory_, ec);
  if (mods_directory.empty() || !std::filesystem::is_directory(mods_directory, ec)) return;
  for (std::filesystem::recursive_directory_iterator it(
           mods_directory, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    auto extension = it->path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".dds") continue;
    const auto stem = it->path().stem().string();
    if (stem.size() < 16u) continue;
    std::uint64_t hash = 0u;
    bool hex = true;
    for (std::size_t i = 0; i < 16u && hex; ++i) {
      const auto c = static_cast<unsigned char>(stem[i]);
      if (!std::isxdigit(c)) hex = false;
      else hash = (hash << 4u) | static_cast<std::uint64_t>(
                                     std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10);
    }
    if (hex) mods_.try_emplace(hash, it->path());
  }
}

std::string TextureReplacements::file_stem(std::uint64_t hash) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string stem(16u, '0');
  for (int i = 15; i >= 0; --i, hash >>= 4u) stem[static_cast<std::size_t>(i)] = kDigits[hash & 15u];
  return stem;
}

bool TextureReplacements::dump(std::uint64_t hash, const DecodedTexture& texture,
                               std::string* error) {
  if (!dumping() || !dumped_.insert(hash).second) return false;
  const auto& layout = texture.layout;
  if (!texture.valid || layout.subresources.empty() ||
      layout.descriptor.dimension != TextureDimension::TwoDOrStacked ||
      (layout.descriptor.stacked && layout.descriptor.depth > 1u) ||
      !dds_dxgi_format(layout.format.host_format))
    return false;
  DdsImage image{};
  image.format = layout.format.host_format;
  image.width = layout.subresources.front().width_texels;
  image.height = layout.subresources.front().height_texels;
  image.mip_levels = static_cast<std::uint32_t>(layout.subresources.size());
  image.data = texture.linear_data;
  std::string name = file_stem(hash) + "_" + std::to_string(image.width) + "x" +
                     std::to_string(image.height) + "_";
  for (const char c : layout.format.name)
    name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  const auto path = dump_directory_ / (name + ".dds");
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return false;
  const auto file = encode_dds(image);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  if (!out) {
    set_error(error, "could not write texture dump " + path.string());
    return false;
  }
  return true;
}

const std::filesystem::path* TextureReplacements::find(std::uint64_t hash) const {
  const auto found = mods_.find(hash);
  return found == mods_.end() ? nullptr : &found->second;
}

std::optional<DecodedTexture> TextureReplacements::load(std::uint64_t hash,
                                                        const TextureLayout& original,
                                                        std::string* error) const {
  const auto* path = find(hash);
  if (!path) return std::nullopt;
  std::ifstream in(*path, std::ios::binary);
  std::vector<std::byte> file;
  if (in) {
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    file.resize(size);
    in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(size));
  }
  if (!in || file.empty()) {
    set_error(error, "could not read " + path->string());
    return std::nullopt;
  }
  std::string reason;
  const auto image = decode_dds(file, &reason);
  if (!image) {
    set_error(error, path->string() + ": " + reason);
    return std::nullopt;
  }
  auto decoded = replacement_texture(original, *image, &reason);
  if (!decoded) set_error(error, path->string() + ": " + reason);
  return decoded;
}

}  // namespace xenon::gpu
