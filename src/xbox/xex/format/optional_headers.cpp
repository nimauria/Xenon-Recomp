// Optional header table and the fixed-layout optional headers the loader consumes.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"

namespace xenon::xbox::detail {

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

}  // namespace xenon::xbox::detail
