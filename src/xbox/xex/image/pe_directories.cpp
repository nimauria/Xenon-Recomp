// PE section table and data directories inside the effective image.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"

namespace xenon::xbox::detail {
namespace {

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

}  // namespace

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

}  // namespace xenon::xbox::detail
