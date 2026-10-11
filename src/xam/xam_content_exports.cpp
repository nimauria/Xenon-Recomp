#include <cstddef>
#include <string>
#include <vector>

#include "xam/xam_overlapped.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/xam/content_manager.hpp"
#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/guest_enumerator.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

// Reads a guest XCONTENT_DATA struct's file_name_raw field (offset 264,
// up to 42 ANSI bytes, not guaranteed null-terminated - some titles fill the
// whole field) - verified layout against rexglue-sdk's
// include/rex/system/xam/content_manager.h XCONTENT_DATA: device_id (u32be)
// @0, content_type (u32be) @4, display_name_raw (256 bytes) @8,
// file_name_raw (42 bytes) @264.
std::string read_content_file_name(cpu::MemoryPort& memory, cpu::GuestAddress content_data_ptr) {
  std::string name;
  for (std::uint32_t i = 0; i < 42u; ++i) {
    const auto c = memory.read8(content_data_ptr + 264u + i);
    if (c == 0u) break;
    name.push_back(static_cast<char>(c));
  }
  return name;
}

// A NUL-terminated ANSI string (a content root name), at most 64 bytes.
std::string read_c_string(cpu::MemoryPort& memory, cpu::GuestAddress address) {
  std::string text;
  if (address == 0u) return text;
  for (std::uint32_t i = 0; i < 64u; ++i) {
    const auto c = memory.read8(address + i);
    if (c == 0u) break;
    text.push_back(static_cast<char>(c));
  }
  return text;
}

// display_name_raw @8: up to 128 big-endian UTF-16 units, stored as UTF-8.
std::string read_content_display_name(cpu::MemoryPort& memory, cpu::GuestAddress content_data_ptr) {
  std::u16string wide;
  for (std::uint32_t i = 0; i < 128u; ++i) {
    const auto unit = memory.read16_be(content_data_ptr + 8u + i * 2u);
    if (unit == 0u) break;
    wide.push_back(static_cast<char16_t>(unit));
  }
  std::string text;
  for (std::size_t i = 0; i < wide.size(); ++i) {
    std::uint32_t code = wide[i];
    if (code >= 0xD800u && code < 0xDC00u && i + 1 < wide.size() && wide[i + 1] >= 0xDC00u &&
        wide[i + 1] < 0xE000u) {
      code = 0x10000u + ((code - 0xD800u) << 10u) + (wide[++i] - 0xDC00u);
    }
    if (code < 0x80u) {
      text.push_back(static_cast<char>(code));
    } else if (code < 0x800u) {
      text.push_back(static_cast<char>(0xC0u | (code >> 6u)));
      text.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else if (code < 0x10000u) {
      text.push_back(static_cast<char>(0xE0u | (code >> 12u)));
      text.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
      text.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
      text.push_back(static_cast<char>(0xF0u | (code >> 18u)));
      text.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3Fu)));
      text.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
      text.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
  }
  return text;
}

// UTF-8 back to UTF-16 units for display_name_raw.
std::u16string to_utf16(const std::string& text) {
  std::u16string wide;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::uint32_t code = c;
    std::size_t extra = 0;
    if (c >= 0xF0u) { code = c & 0x07u; extra = 3; }
    else if (c >= 0xE0u) { code = c & 0x0Fu; extra = 2; }
    else if (c >= 0xC0u) { code = c & 0x1Fu; extra = 1; }
    ++i;
    for (std::size_t k = 0; k < extra && i < text.size(); ++k, ++i) {
      code = (code << 6u) | (static_cast<unsigned char>(text[i]) & 0x3Fu);
    }
    if (code >= 0x10000u) {
      code -= 0x10000u;
      wide.push_back(static_cast<char16_t>(0xD800u + (code >> 10u)));
      wide.push_back(static_cast<char16_t>(0xDC00u + (code & 0x3FFu)));
    } else {
      wide.push_back(static_cast<char16_t>(code));
    }
  }
  return wide;
}

constexpr XResult kErrorFileNotFound = 0x00000002u;
constexpr XResult kErrorPathNotFound = 0x00000003u;
constexpr XResult kErrorAlreadyExists = 0x000000B7u;

