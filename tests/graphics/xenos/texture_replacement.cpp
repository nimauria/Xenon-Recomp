#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <vector>

#include "xenon/gpu/texture_replacement.hpp"

using namespace xenon::gpu;

namespace {

std::uint8_t guest_format_for(TextureHostFormat host) {
  for (std::uint32_t f = 0; f < 64u; ++f)
    if (texture_format_info(static_cast<std::uint8_t>(f)).host_format == host &&
        texture_format_info(static_cast<std::uint8_t>(f)).load == TextureLoad::Copy)
      return static_cast<std::uint8_t>(f);
  assert(false && "no guest format for host format");
  return 0;
}

TextureDescriptor descriptor_2d(TextureHostFormat host, std::uint32_t width, std::uint32_t height,
                                std::uint32_t base) {
  TextureDescriptor d{};
  d.format = guest_format_for(host);
  d.width = width;
  d.height = height;
  d.base_address = base;
  d.dimension = TextureDimension::TwoDOrStacked;
  d.tiled = true;
  d.valid = true;
  return d;
}

void put32(std::vector<std::byte>& file, std::size_t offset, std::uint32_t value) {
  std::memcpy(file.data() + offset, &value, 4u);
}

// The DDS container round-trips every level, and the legacy headers that
// texture tools still write are read too.
void test_dds_round_trip_and_legacy_headers() {
  DdsImage image{};
  image.format = TextureHostFormat::BC1Unorm;
  image.width = 8u;
  image.height = 8u;
  image.mip_levels = 4u;  // 8, 4, 2, 1: 4 + 1 + 1 + 1 blocks
  assert(dds_level_size(image.format, 8u, 8u) == 32u);
  assert(dds_level_size(image.format, 1u, 1u) == 8u);
  image.data.resize(32u + 8u + 8u + 8u);
  for (std::size_t i = 0; i < image.data.size(); ++i) image.data[i] = static_cast<std::byte>(i);
  const auto file = encode_dds(image);
  const auto back = decode_dds(file);
  assert(back && back->format == image.format && back->width == 8u && back->height == 8u &&
         back->mip_levels == 4u && back->data == image.data);

  // Legacy DXT5 FourCC, one level.
  std::vector<std::byte> legacy(128u + 16u);
  put32(legacy, 0, 0x20534444u);  // "DDS "
  put32(legacy, 4, 124u);
  put32(legacy, 8, 0x1007u);
  put32(legacy, 12, 4u);
  put32(legacy, 16, 4u);
  put32(legacy, 76, 32u);
  put32(legacy, 80, 0x4u);
  put32(legacy, 84, 0x35545844u);  // "DXT5"
  const auto dxt5 = decode_dds(legacy);
  assert(dxt5 && dxt5->format == TextureHostFormat::BC3Unorm && dxt5->mip_levels == 1u);

  // Legacy 32-bit BGRA masks become RGBA8; a file without alpha is opaque.
  std::vector<std::byte> bgra(128u + 4u);
  put32(bgra, 0, 0x20534444u);
  put32(bgra, 4, 124u);
  put32(bgra, 8, 0x1007u);
  put32(bgra, 12, 1u);
  put32(bgra, 16, 1u);
  put32(bgra, 76, 32u);
  put32(bgra, 80, 0x40u);
  put32(bgra, 88, 32u);
  put32(bgra, 92, 0x00FF0000u);
  put32(bgra, 96, 0x0000FF00u);
  put32(bgra, 100, 0x000000FFu);
  bgra[128] = std::byte{0x10};  // B
  bgra[129] = std::byte{0x20};  // G
  bgra[130] = std::byte{0x30};  // R
  bgra[131] = std::byte{0x00};
  const auto rgba = decode_dds(bgra);
  assert(rgba && rgba->format == TextureHostFormat::R8G8B8A8Unorm);
  assert(rgba->data[0] == std::byte{0x30} && rgba->data[2] == std::byte{0x10} &&
         rgba->data[3] == std::byte{0xFF});

  // Truncated mip chains and cube maps are rejected, not read past the end.
  auto short_file = file;
  short_file.resize(short_file.size() - 1u);
  std::string error;
  assert(!decode_dds(short_file, &error) && !error.empty());
  auto cube = file;
  put32(cube, 112, 0x200u);
  assert(!decode_dds(cube));
}

// The name of an asset depends only on its own texels: bytes past its
// footprint (the next allocation, in the 4 KiB stride padding) do not change
// it, while any texel does.
void test_content_hash_covers_exactly_the_texels() {
  constexpr std::uint32_t kBase = 0x10000u;
  const auto layout = build_texture_layout(descriptor_2d(TextureHostFormat::BC1Unorm, 64u, 64u, kBase));
  assert(layout.valid);
  // 64x64 BC1: 16 blocks, pitch aligned to 32 blocks, height to 32 -> 8 KiB.
  std::vector<std::byte> guest(0x4000u);
  for (std::size_t i = 0; i < 0x2000u; ++i) guest[i] = static_cast<std::byte>(i * 7u);
  const auto hash = texture_content_hash(layout, guest, kBase);
  assert(hash);
  assert(texture_content_hash(layout, guest, kBase) == hash);
  guest[0x2000u + 5u] = std::byte{0xEE};
  assert(texture_content_hash(layout, guest, kBase) == hash);
  guest[100] ^= std::byte{1};
  assert(texture_content_hash(layout, guest, kBase) != hash);
  guest[100] ^= std::byte{1};
  // The same bytes as a different format are a different asset.
  const auto other = build_texture_layout(descriptor_2d(TextureHostFormat::BC3Unorm, 32u, 64u, kBase));
  assert(texture_content_hash(other, guest, kBase) != hash);
  // 8x8 R8 occupies 32 x 32 bytes of its 4 KiB stride; the rest is not it.
  const auto small = build_texture_layout(descriptor_2d(TextureHostFormat::R8Unorm, 8u, 8u, kBase));
  assert(small.valid && small.subresources[0].guest_size_bytes == 4096u);
  const auto small_hash = texture_content_hash(small, guest, kBase);
  guest[2000] ^= std::byte{0x55};
  assert(texture_content_hash(small, guest, kBase) == small_hash);
  guest[1000] ^= std::byte{0x55};
  assert(texture_content_hash(small, guest, kBase) != small_hash);
  // A range that does not cover the texture has no hash.
  assert(!texture_content_hash(layout, std::span<const std::byte>(guest).first(0x100u), kBase));
}

// A replacement may be larger and carry its own mips; colour formats that
// sample alike may stand in for each other, others may not.
void test_replacement_layouts() {
  const auto original = build_texture_layout(descriptor_2d(TextureHostFormat::BC1Unorm, 64u, 64u, 0x10000u));
  DdsImage hd{};
  hd.format = TextureHostFormat::R8G8B8A8Unorm;
  hd.width = 256u;
  hd.height = 128u;
  hd.mip_levels = 9u;
  std::uint64_t size = 0u;
  for (std::uint32_t level = 0; level < hd.mip_levels; ++level)
    size += dds_level_size(hd.format, std::max(1u, hd.width >> level), std::max(1u, hd.height >> level));
  hd.data.resize(size, std::byte{0x7F});
  std::string error;
  const auto decoded = replacement_texture(original, hd, &error);
  assert(decoded && decoded->valid);
  assert(decoded->layout.format.host_format == TextureHostFormat::R8G8B8A8Unorm);
  assert(decoded->layout.subresources.size() == 9u);
  assert(decoded->layout.subresources[0].width_texels == 256u);
  assert(decoded->layout.descriptor.mip_max_level == 8u);
  assert(decoded->linear_data.size() == size);

  DdsImage single{};
  single.format = TextureHostFormat::R8Unorm;
  single.width = single.height = 64u;
  single.mip_levels = 1u;
  single.data.resize(64u * 64u);
  assert(!replacement_texture(original, single, &error));

  DdsImage odd{};
  odd.format = TextureHostFormat::BC3Unorm;
  odd.width = 6u;
  odd.height = 8u;
  odd.mip_levels = 1u;
  odd.data.resize(dds_level_size(odd.format, 6u, 8u));
  assert(!replacement_texture(original, odd, &error));
}

// End to end: a dumped texture, renamed with a suffix and placed under the
// mods directory, is found by its hash and replaces the original.
void test_dump_then_replace() {
  const auto root = std::filesystem::temp_directory_path() / "xenon_texture_replacement_tests";
  std::filesystem::remove_all(root);
  const auto dump_dir = root / "dump";
  const auto mods_dir = root / "mods" / "pack";
  std::filesystem::create_directories(mods_dir);

  constexpr std::uint32_t kBase = 0x20000u;
  const auto descriptor = descriptor_2d(TextureHostFormat::BC1Unorm, 32u, 32u, kBase);
  const auto layout = build_texture_layout(descriptor);
  std::vector<std::byte> guest(0x8000u);
  for (std::size_t i = 0; i < guest.size(); ++i) guest[i] = static_cast<std::byte>(i * 13u + 1u);
  const auto decoded = decode_texture(descriptor, guest, kBase);
  assert(decoded.valid);
  const auto hash = texture_content_hash(layout, guest, kBase);
  assert(hash);

  TextureReplacements dumping;
  dumping.configure(dump_dir, {});
  assert(dumping.dumping() && !dumping.replacing());
  std::string error;
  assert(dumping.dump(*hash, decoded, &error));
  assert(!dumping.dump(*hash, decoded, &error));  // once per hash
  std::filesystem::path dumped;
  for (const auto& entry : std::filesystem::directory_iterator(dump_dir)) dumped = entry.path();
  assert(dumped.filename().string().rfind(TextureReplacements::file_stem(*hash), 0) == 0u);

  std::filesystem::copy_file(dumped, mods_dir / (TextureReplacements::file_stem(*hash) + "_sky_hd.DDS"));
  TextureReplacements replacing;
  replacing.configure({}, root / "mods");
  assert(replacing.replacing() && replacing.mod_count() == 1u);
  assert(replacing.find(*hash));
  assert(!replacing.find(*hash ^ 1u));
  const auto replacement = replacing.load(*hash, layout, &error);
  assert(replacement && replacement->valid);
  assert(replacement->layout.subresources[0].width_texels == 32u);
  assert(replacement->linear_data == decoded.linear_data);
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_dds_round_trip_and_legacy_headers();
  test_content_hash_covers_exactly_the_texels();
  test_replacement_layouts();
  test_dump_then_replace();
  std::cout << "xenon_texture_replacement_tests: ok\n";
}
