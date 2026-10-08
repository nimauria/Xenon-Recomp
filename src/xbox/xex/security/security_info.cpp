// XEX security info, page descriptors and page protection.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox::detail {
namespace {

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

}  // namespace

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

}  // namespace xenon::xbox::detail