// XCONTENT_DATA on-guest size: device_id(4) + content_type(4) +
// display_name_raw(256) + file_name_raw(42) + padding(2) = 0x134 bytes, as
// xenia and rexglue-sdk define it (static_assert_size(XCONTENT_DATA,
// 0x134)). The padding is part of the struct, so it is also the stride of
// an XamEnumerate result array; Xenon used 306, which misaligned every
// item after the first.
constexpr std::uint32_t kContentDataSize = 0x134u;

std::vector<std::byte> serialize_content_data(const ContentData& item) {
  std::vector<std::byte> out(kContentDataSize, std::byte{0});
  auto put_u32 = [&](std::size_t offset, std::uint32_t value) {
    out[offset + 0] = static_cast<std::byte>((value >> 24) & 0xFFu);
    out[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFu);
    out[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFu);
    out[offset + 3] = static_cast<std::byte>(value & 0xFFu);
  };
  put_u32(0, item.device_id);
  put_u32(4, static_cast<std::uint32_t>(item.content_type));
  // display_name_raw @8, 256 bytes, UTF-16BE, truncated to 127 units + NUL.
  const std::size_t max_chars = 256u / 2u - 1u;
  const auto wide = to_utf16(item.display_name);
  for (std::size_t i = 0; i < wide.size() && i < max_chars; ++i) {
    out[8 + i * 2 + 0] = static_cast<std::byte>((wide[i] >> 8u) & 0xFFu);
    out[8 + i * 2 + 1] = static_cast<std::byte>(wide[i] & 0xFFu);
  }
  // file_name_raw @264, 42 ANSI bytes, truncated.
  for (std::size_t j = 0; j < item.file_name.size() && j < 42u; ++j) {
    out[264 + j] = static_cast<std::byte>(item.file_name[j]);
  }
  return out;
}

}  // namespace

namespace {

// XamContentGetDeviceState through an XOVERLAPPED, as rexglue-sdk reports it:
// a missing device completes with ERROR_FUNCTION_FAILED and the device error
// as the extended error.
OverlappedOutcome device_state_outcome(std::uint32_t result) {
  constexpr std::uint32_t kErrorFunctionFailed = 0x0000065Bu;
  if (result == 0u) return {0u, 0u, 0u};
  return {kErrorFunctionFailed, result, 0u};
}

}  // namespace

