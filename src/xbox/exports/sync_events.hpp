#pragma once

// Private to the xboxkrnl synchronisation exports. Records every guest wait
// and signal as a bounded diagnostic event (xenon/logging/diagnostic_events.hpp)
// so a stall can be traced to the object a thread waits on and the thread
// that last signalled it. Ke* exports pass the guest dispatcher header
// address; Nt* exports pass the handle.

#include <chrono>
#include <cstdint>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/object.hpp"
#include "xenon/logging/diagnostic_events.hpp"

namespace xenon::xbox::sync_events {

inline void record(logging::events::EventKind kind, const core::ExportCallContext& context,
                   const kernel::KernelObject* object, std::uint32_t guest_address,
                   std::uint32_t value, const char* source) {
  if (!logging::events::enabled()) return;
  logging::events::Event event{};
  event.kind = kind;
  event.guest_thread_id = context.thread_id;
  event.object_id = object ? object->object_id() : 0u;
  event.guest_address = guest_address;
  event.value = value;
  event.source = source;
  logging::events::record(event);
}

// The WaitBegin value: the timeout in milliseconds, 0xFFFFFFFF for none.
inline std::uint32_t timeout_value(std::chrono::milliseconds timeout) {
  if (timeout.count() < 0 || timeout.count() >= 0xFFFFFFFFll) return 0xFFFFFFFFu;
  return static_cast<std::uint32_t>(timeout.count());
}

}  // namespace xenon::xbox::sync_events
