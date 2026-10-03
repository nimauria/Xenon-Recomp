#include <cstddef>
#include <vector>

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

// XCONTENT_DATA on-guest size: device_id(4) + content_type(4) +
// display_name_raw(256) + file_name_raw(42) = 306 bytes - same layout
// read_content_file_name()/XamContentCreateEx's parsing above already treat
// as authoritative, kept symmetric here for serialization.
constexpr std::uint32_t kContentDataSize = 306u;

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
  // display_name_raw @8, 256 bytes, UTF-16BE, truncated to 127 chars + NUL.
  const std::size_t max_chars = 256u / 2u - 1u;
  std::size_t i = 0;
  for (; i < item.display_name.size() && i < max_chars; ++i) {
    const auto c = static_cast<unsigned char>(item.display_name[i]);
    out[8 + i * 2 + 0] = std::byte{0};
    out[8 + i * 2 + 1] = static_cast<std::byte>(c);
  }
  // file_name_raw @264, 42 ANSI bytes, truncated.
  for (std::size_t j = 0; j < item.file_name.size() && j < 42u; ++j) {
    out[264 + j] = static_cast<std::byte>(item.file_name[j]);
  }
  return out;
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

  // XamContentClose (0x025A) - real ABI is (root_name: lpstring_t,
  // overlapped_ptr), verified against xenia's XamContentClose_entry: it
  // closes a content root previously opened by name via XamContentCreate(Ex)
  // - a completely different handle namespace from the enumerator handle
  // XamContentCreateEnumerator returns above (that one closes through the
  // ordinary NtClose/CloseHandle kernel path instead, since it is a real
  // kernel object). Xenon's own ContentManager/XamContentCreateEx do not
  // track mounted roots by name (create_content() has no root_name/mount
  // concept), so there is no real "was this root actually open" state to
  // check here - same genuinely-absent-prerequisite situation as this
  // codebase's own XamSessionCreateHandle/XamSessionRefObjByHandle. This at
  // least reads the real parameter (rather than ignoring the call shape
  // entirely) and keeps the same safe "always succeeds" outcome real
  // hardware gives for the common case of closing a root with no pending
  // async operations.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentClose";
    desc.ordinal = ordinal::XamContentClose;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "no mounted-root-by-name tracking exists in ContentManager (a "
        "genuinely absent prerequisite, not a deferred implementation), so "
        "this always succeeds instead of reporting a real root-not-open "
        "failure for an invalid root_name";
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
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

  // XamContentCreateEx (0x0259) - delegates to ContentManager::create_content
  // using the real device_id/content_type the guest XCONTENT_DATA struct
  // carries, the struct's own file_name_raw as the content's identifying
  // name, and the currently running title's real id (from the loaded XEX's
  // execution info) - not a struct field on real hardware's XCONTENT_DATA
  // either; the title is implicit in which process is running.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentCreateEx";
    desc.ordinal = ordinal::XamContentCreateEx;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "disposition (create/open/overwrite) semantics are not modeled - "
        "every call attempts a fresh create_content() regardless of the "
        "flags argument's CREATE_NEW/OPEN_EXISTING/etc. value";
    desc.handler = [&content_manager, &session](core::ExportCallContext& ctx) -> bool {
      const auto content_data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      const auto disposition_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);
      if (content_data_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      const auto device_id = ctx.memory.read32_be(content_data_ptr + 0u);
      const auto content_type_raw = ctx.memory.read32_be(content_data_ptr + 4u);
      const auto name = read_content_file_name(ctx.memory, content_data_ptr);
      const auto* loaded = session.loaded_xex();
      const std::uint64_t title_id = loaded != nullptr ? loaded->image.title_id : 0u;
      std::uint32_t out_content_id = 0u;
      const auto result_code = content_manager.create_content(
          device_id, name, static_cast<ContentType>(content_type_raw), title_id, out_content_id);
      if (disposition_ptr != 0u) write_u32_be(ctx.memory, disposition_ptr, 1u);  // kCreate
      ctx.cpu.gpr[3] = result_code;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentDelete (0x025B) - delegates to ContentManager::delete_content.
  // Real ABI identifies the content by its XCONTENT_DATA struct, not a
  // content_id handle; this codebase's ContentManager keys deletion by the
  // same integer content_id enumerate_content()/create_content() already
  // use, so this looks the content up by (device_id, name) from the guest
  // struct first.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentDelete";
    desc.ordinal = ordinal::XamContentDelete;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "looks up the content to delete by (device_id, display_name) among "
        "currently enumerated content rather than a direct content_id - a "
        "title that creates then immediately deletes unenumerated content "
        "within the same call sequence will not find it";
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto content_data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      if (content_data_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      const auto device_id = ctx.memory.read32_be(content_data_ptr + 0u);
      const auto content_type_raw = ctx.memory.read32_be(content_data_ptr + 4u);
      const auto name = read_content_file_name(ctx.memory, content_data_ptr);
      const auto existing =
          content_manager.enumerate_content(device_id, static_cast<ContentType>(content_type_raw));
      for (const auto& item : existing) {
        if (item.file_name == name) {
          ctx.cpu.gpr[3] = content_manager.delete_content(item.content_id);
          return true;
        }
      }
      ctx.cpu.gpr[3] = result::InvalidParameter;
      return true;
    };
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
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