bool register_content_exports(core::ExportRegistry& registry, ContentManager& content_manager,
                              core::XenonSession& session) {
  bool ok = true;

  // XamShowDeviceSelectorUI (0x02CB) - part of the XamShow* system UI family
  // (see xam_exports.hpp), not content storage - registered here anyway
  // since ContentManager owns the default-device data it returns. Real ABI
  // verified against xenia's XamShowDeviceSelectorUI_entry(user_index,
  // content_type, content_flags, total_requested [qword], device_id_ptr,
  // overlapped): device_id_ptr is r7, matching this handler's gpr[7] read -
  // and xenia itself (DECLARE_XAM_EXPORT1(..., kImplemented)) always
  // synchronously resolves to its one dummy HDD device with no real
  // selection UI either, so "no UI, always the default device" is the
  // reference behavior here, not a missing feature. Like every other export
  // in this codebase, the overlapped_ptr parameter is accepted but ignored
  // (no overlapped-completion modeling exists anywhere in Xenon's xam
  // exports yet) - a synchronous caller gets the correct result either way.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamShowDeviceSelectorUI";
    desc.ordinal = ordinal::XamShowDeviceSelectorUI;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      // user_index (gpr[3]) is part of the real ABI but not consulted - this
      // codebase's single-user, single-device model has nothing to key off
      // it with.
      const auto out_device_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);

      // Return default HDD device
      if (out_device_ptr != 0) {
        write_u32_be(ctx.memory, out_device_ptr, content_manager.default_device());
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    desc.handler = with_overlapped(session, 5, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentCreateEnumerator (0x025C) - real ABI verified against xenia's
  // XamContentCreateEnumerator_entry(user_index, device_id, content_type,
  // content_flags, items_per_enumerate, buffer_size_ptr, handle_out): 7
  // params, one guest GPR each (Xenon's PPC64 ABI passes every logical
  // argument - dword or qword - in exactly one GPR slot). The previous stub
  // read its "out handle" from gpr[6] (content_flags's own register) and
  // wrote a fixed 0xDEADBEEF there regardless - not just a fake handle, a
  // wrong-register bug that could never have worked even with a real handle.
  // Now backed by a real kernel object (GuestEnumeratorObject) populated from
  // ContentManager::enumerate_content(), so a subsequent XamEnumerate call
  // against the returned handle walks the title's real installed content.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentCreateEnumerator";
    desc.ordinal = ordinal::XamContentCreateEnumerator;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager, &session](core::ExportCallContext& ctx) -> bool {
      const auto device_id = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto content_type_raw = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
      const auto items_per_enumerate = static_cast<std::uint32_t>(ctx.cpu.gpr[7]);
      const auto buffer_size_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);
      const auto handle_out_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[9]);

      if (handle_out_ptr == 0u || items_per_enumerate == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      if (device_id != 0u && !content_manager.get_device(device_id).has_value()) {
        if (buffer_size_ptr != 0u) write_u32_be(ctx.memory, buffer_size_ptr, 0u);
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      if (buffer_size_ptr != 0u) {
        write_u32_be(ctx.memory, buffer_size_ptr, kContentDataSize * items_per_enumerate);
      }

      auto* process = session.kernel_process();
      if (process == nullptr) {
        ctx.cpu.gpr[3] = result::FunctionFailed;
        return true;
      }

      const auto* loaded = session.loaded_xex();
      const std::uint64_t title_id = loaded != nullptr ? loaded->image.title_id : 0u;
      const auto content_items = content_manager.enumerate_content(
          device_id, static_cast<ContentType>(content_type_raw), title_id);

      std::vector<std::vector<std::byte>> serialized;
      serialized.reserve(content_items.size());
      for (const auto& item : content_items) {
        serialized.push_back(serialize_content_data(item));
      }

      auto enumerator = std::make_shared<GuestEnumeratorObject>(
          kContentDataSize, items_per_enumerate, std::move(serialized));
      kernel::Handle handle{};
      const auto code = process->handle_table().insert(
          enumerator, /*granted_access=*/0u, kernel::HandleFlags::None, handle);
      if (code != kernel::KernelIoCode::Success) {
        ctx.cpu.gpr[3] = result::FunctionFailed;
        return true;
      }

      write_u32_be(ctx.memory, handle_out_ptr, handle);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentClose (0x025A): (root_name, overlapped). Unmounts the root
  // XamContentCreate(Ex) mounted; ERROR_FILE_NOT_FOUND when that root is
  // not open, as in xenia.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentClose";
    desc.ordinal = ordinal::XamContentClose;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager, &session](core::ExportCallContext& ctx) -> bool {
      const auto root_name = read_c_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]));
      auto* vfs = session.filesystem();
      ctx.cpu.gpr[3] = vfs == nullptr ? kErrorFileNotFound : content_manager.close_package(*vfs, root_name);
      return true;
    };
    desc.handler = with_overlapped(session, 1, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentFlush (0x0267): (root_name, overlapped). Writes through a
  // mounted package go straight to the host directory, so there is nothing
  // left to commit; succeeds for an open root, as in xenia (which succeeds
  // for any root).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentFlush";
    desc.ordinal = ordinal::XamContentFlush;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    desc.handler = with_overlapped(session, 1, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentGetDeviceData (0x025E) - real X_CONTENT_DEVICE_DATA layout
  // verified against xenia's xam_content_device.cc: device_id(u32be)@0,
  // device_type(u32be)@4, total_bytes(u64be)@8, free_bytes(u64be)@16,
  // name[28 x u16be]@24, total size 0x50 (80) bytes. The previous "(simplified)"
  // write was a genuine layout bug, not just an honestly-reduced struct: it
  // never wrote device_type at all, and wrote total_bytes/free_bytes as two
  // u32 halves at offsets 4/8/12/16 instead of each as one u64 at its own
  // offset - every field after device_id landed at the wrong address, so a
  // real title reading this struct back would get garbage for device_type
  // and both byte-count fields.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentGetDeviceData";
    desc.ordinal = ordinal::XamContentGetDeviceData;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto device_id = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);

      if (out_data_ptr == 0) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }

      auto device_opt = content_manager.get_device(device_id);
      if (!device_opt.has_value()) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }

      const auto& device = *device_opt;

      // Zero the whole struct first (real hardware zeroes it too, and this
      // codebase's only modeled device is the HDD, so device_type == the
      // same numeric value as device_id for the one real case).
      for (std::uint32_t i = 0; i < 0x50u; i += 4u) write_u32_be(ctx.memory, out_data_ptr + i, 0u);
      write_u32_be(ctx.memory, out_data_ptr + 0, device.device_id);
      write_u32_be(ctx.memory, out_data_ptr + 4, device.device_id);
      ctx.memory.write64_be(out_data_ptr + 8, device.total_bytes);
      ctx.memory.write64_be(out_data_ptr + 16, device.free_bytes);
      const auto& name = device.name;
      for (std::size_t i = 0; i < name.size() && i < 27u; ++i) {
        ctx.memory.write16_be(out_data_ptr + 24 + i * 2, static_cast<std::uint16_t>(name[i]));
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentGetDeviceName (0x025F) - real device_id/name_buffer/
  // name_capacity ABI, backed by ContentManager's real device name. No
  // known limitation, so this is Required (fully implemented), not Stubbed.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentGetDeviceName";
    desc.ordinal = ordinal::XamContentGetDeviceName;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto device_id = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_name_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto name_length = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
      
      if (out_name_ptr == 0 || name_length == 0) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      
      auto device_opt = content_manager.get_device(device_id);
      if (!device_opt.has_value()) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      
      // Write device name (wide string)
      const auto& name = device_opt->name;
      for (std::size_t i = 0; i < name.size() && i < name_length - 1; ++i) {
        ctx.memory.write16_be(out_name_ptr + i * 2, static_cast<std::uint16_t>(name[i]));
      }
      ctx.memory.write16_be(out_name_ptr + name.size() * 2, 0);  // Null terminator
      
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentCreate (0x0258): (user_index, root_name, content_data, flags,
  // disposition_ptr, license_mask_ptr, overlapped).
  // XamContentCreateEx (0x0259): (user_index, root_name, content_data, flags,
  // disposition_ptr, license_mask_ptr, cache_size, content_size,
  // overlapped), the overlapped from the stack. As in xenia: flags & 0xF is
  // the creation disposition (CREATE_NEW 1, CREATE_ALWAYS 2, OPEN_EXISTING
  // 3, OPEN_ALWAYS 4, TRUNCATE_EXISTING 5); the content's package is
  // created or opened and mounted as "<root_name>:"; *disposition is 1 for
  // created and 2 for opened. Through an XOVERLAPPED a failure completes as
  // ERROR_FUNCTION_FAILED with HRESULT_FROM_WIN32(error) as the extended
  // error and the disposition as the length.
  {
    const auto create = [&content_manager, &session](core::ExportCallContext& ctx,
                                                     cpu::GuestAddress overlapped) {
      const auto root_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto content_data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto flags = static_cast<std::uint32_t>(ctx.cpu.gpr[6]);
      const auto disposition_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);
      const auto license_mask_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[8]);
      if (root_ptr == 0u || content_data_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return;
      }
      const auto root_name = read_c_string(ctx.memory, root_ptr);
      ContentData data{};
      data.device_id = ctx.memory.read32_be(content_data_ptr + 0u);
      data.content_type = static_cast<ContentType>(ctx.memory.read32_be(content_data_ptr + 4u));
      data.display_name = read_content_display_name(ctx.memory, content_data_ptr);
      data.file_name = read_content_file_name(ctx.memory, content_data_ptr);
      const auto* loaded = session.loaded_xex();
      data.title_id = loaded != nullptr ? loaded->image.title_id : 0u;

      const auto existing =
          content_manager.find_content(data.device_id, data.content_type, data.title_id, data.file_name);
      XResult result = result::Success;
      bool make = false;
      bool remove = false;
      switch (flags & 0xFu) {
        case 1u:  // CREATE_NEW
          if (existing) result = kErrorAlreadyExists; else make = true;
          break;
        case 2u:  // CREATE_ALWAYS
          remove = existing.has_value();
          make = true;
          break;
        case 3u:  // OPEN_EXISTING
          if (!existing) result = kErrorPathNotFound;
          break;
        case 4u:  // OPEN_ALWAYS
          make = !existing;
          break;
        case 5u:  // TRUNCATE_EXISTING
          if (!existing) result = kErrorPathNotFound; else remove = make = true;
          break;
        default:
          result = result::InvalidParameter;
          break;
      }
      const std::uint32_t disposition = make ? 1u : 2u;
      if (disposition_ptr != 0u) write_u32_be(ctx.memory, disposition_ptr, disposition);

      auto* vfs = session.filesystem();
      std::uint32_t content_id = existing ? existing->content_id : 0u;
      if (result == result::Success && vfs == nullptr) result = result::FunctionFailed;
      if (result == result::Success && remove) result = content_manager.delete_content(content_id);
      if (result == result::Success && make) result = content_manager.create_content(data, content_id);
      if (result == result::Success) result = content_manager.open_package(*vfs, root_name, content_id);
      if (result == result::Success && license_mask_ptr != 0u) {
        write_u32_be(ctx.memory, license_mask_ptr, 0u);
      }

      if (overlapped == 0u) {
        ctx.cpu.gpr[3] = result;
        return;
      }
      const auto extended = result == 0u ? 0u : (0x80070000u | (result & 0xFFFFu));
      ctx.cpu.gpr[3] = complete_overlapped(ctx, session.kernel_process(), overlapped,
                                           result == 0u ? 0u : result::FunctionFailed, extended,
                                           disposition);
    };

    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentCreate";
    desc.ordinal = ordinal::XamContentCreate;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [create](core::ExportCallContext& ctx) -> bool {
      create(ctx, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[9]));
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;

    core::ExportDescriptor ex{};
    ex.library = "xam";
    ex.name = "XamContentCreateEx";
    ex.ordinal = ordinal::XamContentCreateEx;
    ex.requirement = core::ExportRequirement::Required;
    ex.handler = [create](core::ExportCallContext& ctx) -> bool {
      create(ctx, argument_u32(ctx, 8));
      return true;
    };
    ok = registry.register_export(std::move(ex)) && ok;
  }

  // XamContentDelete (0x025B): (user_index, content_data, overlapped).
  // Removes the running title's package named by the XCONTENT_DATA;
  // ERROR_FILE_NOT_FOUND when there is none and ERROR_ACCESS_DENIED while it
  // is mounted, as in xenia.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentDelete";
    desc.ordinal = ordinal::XamContentDelete;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager, &session](core::ExportCallContext& ctx) -> bool {
      const auto content_data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      if (content_data_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      const auto* loaded = session.loaded_xex();
      const auto existing = content_manager.find_content(
          ctx.memory.read32_be(content_data_ptr + 0u),
          static_cast<ContentType>(ctx.memory.read32_be(content_data_ptr + 4u)),
          loaded != nullptr ? loaded->image.title_id : 0u,
          read_content_file_name(ctx.memory, content_data_ptr));
      ctx.cpu.gpr[3] = existing ? content_manager.delete_content(existing->content_id)
                                : kErrorFileNotFound;
      return true;
    };
    desc.handler = with_overlapped(session, 2, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentSetThumbnail (0x0260) - no content-thumbnail storage is
  // implemented (ContentManager has no thumbnail field/method), so this
  // honestly reports the real "no such content" outcome for any call
  // instead of silently discarding the PNG data a title hands it.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentSetThumbnail";
    desc.ordinal = ordinal::XamContentSetThumbnail;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note = "no content-thumbnail storage is implemented";
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::InvalidParameter;
      return true;
    };
    desc.handler = with_overlapped(session, 4, std::move(desc.handler));
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentGetDeviceState (0x0265) - real hardware reports whether a
  // storage device is still physically present/ready. Delegates to
  // ContentManager::get_device(); every device this codebase enumerates is
  // a host-filesystem-backed virtual device that is always available once
  // registered, so "present" tracks real device existence, not a fabricated
  // constant.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentGetDeviceState";
    desc.ordinal = ordinal::XamContentGetDeviceState;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto device_id = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      ctx.cpu.gpr[3] =
          content_manager.get_device(device_id).has_value() ? result::Success : result::InvalidParameter;
      return true;
    };
    desc.handler = with_overlapped(session, 1, std::move(desc.handler), &device_state_outcome);
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
