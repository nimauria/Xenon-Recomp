#include <cstdint>
#include <vector>

#include "xam/xam_overlapped.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/xam/profile_settings.hpp"
#include "xenon/xam/user_manager.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::xam {

// XamUserReadProfileSettings (0x0219), XamUserReadProfileSettingsEx (0x022A)
// and XamUserWriteProfileSettings (0x021A), over ProfileSettings. ABI and
// buffer layout as in xenia, xenia-canary and rexglue-sdk:
//
//   Read(title_id, user_index, xuid_count, xuids, setting_count, setting_ids,
//        buffer_size_ptr, buffer, overlapped)
//   ReadEx(..., setting_ids, buffer_size_ptr, buffer_size_ptr2, buffer, overlapped)
//   Write(title_id, user_index, setting_count, settings, overlapped)
//
// The result buffer is an X_USER_READ_PROFILE_SETTINGS header (count,
// pointer to the entries), then one 40-byte X_USER_PROFILE_SETTING per
// setting and profile, then the string and binary data the entries point
// at. A call with no buffer or too small a buffer returns
// ERROR_INSUFFICIENT_BUFFER and, when *buffer_size_ptr was 0, the size
// needed. Parameter and size errors return at once even with an
// XOVERLAPPED; the rest complete through it.
//
// A setting with no value comes back with source 0 (XSOURCE_NO_VALUE) and
// zeroed data. The references disagree here: rexglue-sdk fails the whole
// call for a setting its profile lacks and xenia-canary leaves such
// settings out of the result; Xenon follows the XUSER_PROFILE_SETTING
// source field instead, so a title reading its title-specific settings
// before ever writing them sees "no value" rather than an error.
namespace {

constexpr std::uint32_t kErrorInsufficientBuffer = 0x0000007Au;
constexpr std::uint32_t kErrorNoSuchUser = 0x00000525u;
constexpr std::uint32_t kHeaderSize = 8u;
constexpr std::uint32_t kSettingSize = 40u;
constexpr std::uint32_t kMaxSettings = 32u;

struct SettingLayout {
  static constexpr std::uint32_t kSource = 0x00;
  static constexpr std::uint32_t kOwner = 0x08;  // user index (u32) or XUID (u64)
  static constexpr std::uint32_t kSettingId = 0x10;
  static constexpr std::uint32_t kDataType = 0x18;
  static constexpr std::uint32_t kValue = 0x20;  // scalar, or size then pointer
  static constexpr std::uint32_t kValuePointer = 0x24;
};

bool has_data(ProfileSettingType type) {
  return type == ProfileSettingType::WString || type == ProfileSettingType::Binary;
}

bool is_wide_scalar(ProfileSettingType type) {
  return type == ProfileSettingType::Int64 || type == ProfileSettingType::Double ||
         type == ProfileSettingType::DateTime;
}

std::uint32_t current_title_id(core::XenonSession& session) {
  const auto* loaded = session.loaded_xex();
  return loaded ? loaded->image.execution_info.title_id : 0u;
}

// Finishes a call that has passed its parameter checks: directly, or through
// the XOVERLAPPED with HRESULT_FROM_WIN32(result) as the extended error.
void finish(core::ExportCallContext& ctx, core::XenonSession& session,
            cpu::GuestAddress overlapped, std::uint32_t result) {
  if (overlapped == 0u) {
    ctx.cpu.gpr[3] = result;
    return;
  }
  const auto extended = result == 0u ? 0u : (0x80070000u | (result & 0xFFFFu));
  ctx.cpu.gpr[3] = complete_overlapped(ctx, session.kernel_process(), overlapped, result,
                                       extended, 0u);
}

struct ReadRequest {
  std::uint32_t title_id;
  std::uint32_t user_index;
  std::uint32_t xuid_count;
  cpu::GuestAddress xuids;
  std::uint32_t setting_count;
  cpu::GuestAddress setting_ids;
  cpu::GuestAddress buffer_size_ptr;
  cpu::GuestAddress buffer;
  cpu::GuestAddress overlapped;
};

void read_profile_settings(core::ExportCallContext& ctx, core::XenonSession& session,
                           UserManager& users, ProfileSettings& settings,
                           const ReadRequest& request) {
  auto& memory = ctx.memory;
  if (request.setting_count < 1u || request.setting_count > kMaxSettings ||
      request.setting_ids == 0u || request.buffer_size_ptr == 0u ||
      (request.xuid_count != 0u && request.xuids == 0u) || request.xuid_count > 4u) {
    ctx.cpu.gpr[3] = result::InvalidParameter;
    return;
  }
  const auto buffer_size = memory.read32_be(request.buffer_size_ptr);
  if (buffer_size != 0u && request.buffer == 0u) {
    ctx.cpu.gpr[3] = result::InvalidParameter;
    return;
  }

  std::vector<std::uint32_t> ids(request.setting_count);
  std::uint32_t data_size = 0;
  for (std::uint32_t i = 0; i < request.setting_count; ++i) {
    ids[i] = memory.read32_be(request.setting_ids + i * 4u);
    if (has_data(profile_setting_type(ids[i]))) data_size += profile_setting_max_size(ids[i]);
  }
  const auto profiles = request.xuid_count == 0u ? 1u : request.xuid_count;
  const auto entries = profiles * request.setting_count;
  const auto needed = kHeaderSize + profiles * (request.setting_count * kSettingSize + data_size);
  if (request.buffer == 0u || buffer_size < needed) {
    if (buffer_size == 0u) memory.write32_be(request.buffer_size_ptr, needed);
    ctx.cpu.gpr[3] = kErrorInsufficientBuffer;
    return;
  }

  // The profiles asked for: the user at user_index, or each XUID listed.
  std::vector<XUID> xuids;
  if (request.xuid_count == 0u) {
    if (request.user_index >= 4u || !users.is_signed_in(request.user_index)) {
      finish(ctx, session, request.overlapped, kErrorNoSuchUser);
      return;
    }
    xuids.push_back(users.xuid(request.user_index));
  } else {
    for (std::uint32_t i = 0; i < request.xuid_count; ++i) {
      const auto xuid = memory.read64_be(request.xuids + i * 8u);
      if (!users.user_index_for_xuid(xuid)) {
        finish(ctx, session, request.overlapped, kErrorNoSuchUser);
        return;
      }
      xuids.push_back(xuid);
    }
  }

  const auto title_id = request.title_id != 0u ? request.title_id : current_title_id(session);
  const auto first_setting = request.buffer + kHeaderSize;
  auto data = first_setting + entries * kSettingSize;
  memory.write32_be(request.buffer, entries);
  memory.write32_be(request.buffer + 4u, first_setting);
  for (std::uint32_t p = 0; p < profiles; ++p) {
    for (std::uint32_t i = 0; i < request.setting_count; ++i) {
      const auto entry = first_setting + (p * request.setting_count + i) * kSettingSize;
      for (std::uint32_t b = 0; b < kSettingSize; b += 4u) memory.write32_be(entry + b, 0u);
      if (request.xuid_count == 0u) {
        memory.write32_be(entry + SettingLayout::kOwner, request.user_index);
      } else {
        memory.write64_be(entry + SettingLayout::kOwner, xuids[p]);
      }
      memory.write32_be(entry + SettingLayout::kSettingId, ids[i]);
      const auto value = settings.read(xuids[p], title_id, ids[i]);
      if (!value) continue;
      memory.write32_be(entry + SettingLayout::kSource,
                        profile_setting_is_title_specific(ids[i]) ? 2u : 1u);
      memory.write8(entry + SettingLayout::kDataType, static_cast<std::uint8_t>(value->type));
      if (has_data(value->type)) {
        // Bounded by the size the id declares, which the buffer was sized for.
        const auto budget = has_data(profile_setting_type(ids[i])) ? profile_setting_max_size(ids[i])
                                                                   : 0u;
        const auto size = static_cast<std::uint32_t>(
            value->bytes.size() < budget ? value->bytes.size() : budget);
        memory.write32_be(entry + SettingLayout::kValue, size);
        memory.write32_be(entry + SettingLayout::kValuePointer, size == 0u ? 0u : data);
        for (std::uint32_t b = 0; b < size; ++b) memory.write8(data + b, value->bytes[b]);
        data += size;
      } else if (is_wide_scalar(value->type)) {
        memory.write64_be(entry + SettingLayout::kValue, value->bits);
      } else {
        memory.write32_be(entry + SettingLayout::kValue, static_cast<std::uint32_t>(value->bits));
      }
    }
  }
  finish(ctx, session, request.overlapped, result::Success);
}

}  // namespace

