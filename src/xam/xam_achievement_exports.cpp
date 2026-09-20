#include "xenon/core/export_registry.hpp"
#include "xenon/xam/achievement_manager.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

void write_u64_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint64_t value) {
  memory.write64_be(addr, value);
}

std::uint64_t read_u64_be(cpu::MemoryPort& memory, cpu::GuestAddress addr) {
  return memory.read64_be(addr);
}

}  // namespace

bool register_achievement_exports(core::ExportRegistry& registry,
                                 AchievementManager& achievement_manager) {
  bool ok = true;

  // XamUserWriteAchievements (0x0280)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserWriteAchievements";
    desc.ordinal = ordinal::XamUserWriteAchievements;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&achievement_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto title_id_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto achievement_id = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);

      std::uint64_t title_id = 0;
      if (title_id_ptr != 0) {
        title_id = read_u64_be(ctx.memory, title_id_ptr);
      }

      const auto result = achievement_manager.unlock_achievement(
          user_index, title_id, achievement_id);

      ctx.cpu.gpr[3] = result;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserReadStats (0x0281)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserReadStats";
    desc.ordinal = ordinal::XamUserReadStats;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [&achievement_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto title_id_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto stat_id = static_cast<std::uint32_t>(ctx.cpu.gpr[6]);
      const auto out_value_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);

      std::uint64_t title_id = 0;
      if (title_id_ptr != 0) {
        title_id = read_u64_be(ctx.memory, title_id_ptr);
      }

      auto stat_opt = achievement_manager.read_stat(user_index, title_id, stat_id);
      
      if (!stat_opt.has_value()) {
        ctx.cpu.gpr[3] = result::Empty;
        return true;
      }

      if (out_value_ptr != 0) {
        write_u64_be(ctx.memory, out_value_ptr, 
                     static_cast<std::uint64_t>(stat_opt->value));
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserWriteStats (0x0282)
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserWriteStats";
    desc.ordinal = ordinal::XamUserWriteStats;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [&achievement_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto title_id_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto stat_id = static_cast<std::uint32_t>(ctx.cpu.gpr[6]);
      const auto value = static_cast<std::int64_t>(ctx.cpu.gpr[7]);

      std::uint64_t title_id = 0;
      if (title_id_ptr != 0) {
        title_id = read_u64_be(ctx.memory, title_id_ptr);
      }

      const auto result = achievement_manager.write_stat(
          user_index, title_id, stat_id, value);

      ctx.cpu.gpr[3] = result;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserCreateAchievementEnumerator (0x0284) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserCreateAchievementEnumerator";
    desc.ordinal = ordinal::XamUserCreateAchievementEnumerator;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto out_handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);

      // Return stub handle
      if (out_handle_ptr != 0) {
        write_u32_be(ctx.memory, out_handle_ptr, 0xACE00001);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
