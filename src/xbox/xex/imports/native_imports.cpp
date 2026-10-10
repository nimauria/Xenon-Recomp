// XEX-native import libraries (XEX_HEADER_IMPORT_LIBRARIES).

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"

namespace xenon::xbox::detail {

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

}  // namespace xenon::xbox::detail
