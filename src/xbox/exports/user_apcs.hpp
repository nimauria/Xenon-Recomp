#pragma once

// Private to the xboxkrnl exports. Runs the user APCs queued for the calling
// guest thread after one of its alertable waits or delays returned
// STATUS_USER_APC, as xenia and rexglue-sdk do on the same return paths.

#include <cstdint>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/logging/logger.hpp"

namespace xenon::xbox {

// Each APC runs NormalRoutine(NormalContext, SystemArgument1,
// SystemArgument2). APCs queued while one runs are delivered in the same
// call, so the wait returns with the queue empty.
inline void deliver_user_apcs(core::ExportCallContext& context) {
  auto* self = kernel::KernelThread::current();
  if (!self) return;
  for (auto apcs = self->take_user_apcs(); !apcs.empty(); apcs = self->take_user_apcs()) {
    for (const auto& apc : apcs) {
      if (!context.guest_call) {
        logging::Logger::instance().log_if_enabled(logging::Level::Warning, "apc", [&] {
          return std::string("user APC dropped: this caller cannot run guest code");
        });
        continue;
      }
      const std::uint64_t arguments[] = {apc.context, apc.arg1, apc.arg2};
      static_cast<void>((*context.guest_call)(context.cpu, apc.routine, arguments));
    }
  }
}

}  // namespace xenon::xbox