bool register_profile_settings_exports(core::ExportRegistry& registry, UserManager& users,
                                       ProfileSettings& settings, core::XenonSession& session) {
  bool ok = true;

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserReadProfileSettings";
    desc.ordinal = ordinal::XamUserReadProfileSettings;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session, &users, &settings](core::ExportCallContext& ctx) -> bool {
      read_profile_settings(ctx, session, users, settings,
                            {argument_u32(ctx, 0), argument_u32(ctx, 1), argument_u32(ctx, 2),
                             argument_u32(ctx, 3), argument_u32(ctx, 4), argument_u32(ctx, 5),
                             argument_u32(ctx, 6), argument_u32(ctx, 7), argument_u32(ctx, 8)});
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserReadProfileSettingsEx";
    desc.ordinal = ordinal::XamUserReadProfileSettingsEx;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&session, &users, &settings](core::ExportCallContext& ctx) -> bool {
      const auto buffer_size_ptr = argument_u32(ctx, 6);
      read_profile_settings(ctx, session, users, settings,
                            {argument_u32(ctx, 0), argument_u32(ctx, 1), argument_u32(ctx, 2),
                             argument_u32(ctx, 3), argument_u32(ctx, 4), argument_u32(ctx, 5),
                             buffer_size_ptr != 0u ? buffer_size_ptr : argument_u32(ctx, 7),
                             argument_u32(ctx, 8), argument_u32(ctx, 9)});
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // Each X_USER_PROFILE_SETTING written is stored for the calling title; an
  // entry marked unset (data type 0xFF), or whose data type is not the type
  // its id declares, is skipped, as xenia-canary does. Binary and string
  // data longer than the id's declared size is cut to that size. A null
  // data pointer stores zeroes, as in rexglue-sdk.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserWriteProfileSettings";
    desc.ordinal = ordinal::XamUserWriteProfileSettings;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "the XN_SYS_PROFILESETTINGCHANGED notification is not sent";
    desc.handler = [&session, &users, &settings](core::ExportCallContext& ctx) -> bool {
      auto& memory = ctx.memory;
      const auto title_id_arg = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto count = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
      const auto entries = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);
      const auto overlapped = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);
      if (count == 0u || entries == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      if (user_index >= 4u || !users.is_signed_in(user_index)) {
        finish(ctx, session, overlapped, kErrorNoSuchUser);
        return true;
      }
      const auto xuid = users.xuid(user_index);
      const auto title_id = title_id_arg != 0u ? title_id_arg : current_title_id(session);
      bool saved = true;
      for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = entries + i * kSettingSize;
        const auto id = memory.read32_be(entry + SettingLayout::kSettingId);
        const auto type = static_cast<ProfileSettingType>(memory.read8(entry + SettingLayout::kDataType));
        if (type == ProfileSettingType::Unset || type != profile_setting_type(id)) continue;
        ProfileSettingValue value{type, 0u, {}};
        if (has_data(type) || type == ProfileSettingType::Content) {
          auto size = memory.read32_be(entry + SettingLayout::kValue);
          const auto max_size = profile_setting_max_size(id);
          if (size > max_size) size = max_size;
          const auto pointer = memory.read32_be(entry + SettingLayout::kValuePointer);
          value.bytes.resize(size);
          if (pointer != 0u) {
            for (std::uint32_t b = 0; b < size; ++b) value.bytes[b] = memory.read8(pointer + b);
          }
        } else if (is_wide_scalar(type)) {
          value.bits = memory.read64_be(entry + SettingLayout::kValue);
        } else {
          value.bits = memory.read32_be(entry + SettingLayout::kValue);
        }
        if (!settings.write(xuid, title_id, id, std::move(value))) saved = false;
      }
      // A title-specific setting the host could not save fails the call.
      finish(ctx, session, overlapped, saved ? result::Success : result::FunctionFailed);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
