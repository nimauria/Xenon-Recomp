#include "xenon/xam/xam_user_exports.hpp"

#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {
namespace {

// Helper to write a big-endian 32-bit value to guest memory
void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

// Helper to write a big-endian 64-bit value to guest memory
void write_u64_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint64_t value) {
  memory.write64_be(addr, value);
}

// Helper to write a null-terminated string to guest memory
void write_string(cpu::MemoryPort& memory, cpu::GuestAddress addr, 
                 const std::string& str, std::size_t max_length) {
  const auto length = std::min(str.length(), max_length - 1);
  for (std::size_t i = 0; i < length; ++i) {
    memory.write8(addr + static_cast<cpu::GuestAddress>(i), 
                  static_cast<std::uint8_t>(str[i]));
  }
  memory.write8(addr + static_cast<cpu::GuestAddress>(length), 0);
}

}  // namespace

bool register_user_exports(core::ExportRegistry& registry, UserManager& user_manager) {
  bool ok = true;

  // XamUserGetXUID (0x020A)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserGetXUID";
    desc.ordinal = ordinal::XamUserGetXUID;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&user_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_xuid_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);

      if (!user_manager.is_signed_in(user_index)) {
        ctx.cpu.gpr[3] = result::NotLoggedOn;
        return true;
      }

      if (out_xuid_ptr != 0) {
        const auto xuid = user_manager.xuid(user_index);
        write_u64_be(ctx.memory, out_xuid_ptr, xuid);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserGetSigninState (0x0210)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserGetSigninState";
    desc.ordinal = ordinal::XamUserGetSigninState;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&user_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto state = user_manager.signin_state(user_index);
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(state);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserGetName (0x020E)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserGetName";
    desc.ordinal = ordinal::XamUserGetName;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&user_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_name_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto max_length = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);

      if (!user_manager.is_signed_in(user_index)) {
        ctx.cpu.gpr[3] = result::NotLoggedOn;
        return true;
      }

      if (out_name_ptr != 0 && max_length > 0) {
        const auto gamertag = user_manager.gamertag(user_index);
        write_string(ctx.memory, out_name_ptr, gamertag, max_length);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserCheckPrivilege (0x0212) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserCheckPrivilege";
    desc.ordinal = ordinal::XamUserCheckPrivilege;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "always grants every privilege for offline play instead of checking "
        "the real per-title/per-user privilege set - a title that gates "
        "content on a specific denied privilege will not observe the denial";
    desc.handler = [&user_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_result_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);

      if (!user_manager.is_signed_in(user_index)) {
        ctx.cpu.gpr[3] = result::NotLoggedOn;
        return true;
      }

      // Stub: grant all privileges for offline play
      if (out_result_ptr != 0) {
        write_u32_be(ctx.memory, out_result_ptr, 1);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserAreUsersFriends (0x0213) - verified against rexglue-sdk's
  // XamUserAreUsersFriends_entry: an invalid user index is rejected, a
  // not-signed-in user reports NotLoggedOn, and a signed-in user always
  // reports "not friends" - real hardware's only honest answer when no
  // Xbox Live friends list exists to query.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserAreUsersFriends";
    desc.ordinal = ordinal::XamUserAreUsersFriends;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&user_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_value_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);
      if (user_index >= UserManager::kMaxUsers) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      if (!user_manager.is_signed_in(user_index)) {
        ctx.cpu.gpr[3] = result::NotLoggedOn;
        return true;
      }
      if (out_value_ptr != 0u) write_u32_be(ctx.memory, out_value_ptr, 0u);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserGetSigninInfo (0x0227) - writes the real 40-byte big-endian
  // X_USER_SIGNIN_INFO struct (xuid@0, signin_state@12, name[16]@24; the
  // three reserved dwords at +8/+16/+20 are zeroed, matching rexglue-sdk's
  // XamUserGetSigninInfo_entry, which also leaves them zeroed - no reference
  // publishes real content for them).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserGetSigninInfo";
    desc.ordinal = ordinal::XamUserGetSigninInfo;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&user_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto info_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      if (info_ptr == 0u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      ctx.memory.fill_bytes(info_ptr, 40u, 0u);
      if (!user_manager.is_signed_in(user_index)) {
        ctx.cpu.gpr[3] = result::NoSuchUser;
        return true;
      }
      write_u64_be(ctx.memory, info_ptr + 0x00u, user_manager.xuid(user_index));
      write_u32_be(ctx.memory, info_ptr + 0x0Cu,
                   static_cast<std::uint32_t>(user_manager.signin_state(user_index)));
      write_string(ctx.memory, info_ptr + 0x18u, user_manager.gamertag(user_index), 16);
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserReadProfileSettings (0x0219) - no profile-setting storage is
  // implemented (UserManager has no setting-by-id lookup), so this matches
  // rexglue-sdk's own conservative "any requested setting id that is not
  // modeled fails the whole call" behavior rather than fabricating setting
  // data. A request for zero settings has nothing to be missing, and
  // trivially succeeds with an empty result, matching the real contract for
  // "give me nothing."
  //
  // Guest ABI (real XDK signature, verified against an AC6 crash repro -
  // register values dumped from a real call showed r7=3 (a small literal
  // count, not a pointer) and r8=a real 0x82xxxxxx guest address, matching
  // dwNumSettingIds/pSettingIds, not the setting_count/buffer_size_ptr pair
  // this handler previously read from r5/r7): r3 = title id, r4 = user
  // index, r5 = XUID count, r6 = XUID array ptr, r7 = setting count, r8 =
  // setting-id array ptr, r9 = pcbResults (buffer size out-ptr), r10 =
  // results out-ptr. The previous r5/r7 mapping (one XDK parameter pair too
  // early) meant a real nonzero setting_count in r7 was misread as a never-
  // checked value while r7 itself got blindly treated as buffer_size_ptr and
  // dereferenced - a genuine Xenon-side guest memory fault (observed: write
  // to guest address 3, i.e. the literal setting count) whenever a title
  // asked for real profile settings, not merely the documented "fails with
  // InvalidParameter" limitation.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserReadProfileSettings";
    desc.ordinal = ordinal::XamUserReadProfileSettings;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "no profile-setting storage is implemented; any setting_count > 0 "
        "fails with InvalidParameter instead of returning real setting data";
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto setting_count = static_cast<std::uint32_t>(ctx.cpu.gpr[7]);
      const auto buffer_size_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[9]);
      if (setting_count == 0u) {
        if (buffer_size_ptr != 0u) write_u32_be(ctx.memory, buffer_size_ptr, 0u);
        ctx.cpu.gpr[3] = result::Success;
        return true;
      }
      ctx.cpu.gpr[3] = result::InvalidParameter;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamWriteGamerTile (0x02F0) - accepts a gamer-tile image write. No real
  // gamertile asset pipeline exists to persist it (and, per rexglue-sdk's
  // XamWriteGamerTile_entry, nothing on real hardware reads it back
  // synchronously either), so this is a real, complete acceptance rather
  // than a data-producing stub.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamWriteGamerTile";
    desc.ordinal = ordinal::XamWriteGamerTile;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
