#include "xenon/xbox/xboxkrnl_xex_module_exports.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/xbox/module_registry.hpp"
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

namespace {

// Win32 error / NTSTATUS values used by the module-lookup exports.
constexpr std::uint32_t kErrorSuccess = 0u;
constexpr std::uint32_t kErrorInvalidParameter = 0x57u;
constexpr std::uint32_t kErrorNotFound = 0x490u;
constexpr std::uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr std::uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr std::uint32_t kStatusDriverOrdinalNotFound = 0xC0000262u;
constexpr std::uint32_t kStatusDriverEntrypointNotFound = 0xC0000263u;
constexpr std::size_t kMaxModuleNameBytes = 260u;

std::string read_guest_cstring(cpu::MemoryPort& memory, cpu::GuestAddress address,
                               std::size_t limit) {
  std::string text;
  for (std::size_t i = 0; i < limit; ++i) {
    const auto c = static_cast<char>(memory.read8(address + static_cast<std::uint32_t>(i)));
    if (c == '\0') break;
    text.push_back(c);
  }
  return text;
}

}  // namespace

// XexGetModuleHandle (ordinal 0x195)
// Guest ABI: r3 = guest pointer to an ANSI module name (NULL means the calling
// executable), r4 = guest pointer receiving the module handle -> r3 = Win32
// error code (NOT an NTSTATUS: ERROR_SUCCESS 0, ERROR_NOT_FOUND 0x490),
// verified against xenia's implementation. On failure the out handle is set to
// 0 like the reference.
bool xex_get_module_handle_export(GuestModuleRegistry& registry, core::ExportCallContext& context) {
  const auto name_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto out_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (out_ptr == 0u) {
    context.cpu.gpr[3] = kErrorInvalidParameter;
    return true;
  }
  const std::string name =
      name_ptr != 0u ? read_guest_cstring(context.memory, name_ptr, kMaxModuleNameBytes)
                     : std::string{};
  // A non-NULL pointer to an empty string is not "the executable".
  const auto handle = (name_ptr != 0u && name.empty()) ? std::nullopt : registry.module_handle(name);
  if (!handle) {
    context.memory.write32_be(out_ptr, 0u);
    context.cpu.gpr[3] = kErrorNotFound;
    return true;
  }
  context.memory.write32_be(out_ptr, *handle);
  context.cpu.gpr[3] = kErrorSuccess;
  return true;
}

// XexGetProcedureAddress (ordinal 0x197)
// Guest ABI: r3 = module handle (0 = the executable), r4 = an ordinal (<= 0xFFFF)
// or a guest pointer to an ANSI export name (any value with the high 16 bits
// set), r5 = guest pointer receiving the procedure address -> r3 = NTSTATUS.
// A system-module function resolves to a callable thunk address (see
// GuestModuleRegistry); a variable resolves to the variable's own address; the
// executable's exports resolve to their real image addresses. Unknown handle is
// STATUS_INVALID_HANDLE, an unknown ordinal STATUS_DRIVER_ORDINAL_NOT_FOUND and
// an unknown name STATUS_DRIVER_ENTRYPOINT_NOT_FOUND (verified against xenia).
bool xex_get_procedure_address_export(GuestModuleRegistry& registry,
                                      core::ExportCallContext& context) {
  const auto handle = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto ordinal_or_name = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto out_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  if (out_ptr == 0u) {
    context.cpu.gpr[3] = kStatusInvalidParameter;
    return true;
  }
  cpu::GuestAddress address{};
  GuestModuleRegistry::ProcedureResult result;
  if ((ordinal_or_name & 0xFFFF0000u) != 0u) {
    const auto name = read_guest_cstring(context.memory, ordinal_or_name, kMaxModuleNameBytes);
    result = registry.procedure_address_by_name(handle, name, address);
  } else {
    result = registry.procedure_address(handle, ordinal_or_name, address);
  }
  switch (result) {
    case GuestModuleRegistry::ProcedureResult::Found:
      context.memory.write32_be(out_ptr, address);
      context.cpu.gpr[3] = 0u;
      return true;
    case GuestModuleRegistry::ProcedureResult::InvalidHandle:
      context.cpu.gpr[3] = kStatusInvalidHandle;
      break;
    case GuestModuleRegistry::ProcedureResult::OrdinalNotFound:
      context.cpu.gpr[3] = kStatusDriverOrdinalNotFound;
      break;
    case GuestModuleRegistry::ProcedureResult::EntryPointNotFound:
      context.cpu.gpr[3] = kStatusDriverEntrypointNotFound;
      break;
  }
  context.memory.write32_be(out_ptr, 0u);
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
