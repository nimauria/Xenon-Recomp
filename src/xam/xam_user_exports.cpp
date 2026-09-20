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

  // XamUserGetXUID (0x0180)
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

  // XamUserGetSigninState (0x0181)
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

  // XamUserGetName (0x0183)
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

  // XamUserCheckPrivilege (0x0187) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserCheckPrivilege";
    desc.ordinal = ordinal::XamUserCheckPrivilege;
    desc.requirement = core::ExportRequirement::Stubbed;
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

  return ok;
}

}  // namespace xenon::xam
