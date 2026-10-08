#pragma once

// Private XEX Loader V2 implementation header shared by the files under
// src/xbox/xex/. Constants, byte helpers and the optional-header table are
// defined here; the parsing stages are declared here and defined in the file
// noted above each declaration. Not part of the public xenon/xbox API.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/xbox/xex_loader.hpp"

namespace xenon::xbox::detail {

constexpr std::uint32_t kXex1Magic = 0x58455831u;  // 'XEX1'

constexpr std::uint32_t kXex2Magic = 0x58455832u;  // 'XEX2'

// XEX_HEADER_* keys this loader understands (see docs/xbox/XEX_LOADER_V2.md for
// the full enumeration; unknown keys are skipped, not rejected).
constexpr std::uint32_t kHeaderResourceInfo = 0x000002FFu;

constexpr std::uint32_t kHeaderFileFormatInfo = 0x000003FFu;

constexpr std::uint32_t kHeaderDeltaPatchDescriptor = 0x000005FFu;

constexpr std::uint32_t kHeaderEntryPoint = 0x00010100u;

constexpr std::uint32_t kHeaderImageBaseAddress = 0x00010201u;

constexpr std::uint32_t kHeaderImportLibraries = 0x000103FFu;

constexpr std::uint32_t kHeaderOriginalPeName = 0x000183FFu;

constexpr std::uint32_t kHeaderTlsInfo = 0x00020104u;

constexpr std::uint32_t kHeaderExecutionInfo = 0x00040006u;

constexpr std::size_t kOptionalHeaderEntrySize = 8u;

constexpr std::size_t kMaxHeaderBytes = 16u * 1024u * 1024u;

// No retail Xbox 360 title image is remotely close to this; it exists only
// to bound allocation/arithmetic for a corrupt or hostile image_size field
// (XEX_COMPRESSION_DELTA's target image size is otherwise attacker-
// controlled 32-bit data with no other natural bound).
constexpr std::uint64_t kMaxReasonableImageBytes = 512ull * 1024ull * 1024ull;

constexpr std::size_t kSecurityInfoFixedSize = 0x184u;  // XEX2, up to page_descriptor_count.

// XEX1's security_info has a genuinely different byte layout from XEX2 (not
// merely a different magic over the same struct): no header_digest/
// export_table/import_table_count fields, a RootImportAddress in their
// place, and every field from load_address onward reordered - see
// parse_security_info() below. Verified against the real xex1::SecurityInfo
// layout (independent reimplementation; no source copied - see
// docs/xbox/XEX_LOADER_V2.md "Research rule").
constexpr std::size_t kXex1SecurityInfoFixedSize = 0x168u;  // Up to page_descriptor_count.

constexpr std::size_t kPageDescriptorSize = 24u;         // 4-byte value + 20-byte digest.

constexpr std::size_t kExecutionInfoSize = 0x18u;

constexpr std::size_t kTlsInfoSize = 0x10u;

constexpr std::size_t kDeltaPatchDescriptorFixedSize = 0x4Cu;

inline std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 4u > bytes.size()) return 0u;
  return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) << 24u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1u])) << 16u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2u])) << 8u) |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3u]));
}

inline std::uint16_t read_be16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 2u > bytes.size()) return 0u;
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) << 8u) |
                                    static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1u])));
}

inline std::uint16_t read_le16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 2u > bytes.size()) return 0u;
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) |
                                    (static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1u])) << 8u));
}

inline std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 4u > bytes.size()) return 0u;
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1u])) << 8u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2u])) << 16u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3u])) << 24u);
}

