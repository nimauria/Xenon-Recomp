#include "xenon/xbox/xex_loader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox {
namespace {

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

// ---------------------------------------------------------------------------
// Byte helpers.
// ---------------------------------------------------------------------------

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 4u > bytes.size()) return 0u;
  return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) << 24u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1u])) << 16u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2u])) << 8u) |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3u]));
}

std::uint16_t read_be16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 2u > bytes.size()) return 0u;
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) << 8u) |
                                    static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1u])));
}

std::uint16_t read_le16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 2u > bytes.size()) return 0u;
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) |
                                    (static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1u])) << 8u));
}

std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 4u > bytes.size()) return 0u;
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1u])) << 8u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2u])) << 16u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3u])) << 24u);
}

std::string read_string(std::span<const std::byte> source, std::size_t start) {
  if (start >= source.size()) return {};
  std::string result;
  for (std::size_t i = start; i < source.size(); ++i) {
    const auto ch = std::to_integer<unsigned char>(source[i]);
    if (ch == 0u) break;
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

bool range_valid(std::size_t offset, std::size_t length, std::size_t size) {
  return offset <= size && length <= size - offset;
}

std::uint32_t align_down(std::uint32_t value, std::uint32_t alignment) {
  if (alignment == 0u) return value;
  return value - (value % alignment);
}

std::uint64_t align_up(std::uint64_t value, std::uint32_t alignment) {
  if (alignment == 0u) return value;
  const auto remainder = value % alignment;
  return remainder == 0u ? value : value + (alignment - remainder);
}

std::uint32_t xex_page_size_for(std::uint32_t guest_address) {
  if (guest_address >= memory::kXex64KBase && guest_address < memory::kXex64KEnd) {
    return memory::kLargePageSize;
  }
  return memory::kBasePageSize;
}

void read_array(std::span<const std::byte> bytes, std::size_t offset, std::span<std::byte> out) {
  if (!range_valid(offset, out.size(), bytes.size())) return;
  std::memcpy(out.data(), bytes.data() + offset, out.size());
}

// ---------------------------------------------------------------------------
// Optional header table (xex2_opt_header[]): key/value pairs where the low
// byte of `key` selects whether `value` is used inline (classes 0x00, 0x01)
// or is a byte offset into the header where the real structure lives (every
// other class, including 0xFF for variable-size structures that self-
// describe their size in their first field).
// ---------------------------------------------------------------------------

struct OptionalHeaderEntry {
  std::uint32_t key{};
  std::uint32_t raw{};
};

bool opt_header_is_inline(std::uint32_t key) {
  const auto size_class = key & 0xFFu;
  return size_class == 0x00u || size_class == 0x01u;
}

bool enumerate_optional_headers(std::span<const std::byte> bytes, std::uint32_t header_size,
                                std::uint32_t optional_header_count,
                                std::vector<OptionalHeaderEntry>& out, std::string* error) {
  const auto max_entries = (static_cast<std::size_t>(header_size) - 0x18u) / kOptionalHeaderEntrySize;
  const auto entry_count = std::min<std::size_t>(optional_header_count, max_entries);
  out.reserve(entry_count);
  for (std::size_t index = 0u; index < entry_count; ++index) {
    const auto entry_offset = 0x18u + index * kOptionalHeaderEntrySize;
    if (entry_offset + kOptionalHeaderEntrySize > bytes.size()) {
      if (error) *error = "XEX optional header table is truncated.";
      return false;
    }
    out.push_back({read_be32(bytes, entry_offset), read_be32(bytes, entry_offset + 4u)});
  }
  return true;
}

const OptionalHeaderEntry* find_opt_header(const std::vector<OptionalHeaderEntry>& entries,
                                          std::uint32_t key) {
  for (const auto& entry : entries) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Security info + page descriptors.
// ---------------------------------------------------------------------------

bool parse_security_info(std::span<const std::byte> bytes, XexFormat format, std::uint32_t header_size,
                         std::uint32_t security_offset, XexSecurityInfo& out,
                         std::string* error) {
  const auto fixed_size = format == XexFormat::Xex1 ? kXex1SecurityInfoFixedSize : kSecurityInfoFixedSize;
  if (security_offset == 0u || !range_valid(security_offset, fixed_size, header_size)) {
    if (error) *error = "XEX security info offset/size is invalid.";
    return false;
  }
  const std::size_t base = security_offset;
  out.header_size = read_be32(bytes, base + 0x0u);
  out.image_size = read_be32(bytes, base + 0x4u);

  if (format == XexFormat::Xex1) {
    // xex1::SecurityInfo (0x168 bytes): a genuinely different field order/
    // set from XEX2, not merely the same struct under a different magic -
    // see kXex1SecurityInfoFixedSize's comment.
    read_array(bytes, base + 0x8u, out.rsa_signature);            // Signature[0x100]
    read_array(bytes, base + 0x108u, out.import_table_digest);    // ImportDigest[0x14]
    read_array(bytes, base + 0x11Cu, out.section_digest);         // ImageHash[0x14]
    out.load_address = read_be32(bytes, base + 0x130u);
    read_array(bytes, base + 0x134u, out.encrypted_image_key);    // ImageKey[0x10]
    read_array(bytes, base + 0x144u, out.xgd2_media_id);          // MediaID[0x10]
    out.region = read_be32(bytes, base + 0x154u);
    out.image_flags = read_be32(bytes, base + 0x158u);
    out.xex1_root_import_address = read_be32(bytes, base + 0x15Cu);
    out.allowed_media_types = read_be32(bytes, base + 0x160u);
    // XEX1 has no header_digest/export_table/import_table_count fields of
    // its own; leave them at their zero-initialized defaults.
  } else {
    read_array(bytes, base + 0x8u, out.rsa_signature);
    out.image_flags = read_be32(bytes, base + 0x10Cu);
    out.load_address = read_be32(bytes, base + 0x110u);
    read_array(bytes, base + 0x114u, out.section_digest);
    out.import_table_count = read_be32(bytes, base + 0x128u);
    read_array(bytes, base + 0x12Cu, out.import_table_digest);
    read_array(bytes, base + 0x140u, out.xgd2_media_id);
    read_array(bytes, base + 0x150u, out.encrypted_image_key);
    out.export_table = read_be32(bytes, base + 0x160u);
    read_array(bytes, base + 0x164u, out.header_digest);
    out.region = read_be32(bytes, base + 0x178u);
    out.allowed_media_types = read_be32(bytes, base + 0x17Cu);
  }
  const auto page_descriptor_count_offset = base + (format == XexFormat::Xex1 ? 0x164u : 0x180u);
  const auto page_descriptor_count = read_be32(bytes, page_descriptor_count_offset);

  // Bound page-descriptor reads by both the security struct's own declared
  // size and the outer header region, so a corrupt count can never walk past
  // either.
  const auto security_region_limit = std::min<std::size_t>(
      out.header_size != 0u ? static_cast<std::size_t>(security_offset) + out.header_size
                            : static_cast<std::size_t>(header_size),
      static_cast<std::size_t>(header_size));
  const auto descriptors_offset = base + fixed_size;
  const auto available_bytes =
      security_region_limit > descriptors_offset ? security_region_limit - descriptors_offset : 0u;
  const auto max_descriptors = available_bytes / kPageDescriptorSize;
  const auto descriptor_count = std::min<std::size_t>(page_descriptor_count, max_descriptors);

  out.page_descriptors.reserve(descriptor_count);
  for (std::size_t i = 0u; i < descriptor_count; ++i) {
    const auto entry_offset = descriptors_offset + i * kPageDescriptorSize;
    if (!range_valid(entry_offset, kPageDescriptorSize, bytes.size())) break;
    const auto value = read_be32(bytes, entry_offset);
    XexPageDescriptor descriptor{};
    const auto type_value = value & 0xFu;
    descriptor.type = (type_value >= 1u && type_value <= 3u) ? static_cast<XexPageType>(type_value)
                                                              : XexPageType::Unknown;
    descriptor.page_count = value >> 4u;
    read_array(bytes, entry_offset + 4u, descriptor.data_digest);
    out.page_descriptors.push_back(descriptor);
  }
  return true;
}

memory::Protect protect_for_page_type(XexPageType type) {
  switch (type) {
    case XexPageType::Code:
      return memory::Protect::Read | memory::Protect::Execute;
    case XexPageType::Data:
      return memory::Protect::Read | memory::Protect::Write;
    case XexPageType::ReadOnlyData:
      return memory::Protect::Read;
    default:
      return memory::Protect::None;
  }
}

// Resolves the page-descriptor protection covering `address` (a guest
// address), walking the descriptor run list starting at security.load_address.
memory::Protect protect_for_address(const XexSecurityInfo& security, std::uint32_t address) {
  if (address < security.load_address) return memory::Protect::None;
  const auto page_size = (security.image_flags & 0x10000000u) != 0u ? 4096u : 65536u;
  std::uint64_t cursor = security.load_address;
  for (const auto& descriptor : security.page_descriptors) {
    const std::uint64_t run_bytes = static_cast<std::uint64_t>(descriptor.page_count) * page_size;
    if (address >= cursor && address < cursor + run_bytes) {
      return protect_for_page_type(descriptor.type);
    }
    cursor += run_bytes;
  }
  return memory::Protect::None;
}

// ---------------------------------------------------------------------------
// Execution info / original PE name / native TLS / delta patch descriptor.
// ---------------------------------------------------------------------------

void parse_execution_info(std::span<const std::byte> bytes, std::uint32_t header_size,
                          const std::vector<OptionalHeaderEntry>& entries, XexImage& out_image) {
  const auto* entry = find_opt_header(entries, kHeaderExecutionInfo);
  if (!entry || opt_header_is_inline(entry->key)) return;
  const auto offset = static_cast<std::size_t>(entry->raw);
  if (!range_valid(offset, kExecutionInfoSize, header_size)) return;

  XexExecutionInfo info{};
  info.media_id = read_be32(bytes, offset + 0x00u);
  info.version.value = read_be32(bytes, offset + 0x04u);
  info.base_version.value = read_be32(bytes, offset + 0x08u);
  info.title_id = read_be32(bytes, offset + 0x0Cu);
  info.platform = std::to_integer<std::uint8_t>(bytes[offset + 0x10u]);
  info.executable_table = std::to_integer<std::uint8_t>(bytes[offset + 0x11u]);
  info.disc_number = std::to_integer<std::uint8_t>(bytes[offset + 0x12u]);
  info.disc_count = std::to_integer<std::uint8_t>(bytes[offset + 0x13u]);
  info.savegame_id = read_be32(bytes, offset + 0x14u);

  out_image.execution_info = info;
  out_image.media_id = info.media_id;
  out_image.title_id = info.title_id;
  out_image.execution_id = info.savegame_id;
}

void parse_original_pe_name(std::span<const std::byte> bytes, std::uint32_t header_size,
                            const std::vector<OptionalHeaderEntry>& entries,
                            std::string& out_name) {
  const auto* entry = find_opt_header(entries, kHeaderOriginalPeName);
  if (!entry || opt_header_is_inline(entry->key)) return;
  const auto offset = static_cast<std::size_t>(entry->raw);
  if (!range_valid(offset, 4u, header_size)) return;
  const auto record_size = static_cast<std::size_t>(read_be32(bytes, offset));
  if (record_size < 4u || !range_valid(offset, record_size, header_size)) return;
  out_name = read_string(bytes.subspan(offset + 4u, record_size - 4u), 0u);
}

void parse_native_tls(std::span<const std::byte> bytes, std::uint32_t header_size,
                      const std::vector<OptionalHeaderEntry>& entries,
                      std::optional<XexTls>& out_tls) {
  const auto* entry = find_opt_header(entries, kHeaderTlsInfo);
  if (!entry || opt_header_is_inline(entry->key)) return;
  const auto offset = static_cast<std::size_t>(entry->raw);
  if (!range_valid(offset, kTlsInfoSize, header_size)) return;
  XexTls tls{};
  tls.slot = read_be32(bytes, offset + 0x00u);
  tls.raw_data_start = read_be32(bytes, offset + 0x04u);
  tls.data_size = read_be32(bytes, offset + 0x08u);
  tls.raw_data_size = read_be32(bytes, offset + 0x0Cu);
  out_tls = tls;
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

void parse_delta_patch_descriptor(std::span<const std::byte> bytes, std::uint32_t header_size,
                                  const std::vector<OptionalHeaderEntry>& entries,
                                  XexDeltaPatchDescriptor& out) {
  const auto* entry = find_opt_header(entries, kHeaderDeltaPatchDescriptor);
  if (!entry || opt_header_is_inline(entry->key)) return;
  const auto offset = static_cast<std::size_t>(entry->raw);
  if (!range_valid(offset, kDeltaPatchDescriptorFixedSize, header_size)) return;
  out.present = true;
  out.target_version.value = read_be32(bytes, offset + 0x4u);
  out.source_version.value = read_be32(bytes, offset + 0x8u);
  read_array(bytes, offset + 0xCu, out.base_signature_digest);
  read_array(bytes, offset + 0x20u, out.image_key_source);
  out.size_of_target_headers = read_be32(bytes, offset + 0x30u);
  out.delta_headers_source_offset = read_be32(bytes, offset + 0x34u);
  out.delta_headers_source_size = read_be32(bytes, offset + 0x38u);
  out.delta_headers_target_offset = read_be32(bytes, offset + 0x3Cu);
  out.delta_image_source_offset = read_be32(bytes, offset + 0x40u);
  out.delta_image_source_size = read_be32(bytes, offset + 0x44u);
  out.delta_image_target_offset = read_be32(bytes, offset + 0x48u);

  const auto record_header_offset = offset + kDeltaPatchRecordHeaderOffset;
  if (!range_valid(record_header_offset, kDeltaPatchRecordHeaderSize, header_size)) return;
  out.header_patch_old_addr = read_be32(bytes, record_header_offset + 0x0u);
  out.header_patch_new_addr = read_be32(bytes, record_header_offset + 0x4u);
  out.header_patch_uncompressed_len = read_be16(bytes, record_header_offset + 0x8u);
  out.header_patch_compressed_len = read_be16(bytes, record_header_offset + 0xAu);
  out.header_patch_data_offset = record_header_offset + kDeltaPatchRecordHeaderSize;
}

// ---------------------------------------------------------------------------
// File-format info: encryption + compression, and the body decrypt/
// decompress pipeline. The XEX body always begins at file offset
// `header_size` and reconstructs to exactly `security.image_size` bytes of
// PE image data.
// ---------------------------------------------------------------------------

struct FileFormatInfo {
  bool present{false};
  XexEncryptionType encryption{XexEncryptionType::None};
  XexCompressionType compression{XexCompressionType::None};
  std::uint32_t window_size{0x20000u};  // 128 KiB default if unspecified.
  std::size_t info_offset{0};
  std::size_t info_size{0};
};

void parse_file_format_info(std::span<const std::byte> bytes, std::uint32_t header_size,
                            const std::vector<OptionalHeaderEntry>& entries,
                            FileFormatInfo& out) {
  const auto* entry = find_opt_header(entries, kHeaderFileFormatInfo);
  if (!entry || opt_header_is_inline(entry->key)) return;
  const auto offset = static_cast<std::size_t>(entry->raw);
  if (!range_valid(offset, 8u, header_size)) return;
  out.present = true;
  out.info_offset = offset;
  out.info_size = read_be32(bytes, offset + 0x0u);
  out.encryption = static_cast<XexEncryptionType>(read_be16(bytes, offset + 0x4u));
  out.compression = static_cast<XexCompressionType>(read_be16(bytes, offset + 0x6u));
  if (out.compression == XexCompressionType::Normal || out.compression == XexCompressionType::Delta) {
    if (range_valid(offset + 0x8u, 4u, header_size)) {
      out.window_size = read_be32(bytes, offset + 0x8u);
    }
  }
}

// XEX_COMPRESSION_BASIC: a sequence of (data_size, zero_size) pairs. The
// block table itself lives in the XEX *header* (inside the
// xex2_opt_file_format_info optional header, `header_bytes` /
// `basic_info_offset`), immediately after the info_size/encryption/
// compression fields; the bytes it describes are read from the decrypted
// *body* (`body`): `data_size` bytes copied verbatim, followed by
// `zero_size` zero bytes, repeating until the block table is exhausted.
bool decompress_basic(std::span<const std::byte> header_bytes, std::size_t basic_info_offset,
                      std::size_t basic_info_size, std::span<const std::byte> body,
                      std::vector<std::byte>& out, std::string* error) {
  if (basic_info_size < 8u) {
    if (error) *error = "XEX basic-compression info block is too small.";
    return false;
  }
  const auto block_count = (basic_info_size - 8u) / 8u;
  std::size_t src = 0u;
  out.clear();
  for (std::size_t i = 0u; i < block_count; ++i) {
    const auto entry_offset = basic_info_offset + 8u + i * 8u;
    if (!range_valid(entry_offset, 8u, header_bytes.size())) {
      if (error) *error = "XEX basic-compression block table is truncated.";
      return false;
    }
    const auto data_size = read_be32(header_bytes, entry_offset);
    const auto zero_size = read_be32(header_bytes, entry_offset + 4u);
    if (!range_valid(src, data_size, body.size())) {
      if (error) *error = "XEX basic-compression block overruns the file body.";
      return false;
    }
    const auto span = body.subspan(src, data_size);
    out.insert(out.end(), span.begin(), span.end());
    out.insert(out.end(), static_cast<std::size_t>(zero_size), std::byte{0});
    src += data_size;
  }
  return true;
}

// The xex2_compressed_block_info outer chain (4-byte big-endian block_size +
// 20-byte SHA1 digest, block_size counting the header itself, terminated by
// a bare 4-byte zero block_size) is shared, byte-for-byte, by both
// XEX_COMPRESSION_NORMAL and XEX_COMPRESSION_DELTA bodies - only the
// *payload interpretation* differs between them (see degather_compressed_
// blocks() vs apply_image_delta() below). This walks the chain once,
// validating each block's hash over its payload, and invokes `on_payload`
// with each validated payload in order; `on_payload` returns false (and
// must set *error itself) to abort the walk.
bool walk_compressed_block_chain(std::span<const std::byte> body,
                                 const std::function<bool(std::span<const std::byte>)>& on_payload,
                                 std::string* error) {
  std::size_t cursor = 0u;
  while (true) {
    // The chain terminator is a bare 4-byte zero block_size field - it does
    // not need a full 24-byte header (hash + payload) to follow, so only
    // require those 4 bytes until we know there is an actual block here.
    if (!range_valid(cursor, 4u, body.size())) {
      if (error) *error = "XEX compressed-block chain is truncated.";
      return false;
    }
    const auto block_size = read_be32(body, cursor);
    if (block_size == 0u) break;  // Terminator block.
    if (block_size < 24u || !range_valid(cursor, block_size, body.size())) {
      if (error) *error = "XEX compressed-block size is invalid.";
      return false;
    }
    std::array<std::byte, 20> stored_hash{};
    read_array(body, cursor + 4u, stored_hash);

    const auto payload = body.subspan(cursor + 24u, block_size - 24u);
    const auto computed_hash = crypto::sha1(payload);
    if (!std::equal(computed_hash.begin(), computed_hash.end(), stored_hash.begin())) {
      if (error) *error = "XEX compressed-block SHA1 digest mismatch (corrupt or truncated data).";
      return false;
    }

    if (!on_payload(payload)) return false;

    cursor += block_size;
  }
  return true;
}

// XEX_COMPRESSION_NORMAL: each block's payload is a sequence of
// 2-byte-length-prefixed chunks (0 = end of block) whose concatenated
// payload is one continuous raw LZX bitstream.
bool degather_compressed_blocks(std::span<const std::byte> body, std::vector<std::byte>& out_bitstream,
                                std::string* error) {
  out_bitstream.clear();
  return walk_compressed_block_chain(
      body,
      [&](std::span<const std::byte> payload) {
        std::size_t chunk_cursor = 0u;
        while (true) {
          if (!range_valid(chunk_cursor, 2u, payload.size())) {
            if (error) *error = "XEX compressed-block chunk table is truncated.";
            return false;
          }
          const auto chunk_size = read_be16(payload, chunk_cursor);
          chunk_cursor += 2u;
          if (chunk_size == 0u) break;
          if (!range_valid(chunk_cursor, chunk_size, payload.size())) {
            if (error) *error = "XEX compressed-block chunk overruns its block.";
            return false;
          }
          const auto chunk = payload.subspan(chunk_cursor, chunk_size);
          out_bitstream.insert(out_bitstream.end(), chunk.begin(), chunk.end());
          chunk_cursor += chunk_size;
        }
        return true;
      },
      error);
}

// Validates then applies `dest[target_offset, target_offset+size) =
// source[source_offset, source_offset+size)`, snapshotting the source range
// first so this is safe even when `source` and `dest` alias the same
// buffer. Shared by the XEXP header-region delta (which splices within a
// single working buffer seeded from the base header) and the XEXP
// image-region delta (which splices from a separate, read-only base image
// into a fresh working buffer).
bool splice_delta_region(std::span<const std::byte> source, std::uint32_t source_offset, std::uint32_t size,
                         std::span<std::byte> dest, std::uint32_t target_offset, const char* what,
                         std::string* error) {
  if (source_offset > source.size() || size > source.size() - source_offset) {
    if (error) *error = std::string("XEXP ") + what + " source range is outside its source data.";
    return false;
  }
  if (target_offset > dest.size() || size > dest.size() - target_offset) {
    if (error) *error = std::string("XEXP ") + what + " target range is outside its target buffer.";
    return false;
  }
  std::vector<std::byte> snapshot(source.begin() + source_offset, source.begin() + source_offset + size);
  std::copy(snapshot.begin(), snapshot.end(), dest.begin() + target_offset);
  return true;
}

// XEX_COMPRESSION_DELTA (XEXP image/data delta): reconstructs the target
// effective image from `reference_image` (the base image's own
// effective_image - read-only, never mutated; the result is always built in
// a separate `working` buffer) plus the title update's
// XEX_HEADER_DELTA_PATCH_DESCRIPTOR (`delta_image_source_offset/target_
// offset/source_size`, for one optional whole-region splice - the same
// "copy this range from the base, then patch on top" shape the header
// delta uses) and the xex2_compressed_block_info chain in `body`. Each
// block's payload here is a xex2_delta_patch record chain (fill/copy/
// LZXDELTA-compressed records - see xex_lzx::apply_delta_patch_records()),
// applied directly onto `working`, NOT the 2-byte-length-prefixed chunk
// table degather_compressed_blocks() reassembles into a single continuous
// bitstream for XEX_COMPRESSION_NORMAL - this is a structurally different
// payload framing that happens to share the same outer block/hash
// container. Independent reimplementation against the real Xbox 360 XEXP
// image-patch algorithm (research: xenia-project/xenia's
// XexModule::ApplyPatch()/lzxdelta_apply_patch(), read for understanding
// only, not copied - see "Research rule").
bool apply_image_delta(std::span<const std::byte> body, std::uint32_t window_bits,
                       std::span<const std::byte> reference_image, const XexDeltaPatchDescriptor& delta_patch,
                       std::uint32_t target_size, std::vector<std::byte>& out_effective, std::string* error) {
  if (reference_image.empty()) {
    if (error) *error = "XEX_COMPRESSION_DELTA requires a base reference image.";
    return false;
  }
  if (target_size == 0u || target_size > kMaxReasonableImageBytes) {
    if (error) *error = "XEX_COMPRESSION_DELTA target image size is invalid.";
    return false;
  }

  const auto working_size = std::max<std::uint64_t>(reference_image.size(), target_size);
  if (working_size > kMaxReasonableImageBytes) {
    if (error) *error = "XEX_COMPRESSION_DELTA working image size is invalid.";
    return false;
  }
  std::vector<std::byte> working(static_cast<std::size_t>(working_size), std::byte{0});
  std::copy(reference_image.begin(), reference_image.end(), working.begin());

  if (delta_patch.delta_image_source_size != 0u) {
    if (!splice_delta_region(reference_image, delta_patch.delta_image_source_offset,
                             delta_patch.delta_image_source_size, working, delta_patch.delta_image_target_offset,
                             "image-delta", error)) {
      return false;
    }
  }

  if (working_size > target_size) {
    std::fill(working.begin() + static_cast<std::size_t>(target_size), working.end(), std::byte{0});
  }

  std::string chain_error;
  const bool ok = walk_compressed_block_chain(
      body,
      [&](std::span<const std::byte> payload) {
        const auto result = lzx::apply_delta_patch_records(payload, window_bits, working);
        if (!result.ok) {
          chain_error = "XEXP image-delta record chain failed: " + result.error;
          return false;
        }
        return true;
      },
      &chain_error);
  if (!ok) {
    if (error) *error = chain_error;
    return false;
  }

  working.resize(static_cast<std::size_t>(target_size));
  out_effective = std::move(working);
  return true;
}

// xex2_page_descriptor's on-disk 4-byte value field (type in the low 4
// bits, page_count in the remaining 28) - the inverse of the type_value/
// page_count split parse_security_info() performs, needed to reconstruct
// the descriptor's exact on-disk bytes for section_digest verification
// below without re-reading the original file span.
std::uint32_t page_descriptor_type_code(XexPageType type) {
  switch (type) {
    case XexPageType::Code:
      return 1u;
    case XexPageType::Data:
      return 2u;
    case XexPageType::ReadOnlyData:
      return 3u;
    default:
      return 0u;
  }
}

// security_info.section_digest (XEX2 "ImageHash") / the equivalent XEX1
// field is SHA1(first_page_bytes, first_page_descriptor_entry) - a real,
// well-defined integrity digest over the *decompressed* effective image
// that only the correct key+decompression can reproduce, independent of
// whatever "does this happen to parse as a PE" a wrong key's garbage output
// might accidentally satisfy. Returns true (no-op) when there is no page
// descriptor to check against, or the effective image is smaller than the
// first page-descriptor run declares (a genuinely malformed/undersized
// image, which the PE-parsing stage will reject on its own merits).
bool verify_first_page_digest(const XexSecurityInfo& security, std::span<const std::byte> effective_image,
                             std::string& error) {
  if (security.page_descriptors.empty()) return true;
  const auto& first = security.page_descriptors.front();
  const std::uint64_t page_size = (security.image_flags & 0x10000000u) != 0u ? 4096u : 65536u;
  const auto first_run_bytes = first.page_count * page_size;
  if (first_run_bytes == 0u || first_run_bytes > effective_image.size()) return true;

  std::array<std::byte, 24u> descriptor_bytes{};
  const auto value = (first.page_count << 4u) | page_descriptor_type_code(first.type);
  descriptor_bytes[0] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  descriptor_bytes[1] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  descriptor_bytes[2] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  descriptor_bytes[3] = static_cast<std::byte>(value & 0xFFu);
  std::copy(first.data_digest.begin(), first.data_digest.end(), descriptor_bytes.begin() + 4);

  std::vector<std::byte> hashed(effective_image.begin(), effective_image.begin() + first_run_bytes);
  hashed.insert(hashed.end(), descriptor_bytes.begin(), descriptor_bytes.end());
  const auto digest = crypto::sha1(hashed);
  if (!std::equal(digest.begin(), digest.end(), security.section_digest.begin())) {
    error = "XEX first-page digest does not match security_info.section_digest "
            "(wrong key, corrupt data, or incorrect decompression).";
    return false;
  }
  return true;
}

// Applies encryption then compression to reconstruct the effective image,
// then verifies it against security_info's own first-page digest
// (verify_first_page_digest() above) - real cryptographic proof that both
// the correct key and the correct decompression were used, for every
// compression type uniformly (not just Normal/Delta's incidental per-block
// SHA1 checks). A wrong key is rejected here rather than merely happening
// to fail PE parsing later, closing the gap where XEX_COMPRESSION_NONE/
// BASIC bodies previously had no integrity check at all. On failure with
// the retail key, retries once with the devkit key before giving up.
bool decompress_body(std::span<const std::byte> bytes, std::uint32_t header_size,
                     const XexSecurityInfo& security, const FileFormatInfo& format,
                     std::span<const std::byte> reference_image, const XexDeltaPatchDescriptor& delta_patch,
                     std::vector<std::byte>& out_effective, std::string* error) {
  if (header_size > bytes.size()) {
    if (error) *error = "XEX header_size exceeds the file size.";
    return false;
  }
  const auto encrypted_body = bytes.subspan(header_size);
  const std::uint32_t target_size =
      security.image_size != 0u ? security.image_size : static_cast<std::uint32_t>(encrypted_body.size());

  const auto attempt = [&](const crypto::AesKey& wrapping_key, std::string& attempt_error) -> bool {
    std::vector<std::byte> plain;
    if (format.encryption == XexEncryptionType::None) {
      plain.assign(encrypted_body.begin(), encrypted_body.end());
    } else {
      if (encrypted_body.size() % 16u != 0u) {
        attempt_error = "XEX encrypted body is not AES-block aligned.";
        return false;
      }
      crypto::AesKey encrypted_image_key{};
      for (std::size_t i = 0; i < 16; ++i) encrypted_image_key[i] = security.encrypted_image_key[i];
      const auto image_key = crypto::unwrap_image_key(wrapping_key, encrypted_image_key);
      plain.resize(encrypted_body.size());
      if (!crypto::aes128_cbc_decrypt(image_key, encrypted_body, plain)) {
        attempt_error = "XEX AES-CBC decryption failed.";
        return false;
      }
    }

    std::vector<std::byte> decoded_effective;
    switch (format.compression) {
      case XexCompressionType::None: {
        if (plain.size() < target_size) {
          attempt_error = "XEX uncompressed body is smaller than the declared image size.";
          return false;
        }
        decoded_effective.assign(plain.begin(), plain.begin() + target_size);
        break;
      }
      case XexCompressionType::Basic: {
        if (!decompress_basic(bytes, format.info_offset, format.info_size, plain, decoded_effective,
                              &attempt_error)) {
          return false;
        }
        break;
      }
      case XexCompressionType::Normal: {
        std::uint32_t window_bits = 0;
        for (auto size = format.window_size; size > 1u; size >>= 1u) ++window_bits;
        if (!lzx::valid_window_bits(window_bits)) {
          attempt_error = "XEX LZX window size is invalid.";
          return false;
        }
        std::vector<std::byte> bitstream;
        if (!degather_compressed_blocks(plain, bitstream, &attempt_error)) return false;
        decoded_effective.assign(static_cast<std::size_t>(target_size), std::byte{0});
        const auto decoded = lzx::decode(bitstream, window_bits, reference_image, decoded_effective);
        if (!decoded.ok) {
          attempt_error = "XEX LZX decompression failed: " + decoded.error;
          return false;
        }
        break;
      }
      case XexCompressionType::Delta: {
        // XEXP image/data delta: real xex2_delta_patch record-chain
        // framing (apply_image_delta() above), not XEX_COMPRESSION_NORMAL's
        // chunk-table-over-a-continuous-bitstream framing - the two share
        // only the outer xex2_compressed_block_info container.
        std::uint32_t window_bits = 0;
        for (auto size = format.window_size; size > 1u; size >>= 1u) ++window_bits;
        if (!lzx::valid_window_bits(window_bits)) {
          attempt_error = "XEX LZX window size is invalid.";
          return false;
        }
        if (!apply_image_delta(plain, window_bits, reference_image, delta_patch, target_size,
                               decoded_effective, &attempt_error)) {
          return false;
        }
        break;
      }
      default:
        attempt_error = "XEX compression type is not supported.";
        return false;
    }

    if (!verify_first_page_digest(security, decoded_effective, attempt_error)) {
      return false;
    }
    out_effective = std::move(decoded_effective);
    return true;
  };

  std::string retail_error;
  if (attempt(crypto::retail_key(), retail_error)) return true;
  if (format.encryption == XexEncryptionType::None) {
    if (error) *error = retail_error;
    return false;
  }
  std::string devkit_error;
  if (attempt(crypto::devkit_key(), devkit_error)) return true;
  if (error) {
    *error = "XEX body decode failed with both retail and devkit keys (retail: " + retail_error +
             "; devkit: " + devkit_error + ").";
  }
  return false;
}

// ---------------------------------------------------------------------------
// PE parsing over the effective (decrypted+decompressed) image. The
// effective image *is* the PE file, starting at offset 0 - no MZ scanning.
// ---------------------------------------------------------------------------

memory::Protect pe_section_protect(std::uint32_t characteristics) {
  memory::Protect protect = memory::Protect::None;
  if ((characteristics & 0x40000000u) != 0u) protect |= memory::Protect::Read;
  if ((characteristics & 0x80000000u) != 0u) protect |= memory::Protect::Write;
  if ((characteristics & 0x20000000u) != 0u) protect |= memory::Protect::Execute;
  if (protect == memory::Protect::None) protect = memory::Protect::Read;
  return protect;
}

std::optional<std::size_t> rva_to_image_offset(std::span<const std::byte> image,
                                               std::uint32_t rva,
                                               std::size_t length = 1u) {
  const auto offset = static_cast<std::size_t>(rva);
  if (!range_valid(offset, length, image.size())) return std::nullopt;
  return offset;
}

void parse_export_directory(std::span<const std::byte> bytes, std::uint32_t rva,
                           std::vector<XexExport>& exports) {
  if (rva == 0u) return;
  const auto export_offset = rva_to_image_offset(bytes, rva, 0x28u);
  if (!export_offset) return;
  const auto export_dir = bytes.subspan(*export_offset);
  const auto number_of_functions = read_le32(export_dir, 0x14u);
  const auto number_of_names = read_le32(export_dir, 0x18u);
  const auto address_of_functions = read_le32(export_dir, 0x1Cu);
  const auto address_of_names = read_le32(export_dir, 0x20u);
  const auto address_of_name_ordinals = read_le32(export_dir, 0x24u);
  const auto ordinal_base = read_le32(export_dir, 0x10u);
  if (number_of_functions == 0u || address_of_functions == 0u) return;

  const auto functions_offset =
      rva_to_image_offset(bytes, address_of_functions, static_cast<std::size_t>(number_of_functions) * 4u);
  const auto names_offset =
      rva_to_image_offset(bytes, address_of_names, static_cast<std::size_t>(number_of_names) * 4u);
  const auto ordinals_offset =
      rva_to_image_offset(bytes, address_of_name_ordinals, static_cast<std::size_t>(number_of_names) * 2u);
  if (!functions_offset || (!names_offset && number_of_names != 0u) ||
      (!ordinals_offset && number_of_names != 0u)) {
    return;
  }

  for (std::uint32_t i = 0u; i < number_of_functions && i < number_of_names; ++i) {
    const auto name_rva = read_le32(bytes.subspan(*names_offset), i * 4u);
    const auto name_offset = rva_to_image_offset(bytes, name_rva);
    if (!name_offset) continue;
    const auto ordinal_index = read_le16(bytes.subspan(*ordinals_offset), i * 2u);
    if (ordinal_index >= number_of_functions) continue;
    const auto function_rva = read_le32(bytes.subspan(*functions_offset), ordinal_index * 4u);
    if (function_rva == 0u) continue;
    XexExport export_entry{};
    export_entry.name = read_string(bytes.subspan(*name_offset), 0u);
    export_entry.ordinal = static_cast<std::uint16_t>(ordinal_base + ordinal_index);
    export_entry.address = function_rva;
    exports.push_back(export_entry);
  }
}

void parse_pe_import_directory(std::span<const std::byte> bytes, std::uint32_t image_base,
                              std::uint32_t rva,
                              std::vector<XexImport>& imports) {
  if (rva == 0u) return;
  const auto import_desc_offset = rva_to_image_offset(bytes, rva, 20u);
  if (!import_desc_offset) return;

  for (std::size_t descriptor_index = 0u;; ++descriptor_index) {
    const auto descriptor_offset = *import_desc_offset + descriptor_index * 20u;
    if (!range_valid(descriptor_offset, 20u, bytes.size())) break;
    const auto descriptor_span = bytes.subspan(descriptor_offset, 20u);
    const auto original_first_thunk = read_le32(descriptor_span, 0u);
    const auto name_rva = read_le32(descriptor_span, 0x0Cu);
    const auto first_thunk = read_le32(descriptor_span, 0x10u);
    if (original_first_thunk == 0u && name_rva == 0u && first_thunk == 0u) break;
    if (name_rva == 0u) continue;
    const auto name_offset = rva_to_image_offset(bytes, name_rva);
    if (!name_offset) continue;
    const auto module_name = read_string(bytes.subspan(*name_offset), 0u);
    const auto thunk_rva = original_first_thunk != 0u ? original_first_thunk : first_thunk;
    const auto thunk_offset = rva_to_image_offset(bytes, thunk_rva, 4u);
    if (!thunk_offset) continue;
    for (std::size_t i = 0u;; ++i) {
      if ((i + 1u) * 4u > bytes.size() - *thunk_offset) break;
      const auto entry = read_le32(bytes.subspan(*thunk_offset), i * 4u);
      if (entry == 0u) break;
      XexImport import{};
      import.module = module_name;
      // IMAGE_IMPORT_DESCRIPTOR::FirstThunk is an RVA. Preserve the exact IAT
      // entry address, not merely the base of the whole table, so indirect
      // calls through PE imports can be resolved entry-by-entry.
      import.guest_thunk = static_cast<memory::GuestAddress>(
          image_base + first_thunk + static_cast<std::uint32_t>(i * 4u));
      import.attributes = 0u;
      import.kind = XexImportKind::PeFunction;
      if ((entry & 0x80000000u) != 0u) {
        import.ordinal = static_cast<std::uint16_t>(entry & 0xFFFFu);
      } else {
        const auto by_name_rva = static_cast<std::uint32_t>(entry & 0x7FFFFFFFu);
        const auto by_name_offset = rva_to_image_offset(bytes, by_name_rva, 2u);
        if (by_name_offset) {
          const auto hint = read_le16(bytes.subspan(*by_name_offset), 0u);
          import.ordinal = hint;
          import.symbol = read_string(bytes.subspan(*by_name_offset + 2u), 0u);
        }
      }
      imports.push_back(import);
    }
  }
}

void parse_pe_tls_directory(std::span<const std::byte> bytes, std::uint32_t rva,
                           std::optional<XexTls>& tls) {
  if (rva == 0u || tls.has_value()) return;  // Native TLS header, if present, wins.
  const auto tls_offset = rva_to_image_offset(bytes, rva, 0x18u);
  if (!tls_offset) return;
  XexTls result{};
  result.raw_data_start = read_le32(bytes.subspan(*tls_offset), 0x00u);
  result.raw_data_size = read_le32(bytes.subspan(*tls_offset), 0x04u);
  result.index_address = read_le32(bytes.subspan(*tls_offset), 0x08u);
  result.callback_address = read_le32(bytes.subspan(*tls_offset), 0x0Cu);
  result.slot = read_le32(bytes.subspan(*tls_offset), 0x10u);
  tls = result;
}

void parse_relocation_directory(std::span<const std::byte> bytes, std::uint32_t rva, std::uint32_t size,
                               std::vector<XexRelocation>& relocations) {
  if (rva == 0u || size == 0u) return;
  const auto dir_offset = rva_to_image_offset(bytes, rva, size);
  if (!dir_offset) return;

  auto cursor = *dir_offset;
  const auto limit = cursor + static_cast<std::size_t>(size);
  while (cursor + 8u <= limit) {
    const auto block_rva = read_le32(bytes, cursor + 0u);
    const auto block_size = read_le32(bytes, cursor + 4u);
    if (block_size < 8u || block_size > limit - cursor) break;
    if (((block_size - 8u) & 1u) != 0u) break;

    const auto block_data_end = cursor + static_cast<std::size_t>(block_size);
    auto entry_offset = cursor + 8u;
    XexRelocation relocation{};
    relocation.virtual_address = block_rva;
    relocation.size = block_size;
    while (entry_offset + 2u <= block_data_end) {
      const auto entry = read_le16(bytes, entry_offset);
      const auto type = static_cast<std::uint16_t>((entry >> 12u) & 0xFu);
      const auto offset = static_cast<std::uint16_t>(entry & 0x0FFFu);
      if (type != 0u || offset != 0u) {
        relocation.type = type;
        relocation.entries.push_back((static_cast<std::uint32_t>(type) << 16u) |
                                    static_cast<std::uint32_t>(offset));
      }
      entry_offset += 2u;
    }
    if (!relocation.entries.empty()) relocations.push_back(relocation);
    cursor += block_size;
  }
}

void parse_function_metadata_directory(std::span<const std::byte> bytes,
                                      std::uint32_t image_base, std::uint32_t rva, std::uint32_t size,
                                      std::vector<XexFunctionMetadata>& output) {
  if (rva == 0u || size < 12u) return;
  const auto file_offset = rva_to_image_offset(bytes, rva, size);
  if (!file_offset) return;
  const auto available = static_cast<std::size_t>(size);
  if (available % 12u != 0u) return;
  for (std::size_t offset = 0u; offset < available; offset += 12u) {
    const auto begin = read_le32(bytes, *file_offset + offset);
    const auto end = read_le32(bytes, *file_offset + offset + 4u);
    const auto unwind = read_le32(bytes, *file_offset + offset + 8u);
    if (begin == 0u && end == 0u && unwind == 0u) continue;
    if (end <= begin) continue;
    output.push_back({static_cast<memory::GuestAddress>(image_base + begin),
                      static_cast<memory::GuestAddress>(image_base + end), unwind, true});
  }
}

// XEX-native import-libraries optional header (XEX_HEADER_IMPORT_LIBRARIES):
// authoritative for title EXEs, which generally carry no meaningful PE
// import directory of their own. Each import_table[] entry names a guest
// address where the effective image contains a 32-bit loader placeholder:
//   bits  0..15 = ordinal
//   bits 16..23 = import attributes
//   bits 24..31 = record type (0 = address/variable, 1 = function thunk)
// A normal function import therefore appears twice: a type-0 import-address
// slot and a type-1 callable thunk. A type-0 record with no matching type-1
// record is a genuine imported variable. Keeping these roles distinct avoids
// duplicate import diagnostics and lets the runtime bind true variable slots
// without treating the type-0 half of every function import as callable.
void parse_native_import_libraries(std::span<const std::byte> header_bytes, std::uint32_t header_size,
                                  const std::vector<OptionalHeaderEntry>& entries,
                                  std::span<const std::byte> effective_image, std::uint32_t image_base,
                                  std::vector<XexImport>& imports) {
  const auto* entry = find_opt_header(entries, kHeaderImportLibraries);
  if (!entry || opt_header_is_inline(entry->key)) return;
  const auto offset = static_cast<std::size_t>(entry->raw);
  if (!range_valid(offset, 12u, header_size)) return;

  const auto table_size = read_be32(header_bytes, offset + 0x0u);
  const auto string_table_size = read_be32(header_bytes, offset + 0x4u);
  const auto string_table_count = read_be32(header_bytes, offset + 0x8u);
  if (!range_valid(offset, table_size, header_size)) return;
  const auto string_table_offset = offset + 0xCu;
  if (!range_valid(string_table_offset, string_table_size, header_size)) return;

  // The string table is a sequence of `string_table_count` NUL-terminated,
  // 4-byte-aligned library-name strings.
  std::vector<std::string> library_names;
  library_names.reserve(string_table_count);
  {
    std::size_t cursor = string_table_offset;
    const auto end = string_table_offset + string_table_size;
    for (std::uint32_t i = 0u; i < string_table_count && cursor < end; ++i) {
      const auto name = read_string(header_bytes.subspan(cursor), 0u);
      library_names.push_back(name);
      auto advance = name.size() + 1u;
      advance = (advance + 3u) & ~std::size_t{3u};
      cursor += advance;
    }
  }

  std::size_t library_offset = string_table_offset + string_table_size;
  const auto libraries_end = offset + table_size;
  while (library_offset + 0x28u <= libraries_end && library_offset + 0x28u <= header_size) {
    const auto library_size = read_be32(header_bytes, library_offset + 0x0u);
    if (library_size < 0x28u) break;
    const auto name_index = read_be16(header_bytes, library_offset + 0x24u);
    const auto import_count = read_be16(header_bytes, library_offset + 0x26u);
    const std::string library_name =
        name_index < library_names.size() ? library_names[name_index] : std::string("unknown");
    const auto library_import_begin = imports.size();

    for (std::uint16_t i = 0u; i < import_count; ++i) {
      const auto table_entry_offset = library_offset + 0x28u + static_cast<std::size_t>(i) * 4u;
      if (!range_valid(table_entry_offset, 4u, header_size)) break;
      const auto record_address = read_be32(header_bytes, table_entry_offset);
      if (record_address == 0u) continue;

      // A record address outside the effective image is malformed (corrupt
      // data or a hostile/truncated image). Do not fabricate an ordinal-0
      // import from an unreadable placeholder.
      if (record_address < image_base ||
          static_cast<std::size_t>(record_address - image_base) + 4u > effective_image.size()) {
        continue;
      }

      XexImport import{};
      import.module = library_name;
      import.guest_thunk = record_address;
      const auto placeholder = read_be32(effective_image, record_address - image_base);
      import.ordinal = static_cast<std::uint16_t>(placeholder & 0xFFFFu);
      import.attributes = placeholder >> 16u;
      const auto record_type = static_cast<std::uint8_t>((placeholder >> 24u) & 0xFFu);
      switch (record_type) {
        case 0u:
          // This is provisionally a variable. If the same library/ordinal has
          // a type-1 record below, post-processing upgrades it to the address
          // slot of that function import.
          import.kind = XexImportKind::Variable;
          break;
        case 1u:
          import.kind = XexImportKind::FunctionThunk;
          break;
        default:
          import.kind = XexImportKind::Unknown;
          break;
      }
      imports.push_back(std::move(import));
    }

    // Pair each type-0 record with a type-1 record of the same ordinal in the
    // same library. Only unpaired type-0 records remain true variable imports.
    for (std::size_t i = library_import_begin; i < imports.size(); ++i) {
      auto& candidate = imports[i];
      if (!candidate.is_variable()) continue;
      const auto matching_thunk = std::find_if(
          imports.begin() + static_cast<std::ptrdiff_t>(library_import_begin), imports.end(),
          [&](const XexImport& other) {
            return other.kind == XexImportKind::FunctionThunk &&
                   other.ordinal == candidate.ordinal && other.module == candidate.module;
          });
      if (matching_thunk != imports.end()) {
        candidate.kind = XexImportKind::FunctionAddress;
      }
    }

    library_offset += library_size;
  }
}

bool try_parse_pe_sections(std::span<const std::byte> effective_image, std::uint32_t image_base,
                          std::vector<XexSection>& out_sections, std::uint32_t& out_entry_point_rva,
                          std::string* error) {
  if (effective_image.size() < 2u || effective_image[0] != std::byte{'M'} ||
      effective_image[1] != std::byte{'Z'}) {
    if (error) *error = "Effective XEX image does not start with a valid MZ/PE header.";
    return false;
  }

  const auto pe_header_offset = read_le32(effective_image, 0x3Cu);
  const auto coff_offset = static_cast<std::size_t>(pe_header_offset) + 4u;
  if (!range_valid(pe_header_offset, 4u, effective_image.size()) ||
      read_le32(effective_image, pe_header_offset) != 0x4550u) {
    if (error) *error = "Effective XEX image is missing a valid PE signature.";
    return false;
  }

  const auto num_sections = read_le16(effective_image, coff_offset + 0x02u);
  const auto optional_header_size = read_le16(effective_image, coff_offset + 0x10u);
  const auto optional_header_offset = coff_offset + 20u;
  const auto section_table_offset = optional_header_offset + optional_header_size;
  if (!range_valid(optional_header_offset, 2u, effective_image.size()) || optional_header_size == 0u) {
    if (error) *error = "Effective XEX image PE optional header is invalid.";
    return false;
  }

  const auto optional_magic = read_le16(effective_image, optional_header_offset);
  if (optional_magic == 0x10bu) {
    out_entry_point_rva = read_le32(effective_image, optional_header_offset + 0x10u);
  } else if (optional_magic == 0x20bu) {
    out_entry_point_rva = read_le32(effective_image, optional_header_offset + 0x10u);
  } else {
    if (error) *error = "Effective XEX image PE optional header magic is unrecognized.";
    return false;
  }

  const auto section_count = std::min<std::uint16_t>(num_sections, static_cast<std::uint16_t>(96u));
  for (std::uint16_t index = 0u; index < section_count; ++index) {
    const auto section_offset = section_table_offset + static_cast<std::size_t>(index) * 40u;
    if (!range_valid(section_offset, 40u, effective_image.size())) break;

    XexSection section{};
    for (std::size_t char_index = 0u; char_index < 8u; ++char_index) {
      const auto ch = std::to_integer<unsigned char>(effective_image[section_offset + char_index]);
      if (ch == 0u) break;
      section.name.push_back(static_cast<char>(ch));
    }
    if (section.name.empty()) section.name = ".section" + std::to_string(index);

    section.virtual_size = read_le32(effective_image, section_offset + 0x08u);
    section.virtual_address =
        static_cast<memory::GuestAddress>(image_base + read_le32(effective_image, section_offset + 0x0Cu));
    section.raw_size = read_le32(effective_image, section_offset + 0x10u);
    section.raw_pointer = read_le32(effective_image, section_offset + 0x14u);
    section.characteristics = read_le32(effective_image, section_offset + 0x24u);
    section.protect = pe_section_protect(section.characteristics);
    section.executable = (section.characteristics & 0x20000000u) != 0u;
    section.writable = (section.characteristics & 0x80000000u) != 0u;
    section.readable = (section.characteristics & 0x40000000u) != 0u;
    if (section.virtual_size == 0u && section.raw_size != 0u) section.virtual_size = section.raw_size;
    const auto section_rva = read_le32(effective_image, section_offset + 0x0Cu);
    const auto section_length = std::max(section.virtual_size, section.raw_size);
    if (section_length != 0u && section_rva < effective_image.size()) {
      const auto copied_length =
          std::min<std::size_t>(section_length, effective_image.size() - section_rva);
      const auto span = effective_image.subspan(section_rva, copied_length);
      section.bytes.assign(span.begin(), span.end());
    }
    out_sections.push_back(section);
  }

  if (out_sections.empty()) {
    if (error) *error = "Effective XEX image has no PE sections.";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// XEXP header-region delta reconstruction.
// ---------------------------------------------------------------------------

// Splices XexDeltaPatchDescriptor::delta_headers_source_offset/size from
// `base_header_bytes` into a target_size-sized buffer at
// delta_headers_target_offset, then applies the descriptor's embedded
// xex2_delta_patch record on top - reproducing the real Xbox 360 XEXP
// header-patch algorithm (validated against xenia-project/xenia's
// XexModule::ApplyPatch(), read for research purposes only; this is an
// independent reimplementation against Xenon's own XexDeltaPatchDescriptor,
// not copied source - see docs/xbox/XEX_LOADER_V2.md "Research rule").
//
// When the descriptor is absent or delta_headers_source_size == 0 (no header
// *content* actually changed - the common case for code/data-only updates),
// `patch_header_bytes` is returned unchanged, exactly matching the prior,
// documented "known limitation" behaviour for that case.
bool reconstruct_patch_header_bytes(std::span<const std::byte> base_header_bytes,
                                    std::span<const std::byte> patch_header_bytes,
                                    std::span<const std::byte> patch_file_bytes,
                                    const XexDeltaPatchDescriptor& descriptor,
                                    std::uint32_t window_bits, std::vector<std::byte>& out_header,
                                    std::string* error) {
  if (!descriptor.present || descriptor.delta_headers_source_size == 0u) {
    out_header.assign(patch_header_bytes.begin(), patch_header_bytes.end());
    return true;
  }

  if (descriptor.delta_headers_source_offset > base_header_bytes.size()) {
    if (error) *error = "XEXP header-delta source range is outside the base image's header.";
    return false;
  }
  const auto header_size_available = base_header_bytes.size() - descriptor.delta_headers_source_offset;
  if (descriptor.delta_headers_source_size > header_size_available) {
    if (error) *error = "XEXP header-delta source range is too large for the base header.";
    return false;
  }

  // 64-bit intermediate: delta_headers_target_offset is attacker-controlled
  // and unbounded at this point, so target_offset + source_size must not be
  // allowed to silently wrap a 32-bit sum before it is range-checked.
  const std::uint64_t target_size_wide =
      descriptor.size_of_target_headers != 0u
          ? static_cast<std::uint64_t>(descriptor.size_of_target_headers)
          : static_cast<std::uint64_t>(descriptor.delta_headers_target_offset) +
                static_cast<std::uint64_t>(descriptor.delta_headers_source_size);
  if (target_size_wide == 0u || target_size_wide > kMaxHeaderBytes) {
    if (error) *error = "XEXP header-delta target header size is invalid.";
    return false;
  }
  const auto target_size = static_cast<std::uint32_t>(target_size_wide);
  if (descriptor.delta_headers_target_offset > target_size) {
    if (error) *error = "XEXP header-delta target range is outside the target header.";
    return false;
  }
  const auto delta_target_size = target_size - descriptor.delta_headers_target_offset;
  if (descriptor.delta_headers_source_size > delta_target_size) {
    if (error) *error = "XEXP header-delta source range does not fit at its target offset.";
    return false;
  }

  // Working buffer: a copy of the base header, grown (never shrunk yet) to
  // cover both it and the target header, so the splice below always has
  // valid source bytes to read even when the target header is larger.
  const auto working_size = std::max<std::size_t>(base_header_bytes.size(), target_size);
  std::vector<std::byte> working(working_size, std::byte{0});
  std::copy(base_header_bytes.begin(), base_header_bytes.end(), working.begin());

  // The bounds above are already tighter than what splice_delta_region()
  // itself re-checks (source vs base_header_bytes.size(), target vs
  // working.size() which may exceed target_size while growing) - both are
  // kept since splice_delta_region() is the shared primitive with the
  // image-region delta below, which relies on its own checks alone.
  if (!splice_delta_region(base_header_bytes, descriptor.delta_headers_source_offset,
                           descriptor.delta_headers_source_size, working, descriptor.delta_headers_target_offset,
                           "header-delta", error)) {
    return false;
  }

  if (working_size > target_size) {
    std::fill(working.begin() + target_size, working.end(), std::byte{0});
  }

  const bool has_record = descriptor.header_patch_compressed_len != 0u ||
                          descriptor.header_patch_uncompressed_len != 0u ||
                          descriptor.header_patch_old_addr != 0u || descriptor.header_patch_new_addr != 0u;
  if (has_record) {
    if (!range_valid(descriptor.header_patch_data_offset, descriptor.header_patch_compressed_len,
                     patch_file_bytes.size())) {
      if (error) *error = "XEXP header-delta LZX record payload is truncated.";
      return false;
    }
    std::array<std::byte, kDeltaPatchRecordHeaderSize> record_header{};
    const auto put_be32 = [&](std::size_t off, std::uint32_t v) {
      record_header[off + 0] = static_cast<std::byte>((v >> 24) & 0xFFu);
      record_header[off + 1] = static_cast<std::byte>((v >> 16) & 0xFFu);
      record_header[off + 2] = static_cast<std::byte>((v >> 8) & 0xFFu);
      record_header[off + 3] = static_cast<std::byte>(v & 0xFFu);
    };
    const auto put_be16 = [&](std::size_t off, std::uint16_t v) {
      record_header[off + 0] = static_cast<std::byte>((v >> 8) & 0xFFu);
      record_header[off + 1] = static_cast<std::byte>(v & 0xFFu);
    };
    put_be32(0u, descriptor.header_patch_old_addr);
    put_be32(4u, descriptor.header_patch_new_addr);
    put_be16(8u, descriptor.header_patch_uncompressed_len);
    put_be16(10u, descriptor.header_patch_compressed_len);

    std::vector<std::byte> record_bytes(record_header.begin(), record_header.end());
    const auto payload = patch_file_bytes.subspan(descriptor.header_patch_data_offset,
                                                   descriptor.header_patch_compressed_len);
    record_bytes.insert(record_bytes.end(), payload.begin(), payload.end());

    const auto patched = lzx::apply_delta_patch_records(record_bytes, window_bits, working);
    if (!patched.ok) {
      if (error) *error = "XEXP header-delta LZX patch failed: " + patched.error;
      return false;
    }
  }

  working.resize(target_size);
  out_header = std::move(working);
  return true;
}

}  // namespace

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

bool map_xex_image(memory::AddressSpace& memory, const XexImage& image, LoadedXex& out_loaded,
                   memory::GuestAddress preferred_base, std::string* error) {
  out_loaded = {};
  if (!memory.initialize()) {
    if (error) *error = "Memory V2 cannot initialize for XEX mapping.";
    return false;
  }

  memory::GuestAddress image_base = preferred_base;
  if (image.image_base != 0u) {
    image_base = static_cast<memory::GuestAddress>(image.image_base);
  }
  out_loaded.image_base = image_base;
  out_loaded.image = image;

  if (image.sections.empty()) {
    if (error) *error = "No PE or section data were extracted from this XEX image.";
    return false;
  }

  struct SectionMappingPlan {
    const XexSection* section{};
    memory::GuestAddress section_begin{};
    std::uint64_t section_end{};  // Exclusive, kept wide for overflow checks.
    memory::GuestAddress mapped_begin{};
    std::uint64_t mapped_end{};   // Exclusive.
    std::uint32_t page_size{};
  };

  struct AllocationPlan {
    memory::GuestAddress begin{};
    std::uint64_t end{};  // Exclusive.
    std::uint32_t page_size{};
  };

  std::vector<SectionMappingPlan> section_plans;
  section_plans.reserve(image.sections.size());

  const auto set_section_error = [&](std::string_view stage,
                                     const XexSection& section,
                                     memory::GuestAddress mapped_begin,
                                     std::uint64_t mapped_end,
                                     std::uint32_t page_size) {
    if (!error) return;
    std::ostringstream stream;
    stream << "Failed to " << stage << " PE section '"
           << (section.name.empty() ? "<unnamed>" : section.name)
           << "' in Xenon Memory V2: address=0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(section.virtual_address)
           << " virtual_size=0x" << section.virtual_size
           << " raw_size=0x" << section.raw_size
           << " mapped_base=0x" << mapped_begin
           << " mapped_size=0x" << (mapped_end >= mapped_begin ? mapped_end - mapped_begin : 0u)
           << " page_size=0x" << page_size << '.';
    *error = stream.str();
  };

  // Plan every section before reserving anything. Xbox PE sections are byte
  // ranges, while Memory V2 reserves the native Xbox allocation granularity
  // (64 KiB in the 0x8... XEX aperture, 4 KiB in the 0x9... aperture). Real
  // titles may therefore have distinct PE sections that share one or more
  // allocation pages. Those pages must be reserved once for the image, not
  // once per section.
  for (const auto& section : image.sections) {
    const std::uint64_t section_size = std::max<std::uint64_t>(
        std::max(section.virtual_size, section.raw_size), section.bytes.size());
    if (section_size == 0u) continue;

    const auto section_begin =
        static_cast<memory::GuestAddress>(section.virtual_address);
    const auto page_size = xex_page_size_for(section_begin);
    const std::uint64_t section_end =
        static_cast<std::uint64_t>(section_begin) + section_size;
    if (section_end > (std::uint64_t{1} << 32u) || section_end <= section_begin) {
      set_section_error("size", section, section_begin, section_end, page_size);
      return false;
    }

    const auto last_byte = static_cast<memory::GuestAddress>(section_end - 1u);
    if (xex_page_size_for(last_byte) != page_size) {
      set_section_error("map across incompatible XEX page regions for", section,
                        section_begin, section_end, page_size);
      return false;
    }

    const auto mapped_begin = align_down(section_begin, page_size);
    const auto mapped_end = align_up(section_end, page_size);
    if (mapped_end <= mapped_begin || mapped_end > (std::uint64_t{1} << 32u)) {
      set_section_error("align", section, mapped_begin, mapped_end, page_size);
      return false;
    }

    section_plans.push_back(
        {&section, section_begin, section_end, mapped_begin, mapped_end, page_size});
  }

  if (section_plans.empty()) {
    if (error) *error = "XEX image contains no non-empty PE sections to map.";
    return false;
  }

  std::vector<AllocationPlan> allocations;
  allocations.reserve(section_plans.size());
  std::vector<std::size_t> order(section_plans.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    const auto& a = section_plans[lhs];
    const auto& b = section_plans[rhs];
    if (a.mapped_begin != b.mapped_begin) return a.mapped_begin < b.mapped_begin;
    if (a.mapped_end != b.mapped_end) return a.mapped_end < b.mapped_end;
    return a.page_size < b.page_size;
  });

  for (const auto index : order) {
    const auto& plan = section_plans[index];
    if (!allocations.empty() && allocations.back().page_size == plan.page_size &&
        static_cast<std::uint64_t>(plan.mapped_begin) <= allocations.back().end) {
      allocations.back().end = std::max(allocations.back().end, plan.mapped_end);
      continue;
    }
    allocations.push_back({plan.mapped_begin, plan.mapped_end, plan.page_size});
  }

  std::vector<memory::GuestAddress> mapped_allocation_bases;
  mapped_allocation_bases.reserve(allocations.size());

  const auto rollback_mappings = [&] {
    for (auto it = mapped_allocation_bases.rbegin();
         it != mapped_allocation_bases.rend(); ++it) {
      (void)memory.release(*it);
    }
    mapped_allocation_bases.clear();
    out_loaded.mapped_sections.clear();
    out_loaded.executable_ranges.clear();
    out_loaded.loaded = false;
  };

  const auto describe_allocation_failure = [&](std::string_view stage,
                                               const AllocationPlan& allocation) {
    if (!error) return;
    std::ostringstream stream;
    stream << "Failed to " << stage
           << " merged PE allocation in Xenon Memory V2: mapped_base=0x"
           << std::hex << std::uppercase << allocation.begin
           << " mapped_size=0x" << (allocation.end - allocation.begin)
           << " page_size=0x" << allocation.page_size << " sections=";
    bool first = true;
    for (const auto& plan : section_plans) {
      if (plan.mapped_end <= allocation.begin ||
          static_cast<std::uint64_t>(plan.mapped_begin) >= allocation.end) {
        continue;
      }
      if (!first) stream << ',';
      stream << '\'' << (plan.section->name.empty() ? "<unnamed>" : plan.section->name)
             << '\'';
      first = false;
    }
    stream << '.';
    *error = stream.str();
  };

  // Reserve/commit each merged allocation exactly once. Keep it writable only
  // while the PE bytes are copied; final guest protections are applied below.
  for (const auto& allocation : allocations) {
    const auto size64 = allocation.end - allocation.begin;
    if (size64 == 0u || size64 > std::numeric_limits<std::uint32_t>::max()) {
      describe_allocation_failure("size", allocation);
      rollback_mappings();
      return false;
    }
    const auto size = static_cast<std::uint32_t>(size64);
    if (!memory.reserve_fixed(allocation.begin, size, memory::kReadWrite)) {
      describe_allocation_failure("reserve", allocation);
      rollback_mappings();
      return false;
    }
    mapped_allocation_bases.push_back(allocation.begin);
    // PE virtual tails are required to start zeroed. Committing the merged
    // image ranges with zero initialization also makes bytes between sections
    // deterministic instead of exposing recycled physical RAM contents.
    if (!memory.commit_fixed(allocation.begin, size, memory::kReadWrite, true)) {
      describe_allocation_failure("commit", allocation);
      rollback_mappings();
      return false;
    }
  }

  // Copy each section at its real guest virtual address. Sharing allocation
  // pages is now harmless because the backing range was created once above.
  for (const auto& plan : section_plans) {
    const auto& section = *plan.section;
    if (!section.bytes.empty()) {
      try {
        memory.write_bytes(plan.section_begin, section.bytes);
      } catch (...) {
        set_section_error("write", section, plan.mapped_begin, plan.mapped_end,
                          plan.page_size);
        rollback_mappings();
        return false;
      }
    }
    out_loaded.mapped_sections.push_back(section);
  }

  // Native import records are intentionally left byte-for-byte as loaded here.
  // Type-0 records paired with a type-1 function thunk are metadata/address
  // slots, not function pointers that should be rewritten to the thunk. True
  // unpaired type-0 variable imports are bound later by XenonSession once the
  // active system-module variable registry exists.

  // Resolve the final protection for one Memory V2 native XEX page. XEX page
  // descriptors remain authoritative when present. If a title has no
  // descriptor coverage for that native page, PE-section protection is the
  // fallback. Multiple descriptor sub-pages are ORed because Memory V2's
  // 0x8... aperture intentionally exposes 64 KiB protection granularity.
  const auto protection_for_native_page = [&](memory::GuestAddress page_begin,
                                              std::uint32_t native_page_size) {
    memory::Protect descriptor_protect = memory::Protect::None;
    bool descriptor_covered = false;
    if (!image.security.page_descriptors.empty()) {
      const auto descriptor_page_size =
          (image.security.image_flags & 0x10000000u) != 0u
              ? memory::kBasePageSize
              : memory::kLargePageSize;
      const std::uint64_t page_end =
          static_cast<std::uint64_t>(page_begin) + native_page_size;
      for (std::uint64_t address = page_begin; address < page_end;
           address += descriptor_page_size) {
        const auto protect = protect_for_address(
            image.security, static_cast<std::uint32_t>(address));
        if (protect != memory::Protect::None) {
          descriptor_covered = true;
          descriptor_protect |= protect;
        }
      }
    }
    if (descriptor_covered) return descriptor_protect;

    memory::Protect section_protect = memory::Protect::None;
    const std::uint64_t page_end =
        static_cast<std::uint64_t>(page_begin) + native_page_size;
    for (const auto& plan : section_plans) {
      if (plan.section_end <= page_begin ||
          static_cast<std::uint64_t>(plan.section_begin) >= page_end) {
        continue;
      }
      section_protect |= plan.section->protect;
    }
    return section_protect;
  };

  out_loaded.executable_ranges.clear();
  for (const auto& allocation : allocations) {
    for (std::uint64_t page64 = allocation.begin; page64 < allocation.end;
         page64 += allocation.page_size) {
      const auto page = static_cast<memory::GuestAddress>(page64);
      const auto final_protect = protection_for_native_page(page, allocation.page_size);
      if (!memory.protect(page, allocation.page_size, final_protect)) {
        if (error) {
          std::ostringstream stream;
          stream << "Failed to apply final XEX page protection in Memory V2: address=0x"
                 << std::hex << std::uppercase << page
                 << " size=0x" << allocation.page_size
                 << " protect=0x" << static_cast<unsigned>(final_protect) << '.';
          *error = stream.str();
        }
        rollback_mappings();
        return false;
      }

      if (memory::has(final_protect, memory::Protect::Execute)) {
        if (!out_loaded.executable_ranges.empty()) {
          auto& previous = out_loaded.executable_ranges.back();
          if (previous.end == page && previous.page_size == allocation.page_size &&
              previous.protect == final_protect) {
            previous.end = page + allocation.page_size;
            continue;
          }
        }
        out_loaded.executable_ranges.push_back(
            {page, page + allocation.page_size, allocation.page_size, final_protect});
      }
    }
  }

  out_loaded.loaded = true;
  out_loaded.error.clear();
  return true;
}

bool load_xex(memory::AddressSpace& memory, std::span<const std::byte> file_bytes, LoadedXex& out_loaded,
             memory::GuestAddress preferred_base, std::string* error) {
  XexImage image{};
  if (!parse_xex_image(file_bytes, image, error)) {
    out_loaded = {};
    return false;
  }
  return map_xex_image(memory, image, out_loaded, preferred_base, error);
}

bool apply_title_update(const XexImage& base_image, std::span<const std::byte> update_bytes,
                       XexImage& out_image, std::string* error) {
  out_image = {};
  if (base_image.effective_image.empty()) {
    if (error) *error = "No base executable image is available for title-update application.";
    return false;
  }
  if (base_image.header_bytes.empty()) {
    if (error) *error = "Base image has no header bytes available for title-update application.";
    return false;
  }
  if (update_bytes.empty()) {
    if (error) *error = "Title-update payload is empty.";
    return false;
  }
  if (update_bytes.size() < 0x18u) {
    if (error) *error = "Title-update payload is too small to contain a valid header.";
    return false;
  }
  if (detect_xex_format(update_bytes) == XexFormat::Unknown) {
    if (error) *error = "Title-update payload is not a recognized XEX1/XEX2 image.";
    return false;
  }

  // Preliminary pass over the *patch file's own* on-disk header: enough to
  // locate its delta-patch descriptor and file-format-info window size,
  // before deciding whether that on-disk header needs XEXP header-region
  // delta reconstruction against the base image's header
  // (see reconstruct_patch_header_bytes()).
  const auto patch_header_size = read_be32(update_bytes, 8u);
  const auto patch_optional_header_count = read_be32(update_bytes, 0x14u);
  if (patch_header_size < 0x18u || patch_header_size > update_bytes.size() ||
      patch_header_size > kMaxHeaderBytes) {
    if (error) *error = "Title-update header size is invalid.";
    return false;
  }

  std::vector<OptionalHeaderEntry> patch_entries;
  if (!enumerate_optional_headers(update_bytes, patch_header_size, patch_optional_header_count,
                                  patch_entries, error)) {
    return false;
  }

  XexDeltaPatchDescriptor patch_descriptor{};
  parse_delta_patch_descriptor(update_bytes, patch_header_size, patch_entries, patch_descriptor);

  FileFormatInfo patch_format_info{};
  parse_file_format_info(update_bytes, patch_header_size, patch_entries, patch_format_info);
  std::uint32_t header_window_bits = 17u;  // 128 KiB default, matches FileFormatInfo's own default.
  {
    std::uint32_t bits = 0u;
    for (auto size = patch_format_info.window_size; size > 1u; size >>= 1u) ++bits;
    if (lzx::valid_window_bits(bits)) header_window_bits = bits;
  }

  std::vector<std::byte> reconstructed_header;
  if (!reconstruct_patch_header_bytes(base_image.header_bytes, update_bytes.subspan(0u, patch_header_size),
                                      update_bytes, patch_descriptor, header_window_bits,
                                      reconstructed_header, error)) {
    return false;
  }

  // Reassemble a byte buffer where the (possibly reconstructed) header
  // replaces the patch's on-disk header verbatim; the body (compression/
  // encryption/PE parsing) is untouched by header reconstruction and is
  // still located via the patch *file's* own on-disk header_size.
  std::vector<std::byte> full_patch_bytes(reconstructed_header.begin(), reconstructed_header.end());
  const auto patch_body = update_bytes.subspan(patch_header_size);
  full_patch_bytes.insert(full_patch_bytes.end(), patch_body.begin(), patch_body.end());

  // The reconstructed header's own header_size field must reflect where we
  // actually placed the header/body boundary in full_patch_bytes, so
  // parse_xex_image() (which derives that boundary from the field, not from
  // an externally-tracked size) splits them at the same point. This must be
  // enforced unconditionally, not just when the reconstructed size differs
  // from the patch file's own on-disk header size: a header-delta
  // reconstruction that happens to produce a same-sized buffer can still
  // carry a *stale* header_size field value at content offset 8 (copied
  // verbatim from the base header, e.g. when neither the splice nor the LZX
  // record's target range covers that field) - the field must always be
  // re-derived from the buffer's actual size, never trusted as incidental
  // byte content.
  if (reconstructed_header.size() < 0x0Cu) {
    if (error) *error = "XEXP reconstructed header is too small to contain a valid header.";
    return false;
  }
  {
    const auto new_header_size = static_cast<std::uint32_t>(reconstructed_header.size());
    full_patch_bytes[8] = static_cast<std::byte>((new_header_size >> 24) & 0xFFu);
    full_patch_bytes[9] = static_cast<std::byte>((new_header_size >> 16) & 0xFFu);
    full_patch_bytes[10] = static_cast<std::byte>((new_header_size >> 8) & 0xFFu);
    full_patch_bytes[11] = static_cast<std::byte>(new_header_size & 0xFFu);
  }

  // Patch-kind/identity validation is performed against the patch *file's
  // own* on-disk declarations, never against the reconstructed header: real
  // hardware determines is_patch()/is_full_patch()/is_delta_patch() from the
  // patch XEX's own physical module_flags before any header-delta
  // reconstruction runs, and a header-region delta that splices a large
  // (even whole-header) range from the base is expected to also overwrite
  // module_flags/title_id/media_id with the base's own values in the
  // *reconstructed* buffer - that reconstructed state describes the
  // resulting effective title image (correctly no longer "a patch"), not
  // what the patch payload itself claimed to be.
  const auto patch_module_flags = read_be32(update_bytes, 4u);
  const bool patch_is_patch = (patch_module_flags & module_flags::kModulePatch) != 0u;
  const bool patch_is_full_patch = (patch_module_flags & module_flags::kPatchFull) != 0u;
  const bool patch_is_delta_patch = (patch_module_flags & module_flags::kPatchDelta) != 0u;

  if (!patch_is_patch) {
    if (error) *error = "Payload is not a XEX title-update (XEX_MODULE_MODULE_PATCH not set).";
    return false;
  }

  XexImage patch_declared{};
  parse_execution_info(update_bytes, patch_header_size, patch_entries, patch_declared);
  if (patch_declared.title_id != base_image.title_id) {
    if (error) *error = "Title-update title ID does not match the base image.";
    return false;
  }
  if (patch_declared.media_id != 0u && base_image.media_id != 0u &&
      patch_declared.media_id != base_image.media_id) {
    if (error) *error = "Title-update media ID does not match the base image.";
    return false;
  }
  if (patch_descriptor.present) {
    const auto base_signature_digest = crypto::sha1(base_image.security.rsa_signature);
    if (!std::equal(base_signature_digest.begin(), base_signature_digest.end(),
                    patch_descriptor.base_signature_digest.begin())) {
      if (error) *error = "Title-update base-signature digest does not match the base image.";
      return false;
    }
    if (patch_descriptor.source_version.value != base_image.execution_info.version.value) {
      if (error) *error = "Title-update source version does not match the base image's version.";
      return false;
    }
  }

  if (!patch_is_full_patch && !patch_is_delta_patch) {
    if (error) *error = "Title-update is neither a full nor delta patch (unsupported patch kind).";
    return false;
  }

  XexImage patch_image{};
  std::string parse_error;
  if (!parse_xex_image(full_patch_bytes, patch_image, &parse_error, base_image.effective_image)) {
    if (error) *error = "Title-update image failed to parse: " + parse_error;
    return false;
  }

  // For both patch kinds, parse_xex_image() has already produced the
  // correct effective image: XEX_MODULE_PATCH_DELTA bodies were LZXDELTA-
  // decoded against base_image.effective_image as reference data (via
  // reference_image above), and XEX_MODULE_PATCH_FULL bodies are a complete
  // replacement image in their own right.
  out_image = std::move(patch_image);
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
