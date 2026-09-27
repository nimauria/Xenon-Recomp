#include "xenon/xbox/xboxkrnl_xex_module_exports.hpp"

#include <cstdint>
#include <cstring>

#include "xenon/core/export_registry.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::xbox {
namespace {

// XEX2 root header layout (see xex_loader.cpp/rtl.cpp for the identical,
// independently-verified guest-memory version of this same layout):
//   +0x00: magic, +0x04: module_flags, +0x08: header_size, +0x0C: image_size,
//   +0x10: security_info_offset, +0x14: optional_header_count,
//   +0x18: optional_header_table[] (key(4) + value(4), big-endian).
constexpr std::size_t kOptionalHeaderTableOffset = 0x18u;
constexpr std::size_t kOptionalHeaderEntrySize = 8u;
constexpr std::uint32_t kXexHeaderSystemFlags = 0x00030000u;

std::uint32_t read_be32(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint32_t value = 0u;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
#if defined(_MSC_VER)
  return _byteswap_ulong(value);
#else
  return __builtin_bswap32(value);
#endif
}

// Looks up a single inline (size_class 0x00) 32-bit optional header value
// directly from the host-side header bytes captured at load time. Returns
// false (value left untouched) if the XEX has no such optional header - a
// title that never overrides the field, which is common and not an error.
bool find_inline_optional_header(const std::vector<std::byte>& header_bytes, std::uint32_t key,
                                 std::uint32_t& out_value) {
  if (header_bytes.size() < kOptionalHeaderTableOffset) return false;
  const auto header_size = read_be32(header_bytes, 0x08u);
  const auto count = read_be32(header_bytes, 0x14u);
  if (header_size > header_bytes.size()) return false;

  for (std::uint32_t i = 0u; i < count; ++i) {
    const auto entry_offset =
        kOptionalHeaderTableOffset + static_cast<std::size_t>(i) * kOptionalHeaderEntrySize;
    if (entry_offset + kOptionalHeaderEntrySize > header_size ||
        entry_offset + kOptionalHeaderEntrySize > header_bytes.size()) {
      return false;
    }
    if (read_be32(header_bytes, entry_offset) == key) {
      out_value = read_be32(header_bytes, entry_offset + 4u);
      return true;
    }
  }
  return false;
}

}  // namespace

bool xex_check_executable_privilege_export(const XexImage& image,
                                           core::ExportCallContext& context) {
  const auto privilege = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const std::uint32_t mask = 1u << (privilege & 31u);

  std::uint32_t system_flags = 0u;
  const bool found =
      find_inline_optional_header(image.header_bytes, kXexHeaderSystemFlags, system_flags);

  context.cpu.gpr[3] = (found && (system_flags & mask) != 0u) ? 1u : 0u;
  return true;
}

bool register_xboxkrnl_xex_module_exports(core::ExportRegistry& registry, const XexImage& image) {
  core::ExportDescriptor descriptor{};
  descriptor.library = "xboxkrnl.exe";
  descriptor.name = "XexCheckExecutablePrivilege";
  descriptor.ordinal = 0x194u;
  descriptor.requirement = core::ExportRequirement::Required;
  descriptor.handler = [&image](core::ExportCallContext& context) {
    return xex_check_executable_privilege_export(image, context);
  };
  return registry.register_export(std::move(descriptor));
}

}  // namespace xenon::xbox
