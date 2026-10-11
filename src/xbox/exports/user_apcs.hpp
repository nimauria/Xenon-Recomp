#pragma once

// Private to the xboxkrnl exports. Runs the user APCs queued for the calling
// guest thread after one of its alertable waits or delays returned
// STATUS_USER_APC, as xenia and rexglue-sdk do on the same return paths.

#include <cstdint>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/logging/logger.hpp"

namespace xenon::xbox {

// The Xbox 360 KAPC (40 bytes; 4 shorter than NT's), as xenia and
// rexglue-sdk lay it out.
struct KapcLayout {
  static constexpr std::uint32_t kSize = 40;
  static constexpr std::uint32_t kType = 0x00;  // u16, 18 = ApcObject
  static constexpr std::uint32_t kApcMode = 0x02;  // u8: 0 kernel, 1 user
  static constexpr std::uint32_t kEnqueued = 0x03;  // u8
  static constexpr std::uint32_t kThread = 0x04;  // guest KTHREAD*
  static constexpr std::uint32_t kListEntry = 0x08;  // LIST_ENTRY, 8 bytes
  static constexpr std::uint32_t kKernelRoutine = 0x10;
  static constexpr std::uint32_t kRundownRoutine = 0x14;
  static constexpr std::uint32_t kNormalRoutine = 0x18;
  static constexpr std::uint32_t kNormalContext = 0x1C;
  static constexpr std::uint32_t kArgument1 = 0x20;
  static constexpr std::uint32_t kArgument2 = 0x24;
  static constexpr std::uint16_t kApcObjectType = 18;
};

namespace detail {

inline bool run_routine(core::ExportCallContext& context, std::uint32_t routine,
                        std::initializer_list<std::uint64_t> arguments) {
  if (!context.guest_call) {
    logging::Logger::instance().log_if_enabled(logging::Level::Warning, "apc", [&] {
      return std::string("user APC dropped: this caller cannot run guest code");
    });
    return false;
  }
  return (*context.guest_call)(context.cpu, routine,
                               std::span<const std::uint64_t>(arguments.begin(), arguments.size()));
}

// A guest KAPC: KernelRoutine(Apc, &NormalRoutine, &NormalContext,
// &Argument1, &Argument2) runs first and may change those four values (or
// free the KAPC); then NormalRoutine(NormalContext, Argument1, Argument2)
// runs if it is still set. The four values are passed in a scratch block in
// the gap below the caller's stack that call_guest_routine() leaves unused.
inline void deliver_guest_kapc(core::ExportCallContext& context, std::uint32_t apc) {
  auto& memory = context.memory;
  memory.write8(apc + KapcLayout::kEnqueued, 0u);
  std::uint32_t values[4] = {memory.read32_be(apc + KapcLayout::kNormalRoutine),
                             memory.read32_be(apc + KapcLayout::kNormalContext),
                             memory.read32_be(apc + KapcLayout::kArgument1),
                             memory.read32_be(apc + KapcLayout::kArgument2)};
  const auto kernel_routine = memory.read32_be(apc + KapcLayout::kKernelRoutine);
  if (kernel_routine != 0u) {
    const auto scratch =
        static_cast<std::uint32_t>((context.cpu.gpr[1] - 0x40u) & ~std::uint64_t{0xF});
    for (std::uint32_t i = 0; i < 4; ++i) memory.write32_be(scratch + i * 4u, values[i]);
    run_routine(context, kernel_routine, {apc, scratch, scratch + 4u, scratch + 8u, scratch + 12u});
    for (std::uint32_t i = 0; i < 4; ++i) values[i] = memory.read32_be(scratch + i * 4u);
  }
  if (values[0] != 0u) run_routine(context, values[0], {values[1], values[2], values[3]});
}

}  // namespace detail

// Each APC runs NormalRoutine(NormalContext, SystemArgument1,
// SystemArgument2); a guest KAPC first runs its kernel routine (see above).
// APCs queued while one runs are delivered in the same call, so the wait
// returns with the queue empty.
inline void deliver_user_apcs(core::ExportCallContext& context) {
  auto* self = kernel::KernelThread::current();
  if (!self) return;
  for (auto apcs = self->take_user_apcs(); !apcs.empty(); apcs = self->take_user_apcs()) {
    for (const auto& apc : apcs) {
      if (apc.guest_apc != 0u) {
        detail::deliver_guest_kapc(context, apc.guest_apc);
      } else {
        detail::run_routine(context, apc.routine, {apc.context, apc.arg1, apc.arg2});
      }
    }
  }
}

}  // namespace xenon::xbox
