#include "xenon/core/export_registry.hpp"
#include "xenon/xam/locale_manager.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

void write_string_wide(cpu::MemoryPort& memory, cpu::GuestAddress addr,
                       const std::string& str, std::size_t max_chars) {
  // Write as UTF-16 big-endian
  for (std::size_t i = 0; i < std::min(str.length(), max_chars - 1); ++i) {
    memory.write16_be(addr + static_cast<cpu::GuestAddress>(i * 2),
                      static_cast<std::uint16_t>(str[i]));
  }
  // Null terminator
  memory.write16_be(addr + static_cast<cpu::GuestAddress>(
                        std::min(str.length(), max_chars - 1) * 2), 0);
}

}  // namespace

bool register_locale_exports(core::ExportRegistry& registry, LocaleManager& locale_manager) {
  bool ok = true;

  // XamGetLanguage (0x03D2)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamGetLanguage";
    desc.ordinal = ordinal::XamGetLanguage;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&locale_manager](core::ExportCallContext& ctx) -> bool {
      const auto language = locale_manager.language();
      ctx.cpu.gpr[3] = static_cast<std::uint32_t>(language);
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamGetLocale (0x04A9)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamGetLocale";
    desc.ordinal = ordinal::XamGetLocale;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&locale_manager](core::ExportCallContext& ctx) -> bool {
      const auto locale = locale_manager.locale();
      ctx.cpu.gpr[3] = locale;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamQueryTimeZoneInformation (0x04AA) - real Xbox 360 identity for what
  // was previously registered under the invented name
  // "XamGetTimeZoneInformation"; see xam_exports.hpp for the distinct
  // GetTimeZoneInformation (0x043F, Win32-compatible re-export) and
  // XamSetTimeZoneInformation (0x04AB, write side) identities this does
  // NOT cover.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamQueryTimeZoneInformation";
    desc.ordinal = ordinal::XamQueryTimeZoneInformation;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [&locale_manager](core::ExportCallContext& ctx) -> bool {
      const auto out_tz_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);

      if (out_tz_ptr != 0) {
        const auto& tz = locale_manager.timezone();
        
        // Write bias (offset +0, 4 bytes)
        write_u32_be(ctx.memory, out_tz_ptr, 
                     static_cast<std::uint32_t>(tz.bias_minutes));
        
        // Write standard name (offset +4, 64 bytes = 32 wide chars)
        write_string_wide(ctx.memory, out_tz_ptr + 4, tz.standard_name, 32);
        
        // Write daylight name (offset +68, 64 bytes = 32 wide chars)
        write_string_wide(ctx.memory, out_tz_ptr + 68, tz.daylight_name, 32);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
