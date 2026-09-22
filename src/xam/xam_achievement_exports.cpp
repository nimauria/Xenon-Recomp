#include "xenon/core/export_registry.hpp"
#include "xenon/xam/achievement_manager.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

}  // namespace

// XamUserWriteAchievements/XamUserReadStats/XamUserWriteStats were removed
// from here: they were ordinals invented by earlier Xenon code with no
// corresponding xam.xex export under any name (verified against both
// xenia's src/xenia/kernel/xam/xam_table.inc and rexglue-sdk's
// src/kernel/xam/export_table.inc - neither lists nor implements them). A
// real retail title can never import a function by a name that does not
// exist in xam.xex, so registering them under any ordinal would still leave
// them permanently unreachable. On real hardware, achievement unlocks and
// stat updates are written directly into the title's cached profile GPD via
// the content APIs, not through a dedicated XAM ordinal call.
// AchievementManager::unlock_achievement()/write_stat() remain available as
// internal Xenon APIs (see XamSession::achievements()) for a future
// GPD-backed write path; see xam_exports.hpp for the full rationale.
bool register_achievement_exports(core::ExportRegistry& registry,
                                 AchievementManager& achievement_manager) {
  bool ok = true;

  // XamUserCreateAchievementEnumerator (0x02EE) - Stubbed. Real xam.xex
  // parameters are (title_id, user_index, flags, count_ptr, buffer_size_ptr,
  // out_handle_ptr); only the output handle is currently marshalled.
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

  // XamUserCreateStatsEnumerator (0x02F7) - Stubbed. Same parameter shape as
  // the achievement enumerator above; AchievementManager already tracks
  // per-title stats via write_stat()/enumerate_stats() for internal callers,
  // but the enumerator handle returned here does not yet back a real
  // walk-the-results XamEnumerate path (no generic enumerator/handle
  // infrastructure exists in Xenon yet - out of scope for ordinal
  // correctness).
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserCreateStatsEnumerator";
    desc.ordinal = ordinal::XamUserCreateStatsEnumerator;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto out_handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);

      if (out_handle_ptr != 0) {
        write_u32_be(ctx.memory, out_handle_ptr, 0xACE00002);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