inline std::string read_string(std::span<const std::byte> source, std::size_t start) {
  if (start >= source.size()) return {};
  std::string result;
  for (std::size_t i = start; i < source.size(); ++i) {
    const auto ch = std::to_integer<unsigned char>(source[i]);
    if (ch == 0u) break;
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

inline bool range_valid(std::size_t offset, std::size_t length, std::size_t size) {
  return offset <= size && length <= size - offset;
}

inline std::uint32_t align_down(std::uint32_t value, std::uint32_t alignment) {
  if (alignment == 0u) return value;
  return value - (value % alignment);
}

inline std::uint64_t align_up(std::uint64_t value, std::uint32_t alignment) {
  if (alignment == 0u) return value;
  const auto remainder = value % alignment;
  return remainder == 0u ? value : value + (alignment - remainder);
}

inline std::uint32_t xex_page_size_for(std::uint32_t guest_address) {
  if (guest_address >= memory::kXex64KBase && guest_address < memory::kXex64KEnd) {
    return memory::kLargePageSize;
  }
  return memory::kBasePageSize;
}

inline void read_array(std::span<const std::byte> bytes, std::size_t offset, std::span<std::byte> out) {
  if (!range_valid(offset, out.size(), bytes.size())) return;
  std::memcpy(out.data(), bytes.data() + offset, out.size());
}

struct OptionalHeaderEntry {
  std::uint32_t key{};
  std::uint32_t raw{};
};

inline bool opt_header_is_inline(std::uint32_t key) {
  const auto size_class = key & 0xFFu;
  return size_class == 0x00u || size_class == 0x01u;
}

inline const OptionalHeaderEntry* find_opt_header(const std::vector<OptionalHeaderEntry>& entries,
                                          std::uint32_t key) {
  for (const auto& entry : entries) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

// xex2_opt_delta_patch_descriptor is 0x4C bytes of fixed fields, immediately
// followed (still inside the same optional-header structure) by one
// embedded xex2_delta_patch record: a 12-byte (old_addr, new_addr,
// uncompressed_len, compressed_len) header plus `compressed_len` bytes of
// LZX-compressed payload (or no payload at all for the compressed_len 0/1
// sentinel record kinds - see xex_lzx::apply_delta_patch_records()). Layout
// verified against the real xex2_opt_delta_patch_descriptor structure
// (independent reimplementation; no source copied - see docs/xbox/XEX_LOADER_V2.md
// "Research rule").
constexpr std::size_t kDeltaPatchRecordHeaderOffset = 0x4Cu;

constexpr std::size_t kDeltaPatchRecordHeaderSize = 12u;

struct FileFormatInfo {
  bool present{false};
  XexEncryptionType encryption{XexEncryptionType::None};
  XexCompressionType compression{XexCompressionType::None};
  std::uint32_t window_size{0x20000u};  // 128 KiB default if unspecified.
  std::size_t info_offset{0};
  std::size_t info_size{0};
};

// --- format/optional_headers.cpp ---
bool enumerate_optional_headers(std::span<const std::byte> bytes, std::uint32_t header_size,
                                std::uint32_t optional_header_count,
                                std::vector<OptionalHeaderEntry>& out, std::string* error);
void parse_execution_info(std::span<const std::byte> bytes, std::uint32_t header_size,
                          const std::vector<OptionalHeaderEntry>& entries, XexImage& out_image);
void parse_original_pe_name(std::span<const std::byte> bytes, std::uint32_t header_size,
                            const std::vector<OptionalHeaderEntry>& entries,
                            std::string& out_name);
void parse_native_tls(std::span<const std::byte> bytes, std::uint32_t header_size,
                      const std::vector<OptionalHeaderEntry>& entries,
                      std::optional<XexTls>& out_tls);
void parse_delta_patch_descriptor(std::span<const std::byte> bytes, std::uint32_t header_size,
                                  const std::vector<OptionalHeaderEntry>& entries,
                                  XexDeltaPatchDescriptor& out);
void parse_file_format_info(std::span<const std::byte> bytes, std::uint32_t header_size,
                            const std::vector<OptionalHeaderEntry>& entries,
                            FileFormatInfo& out);

// --- security/security_info.cpp ---
bool parse_security_info(std::span<const std::byte> bytes, XexFormat format, std::uint32_t header_size,
                         std::uint32_t security_offset, XexSecurityInfo& out,
                         std::string* error);
memory::Protect protect_for_address(const XexSecurityInfo& security, std::uint32_t address);
bool verify_first_page_digest(const XexSecurityInfo& security, std::span<const std::byte> effective_image,
                             std::string& error);

// --- compression/body_decompression.cpp ---
bool walk_compressed_block_chain(std::span<const std::byte> body,
                                 const std::function<bool(std::span<const std::byte>)>& on_payload,
                                 std::string* error);
bool decompress_body(std::span<const std::byte> bytes, std::uint32_t header_size,
                     const XexSecurityInfo& security, const FileFormatInfo& format,
                     std::span<const std::byte> reference_image, const XexDeltaPatchDescriptor& delta_patch,
                     std::vector<std::byte>& out_effective, std::string* error);

// --- title_updates/image_delta.cpp ---
bool apply_image_delta(std::span<const std::byte> body, std::uint32_t window_bits,
                       std::span<const std::byte> reference_image, const XexDeltaPatchDescriptor& delta_patch,
                       std::uint32_t target_size, std::vector<std::byte>& out_effective, std::string* error);
bool reconstruct_patch_header_bytes(std::span<const std::byte> base_header_bytes,
                                    std::span<const std::byte> patch_header_bytes,
                                    std::span<const std::byte> patch_file_bytes,
                                    const XexDeltaPatchDescriptor& descriptor,
                                    std::uint32_t window_bits, std::vector<std::byte>& out_header,
                                    std::string* error);

// --- image/pe_directories.cpp ---
void parse_export_directory(std::span<const std::byte> bytes, std::uint32_t rva,
                           std::vector<XexExport>& exports);
void parse_pe_import_directory(std::span<const std::byte> bytes, std::uint32_t image_base,
                              std::uint32_t rva,
                              std::vector<XexImport>& imports);
void parse_pe_tls_directory(std::span<const std::byte> bytes, std::uint32_t rva,
                           std::optional<XexTls>& tls);
void parse_relocation_directory(std::span<const std::byte> bytes, std::uint32_t rva, std::uint32_t size,
                               std::vector<XexRelocation>& relocations);
void parse_function_metadata_directory(std::span<const std::byte> bytes,
                                      std::uint32_t image_base, std::uint32_t rva, std::uint32_t size,
                                      std::vector<XexFunctionMetadata>& output);
bool try_parse_pe_sections(std::span<const std::byte> effective_image, std::uint32_t image_base,
                          std::vector<XexSection>& out_sections, std::uint32_t& out_entry_point_rva,
                          std::string* error);

// --- imports/native_imports.cpp ---
void parse_native_import_libraries(std::span<const std::byte> header_bytes, std::uint32_t header_size,
                                  const std::vector<OptionalHeaderEntry>& entries,
                                  std::span<const std::byte> effective_image, std::uint32_t image_base,
                                  std::vector<XexImport>& imports);

}  // namespace xenon::xbox::detail
