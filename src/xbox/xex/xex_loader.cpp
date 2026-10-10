#include "xenon/xbox/xex_loader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox {

using namespace detail;

XexFormat detect_xex_format(std::span<const std::byte> bytes) noexcept {
  if (bytes.size() < 4u) return XexFormat::Unknown;
  const auto magic = read_be32(bytes, 0u);
  if (magic == kXex1Magic) return XexFormat::Xex1;
  if (magic == kXex2Magic) return XexFormat::Xex2;
  return XexFormat::Unknown;
}

bool parse_xex_image(std::span<const std::byte> bytes, XexImage& out_image, std::string* error,
                    std::span<const std::byte> reference_image) {
  out_image = {};
  if (bytes.size() < 0x18u) {
    if (error) *error = "XEX payload is too small to contain a valid header.";
    return false;
  }

  const auto format = detect_xex_format(bytes);
  if (format == XexFormat::Unknown) {
    if (error) *error = "Not a recognized XEX1/XEX2 image.";
    return false;
  }
  out_image.format = format;

  const auto module_flags = read_be32(bytes, 4u);
  const auto header_size = read_be32(bytes, 8u);
  const auto security_offset = read_be32(bytes, 0x10u);
  const auto optional_header_count = read_be32(bytes, 0x14u);
  if (header_size < 0x18u || header_size > bytes.size() || header_size > kMaxHeaderBytes) {
    if (error) *error = "XEX header size is invalid.";
    return false;
  }

  out_image.module_flags = module_flags;
  out_image.header_size = header_size;
  out_image.header_bytes.assign(bytes.begin(), bytes.begin() + header_size);
  out_image.security_offset = security_offset;
  out_image.optional_header_count = optional_header_count;
  out_image.is_patch = (module_flags & module_flags::kModulePatch) != 0u;
  out_image.is_full_patch = (module_flags & module_flags::kPatchFull) != 0u;
  out_image.is_delta_patch = (module_flags & module_flags::kPatchDelta) != 0u;

  std::vector<OptionalHeaderEntry> entries;
  if (!enumerate_optional_headers(bytes, header_size, optional_header_count, entries, error)) {
    return false;
  }

  if (!parse_security_info(bytes, format, header_size, security_offset, out_image.security, error)) {
    return false;
  }
  out_image.region = out_image.security.region;

  parse_execution_info(bytes, header_size, entries, out_image);
  parse_original_pe_name(bytes, header_size, entries, out_image.original_pe_name);
  parse_native_tls(bytes, header_size, entries, out_image.tls);
  parse_delta_patch_descriptor(bytes, header_size, entries, out_image.delta_patch);

  FileFormatInfo format_info{};
  parse_file_format_info(bytes, header_size, entries, format_info);
  out_image.encryption_type = format_info.encryption;
  out_image.compression_type = format_info.compression;

  if (!decompress_body(bytes, header_size, out_image.security, format_info, reference_image,
                       out_image.delta_patch, out_image.effective_image, error)) {
    return false;
  }

  // Resolve the effective load address before anything derives section
  // virtual addresses from it: security_info.load_address is authoritative,
  // XEX_HEADER_IMAGE_BASE_ADDRESS is only a fallback for the (malformed)
  // case where security info carries no load address at all.
  out_image.image_base = out_image.security.load_address;
  if (out_image.image_base == 0u) {
    if (const auto* base_entry = find_opt_header(entries, kHeaderImageBaseAddress);
        base_entry && opt_header_is_inline(base_entry->key) && base_entry->raw != 0u) {
      out_image.image_base = base_entry->raw;
    } else {
      out_image.image_base = memory::kXex64KBase;
    }
  }

  std::uint32_t entry_point_rva = 0u;
  if (!try_parse_pe_sections(out_image.effective_image, out_image.image_base, out_image.sections,
                            entry_point_rva, error)) {
    return false;
  }

  if (const auto* entry = find_opt_header(entries, kHeaderEntryPoint);
      entry && opt_header_is_inline(entry->key) && entry->raw != 0u) {
    out_image.entry_point = entry->raw;
  } else {
    out_image.entry_point = out_image.image_base + entry_point_rva;
  }

  // Cross-check every PE section's protection against the ground-truth page
  // descriptor run covering it; page descriptors win when present (they are
  // what the real Xbox 360 loader actually enforces), PE characteristics
  // remain the fallback for sections outside the descriptor coverage.
  for (auto& section : out_image.sections) {
    const auto descriptor_protect =
        protect_for_address(out_image.security, static_cast<std::uint32_t>(section.virtual_address));
    if (descriptor_protect != memory::Protect::None) {
      section.protect = descriptor_protect;
      section.readable = memory::has(descriptor_protect, memory::Protect::Read);
      section.writable = memory::has(descriptor_protect, memory::Protect::Write);
      section.executable = memory::has(descriptor_protect, memory::Protect::Execute);
    }
  }

  // Re-derive the data directory location the same way try_parse_pe_sections
  // did, to parse export/import/tls/relocation/exception directories.
  const auto pe_header_offset = read_le32(out_image.effective_image, 0x3Cu);
  const auto coff_offset = static_cast<std::size_t>(pe_header_offset) + 4u;
  const auto optional_header_offset = coff_offset + 20u;
  const auto optional_magic = read_le16(out_image.effective_image, optional_header_offset);
  const auto data_directory_offset =
      optional_header_offset + (optional_magic == 0x10bu ? 0x60u : 0x70u);
  const auto export_rva = read_le32(out_image.effective_image, data_directory_offset + 0u);
  const auto import_rva = read_le32(out_image.effective_image, data_directory_offset + 8u);
  const auto tls_rva = read_le32(out_image.effective_image, data_directory_offset + 9u * 8u);
  const auto relocation_rva = read_le32(out_image.effective_image, data_directory_offset + 5u * 8u);
  const auto relocation_size = read_le32(out_image.effective_image, data_directory_offset + 5u * 8u + 4u);
  const auto exception_rva = read_le32(out_image.effective_image, data_directory_offset + 3u * 8u);
  const auto exception_size = read_le32(out_image.effective_image, data_directory_offset + 3u * 8u + 4u);

  parse_export_directory(out_image.effective_image, export_rva, out_image.exports);

  parse_native_import_libraries(bytes, header_size, entries, out_image.effective_image,
                               out_image.image_base, out_image.imports);
  if (out_image.imports.empty()) {
    parse_pe_import_directory(out_image.effective_image, out_image.image_base, import_rva,
                              out_image.imports);
  }

  parse_pe_tls_directory(out_image.effective_image, tls_rva, out_image.tls);
  parse_relocation_directory(out_image.effective_image, relocation_rva, relocation_size,
                             out_image.relocations);
  parse_function_metadata_directory(out_image.effective_image, out_image.image_base, exception_rva,
                                    exception_size, out_image.function_metadata);

  return true;
}

std::array<std::byte, 20> compute_effective_image_hash(const XexImage& image) {
  return crypto::sha1(image.effective_image);
}

std::string format_effective_image_hash(const std::array<std::byte, 20>& hash) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(hash.size() * 2u);
  for (const auto byte : hash) {
    const auto value = std::to_integer<unsigned char>(byte);
    result.push_back(kHexDigits[(value >> 4u) & 0xFu]);
    result.push_back(kHexDigits[value & 0xFu]);
  }
  return result;
}

XexEffectiveIdentity compute_effective_identity(const XexImage& base_image, const XexImage* patched_image) {
  const XexImage& effective = patched_image ? *patched_image : base_image;
  XexEffectiveIdentity identity{};
  identity.title_id = effective.title_id;
  identity.media_id = effective.media_id;
  identity.base_version = base_image.execution_info.version;
  identity.effective_version = effective.execution_info.version;
  identity.effective_image_hash = compute_effective_image_hash(effective);
  identity.base_image_hash = patched_image ? compute_effective_image_hash(base_image)
                                           : identity.effective_image_hash;
  identity.title_update_applied = patched_image != nullptr;
  return identity;
}

}  // namespace xenon::xbox
