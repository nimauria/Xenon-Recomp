#include <cstddef>
#include <memory>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/xam/achievement_manager.hpp"
#include "xenon/xam/guest_enumerator.hpp"
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
                                 AchievementManager& /*achievement_manager*/,
                                 core::XenonSession& session) {
  bool ok = true;

  // Both enumerator calls take eight arguments with the buffer size and the
  // handle out-pointers last: r9 and r10. Earlier Xenon code wrote a fixed
  // fake handle through r7 (offset for achievements, size for stats) and
  // left the real out-pointers unwritten, so the title's handle and buffer
  // size stayed uninitialized and a non-zero offset was written through as
  // an address.
  //
  // Each now returns a real enumerator handle, which XamEnumerate walks and
  // NtClose closes. Xenon reads no XDBF achievement table and has no
  // offline stats, so both enumerators are empty and the first
  // XamEnumerate reports ERROR_NO_MORE_FILES - what rexglue-sdk does for a
  // title without XDBF data (xenia instead fabricates dummy entries).
  const auto create_empty_enumerator = [&session](core::ExportCallContext& ctx,
                                                  std::uint32_t item_size,
                                                  std::uint32_t items_per_enumerate,
                                                  cpu::GuestAddress handle_ptr) {
    auto* process = session.kernel_process();
    kernel::Handle handle{};
    if (process == nullptr ||
        process->handle_table().insert(
            std::make_shared<GuestEnumeratorObject>(item_size, items_per_enumerate,
                                                    std::vector<std::vector<std::byte>>{}),
            0u, kernel::HandleFlags::None, handle) != kernel::KernelIoCode::Success) {
      ctx.cpu.gpr[3] = result::FunctionFailed;
      return true;
    }
    write_u32_be(ctx.memory, handle_ptr, handle);
    ctx.cpu.gpr[3] = result::Success;
    return true;
  };

  // XamUserCreateAchievementEnumerator (0x02EE): (title_id, user_index,
  // xuid, flags, offset, count, buffer_size_ptr, handle_ptr), as in xenia
  // and rexglue-sdk. Each entry is an X_ACHIEVEMENT_DETAILS (36 bytes) plus,
  // when flags requests any of the three strings, 464 bytes of string
  // space; the required buffer size is that times count.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserCreateAchievementEnumerator";
    desc.ordinal = ordinal::XamUserCreateAchievementEnumerator;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "the enumerator lists no achievements: Xenon reads no XDBF achievement table";
    desc.handler = [create_empty_enumerator](core::ExportCallContext& ctx) -> bool {
      constexpr std::uint32_t kDetailsSize = 36u;
      constexpr std::uint32_t kStringBufferSize = 464u;
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto flags = static_cast<std::uint32_t>(ctx.cpu.gpr[6]);
      const auto count = static_cast<std::uint32_t>(ctx.cpu.gpr[8]);
      const auto buffer_size_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[9]);
      const auto handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[10]);
      if (count == 0u || buffer_size_ptr == 0u || handle_ptr == 0u || user_index >= 4u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      const auto entry_size = kDetailsSize + ((flags & 7u) != 0u ? kStringBufferSize : 0u);
      write_u32_be(ctx.memory, buffer_size_ptr, entry_size * count);
      return create_empty_enumerator(ctx, entry_size, count, handle_ptr);
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamUserCreateStatsEnumerator (0x02F7): eight arguments ending in
  // (..., buffer_size_ptr, handle_ptr); xenia-canary, and two rexglue-sdk
  // based ports that implement it, agree on those two positions but not on
  // the names of the middle ones, so only the out-pointers and user index
  // are used. Stats are Xbox Live leaderboard reads; offline there are none,
  // so the required buffer size is 0, as in xenia-canary.
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamUserCreateStatsEnumerator";
    desc.ordinal = ordinal::XamUserCreateStatsEnumerator;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "the enumerator lists no stats: leaderboards are online-only and Xenon is offline";
    desc.handler = [create_empty_enumerator](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
      const auto buffer_size_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[9]);
      const auto handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[10]);
      if (buffer_size_ptr == 0u || handle_ptr == 0u || user_index >= 4u) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      write_u32_be(ctx.memory, buffer_size_ptr, 0u);
      return create_empty_enumerator(ctx, 0u, 1u, handle_ptr);
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
