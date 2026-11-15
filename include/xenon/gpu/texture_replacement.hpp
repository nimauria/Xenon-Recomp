#pragma once

// Texture dumping and replacement ("texture mods"), for every title.
//
// A texture the guest loaded from its own data is identified by a hash of
// its guest bytes (base level, as stored: tiled and in guest endian) and its
// shape, so the same asset gets the same name on every run and every
// machine. With a dump directory set, each such texture is written once as
// <hash>.dds in the texels the host image holds; with a mods directory set,
// a <hash>*.dds anywhere under it is sampled in its place - at any size and
// with its own mip chain. Render targets the GPU wrote (resolves, memory
// exports) are never dumped or replaced.
//
// XENON_TEXTURE_DUMP / XENON_TEXTURE_MODS name the directories.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenon/gpu/texture.hpp"

namespace xenon::gpu {

// DXGI_FORMAT values (the DDS DX10 header's), API-neutral.
[[nodiscard]] std::uint32_t dds_dxgi_format(TextureHostFormat format) noexcept;
[[nodiscard]] TextureHostFormat host_format_from_dds_dxgi(std::uint32_t dxgi) noexcept;

struct DdsImage {
  TextureHostFormat format{TextureHostFormat::Unsupported};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t mip_levels{};
  // Every level, largest first, rows tightly packed (blocks for BCn).
  std::vector<std::byte> data{};
};

// Bytes of one mip level of a tightly packed 2D image.
[[nodiscard]] std::uint64_t dds_level_size(TextureHostFormat format, std::uint32_t width,
                                           std::uint32_t height) noexcept;
// A DDS file with a DX10 header holding `image`.
[[nodiscard]] std::vector<std::byte> encode_dds(const DdsImage& image);
// Reads a DDS file: DX10 headers, the legacy DXT1/DXT3/DXT5/ATI1/ATI2/BC4U/
// BC5U FourCCs and legacy 32-bit RGBA/BGRA masks (BGRA is swapped to RGBA).
[[nodiscard]] std::optional<DdsImage> decode_dds(std::span<const std::byte> file,
                                                 std::string* error = nullptr);

// The identity of a guest-authored texture: its base level's guest bytes and
// its format and extent. `guest` holds guest memory from `guest_base` on and
// must cover the base level.
[[nodiscard]] std::optional<std::uint64_t> texture_content_hash(
    const TextureLayout& layout, std::span<const std::byte> guest, std::uint32_t guest_base);

// The host texels to sample in place of a texture of `original` from a DDS
// replacement: a 2D layout of the replacement's own extent and mip chain.
// The replacement must hold the original's host format or, for a four-channel
// colour texture, any of RGBA8/BC1/BC2/BC3 (they sample alike).
[[nodiscard]] std::optional<DecodedTexture> replacement_texture(
    const TextureLayout& original, const DdsImage& replacement, std::string* error = nullptr);

class TextureReplacements {
 public:
  // XENON_TEXTURE_DUMP and XENON_TEXTURE_MODS.
  [[nodiscard]] static TextureReplacements from_environment();
  void configure(std::filesystem::path dump_directory, std::filesystem::path mods_directory);

  [[nodiscard]] bool active() const noexcept { return dumping() || replacing(); }
  [[nodiscard]] bool dumping() const noexcept { return !dump_directory_.empty(); }
  [[nodiscard]] bool replacing() const noexcept { return !mods_.empty(); }
  [[nodiscard]] std::size_t mod_count() const noexcept { return mods_.size(); }

  [[nodiscard]] static std::string file_stem(std::uint64_t hash);
  // Writes `texture` (2D, one layer) as <hash>.dds once per hash; false with
  // `error` when it cannot be written, false without one when it was already
  // dumped or has no DDS form.
  [[nodiscard]] bool dump(std::uint64_t hash, const DecodedTexture& texture,
                          std::string* error = nullptr);
  // The replacement file for `hash`, if the mods directory has one.
  [[nodiscard]] const std::filesystem::path* find(std::uint64_t hash) const;
  // Reads and validates the replacement for `hash` against `original`.
  [[nodiscard]] std::optional<DecodedTexture> load(std::uint64_t hash,
                                                   const TextureLayout& original,
                                                   std::string* error = nullptr) const;

 private:
  std::filesystem::path dump_directory_{};
  std::unordered_set<std::uint64_t> dumped_{};
  std::unordered_map<std::uint64_t, std::filesystem::path> mods_{};
};

}  // namespace xenon::gpu
