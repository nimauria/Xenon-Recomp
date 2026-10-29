#pragma once

// Private to the xboxkrnl synchronisation exports. Records every guest wait
// and signal as a bounded diagnostic event (xenon/logging/diagnostic_events.hpp)
// so a stall can be traced to the object a thread waits on and the thread
// that last signalled it. Ke* exports pass the guest dispatcher header
// address; Nt* exports pass the handle.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/object.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/logging/diagnostic_events.hpp"
#include "xenon/xbox/xex_dispatcher_header.hpp"

namespace xenon::xbox::sync_events {

inline void record(logging::events::EventKind kind, const core::ExportCallContext& context,
                   const kernel::KernelObject* object, std::uint32_t guest_address,
                   std::uint32_t value, const char* source) {
  // Kept on the object whether or not events are recorded: the stop report
  // names an open wait's last signaller from it.
  if (kind == logging::events::EventKind::Signal && object)
    object->note_signal(context.thread_id, static_cast<std::uint32_t>(context.cpu.lr), source);
  if (!logging::events::enabled()) return;
  logging::events::Event event{};
  event.kind = kind;
  event.guest_thread_id = context.thread_id;
  event.guest_caller = static_cast<std::uint32_t>(context.cpu.lr);
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

// resolve_dispatcher_object() for a Ke* export, recording a GuestStateAdopted
// or GuestStateRejected event when guest code had written the header's
// SignalState directly.
inline std::shared_ptr<kernel::KernelObject> resolve_header(kernel::KernelProcess& process,
                                                           core::ExportCallContext& context,
                                                           cpu::GuestAddress header_address,
                                                           std::string* error,
                                                           const char* source) {
  auto sync = kernel::GuestStateSync::Unchanged;
  auto object = resolve_dispatcher_object(process, context.memory, header_address, error, &sync);
  if (object && sync != kernel::GuestStateSync::Unchanged) {
    const auto kind = sync == kernel::GuestStateSync::Adopted
                          ? logging::events::EventKind::GuestStateAdopted
                          : logging::events::EventKind::GuestStateRejected;
    record(kind, context, object.get(), header_address,
           context.memory.read32_be(header_address + DispatchHeaderLayout::kSignalStateOffset),
           source);
  }
  return object;
}

}  // namespace xenon::xbox::sync_events
