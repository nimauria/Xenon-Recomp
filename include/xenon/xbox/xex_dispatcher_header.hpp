#pragma once

// Guest-memory X_DISPATCH_HEADER resolution (Ke* kernel-mode synchronization
// exports - AC6 Runtime Readiness pass). Real Xbox 360 titles frequently
// embed a KEVENT/KSEMAPHORE/... directly inside their own data structures
// and call the Ke* family (KeSetEvent, KeWaitForSingleObject, ...) with a
// guest POINTER to that embedded struct, never a Handle - distinct from the
// Nt* family (see xboxkrnl_sync_exports.hpp), which is already implemented
// and handle-based.
//
// X_DISPATCH_HEADER is the real, stable, documented Windows NT
// DISPATCHER_HEADER structure (32-bit layout, matching the Xbox 360's
// 32-bit PPC guest ABI) - not an Xbox-specific unknown:
//   offset 0x0: uint8_t  type   (KOBJECTS enum: 0=EventNotification,
//                                1=EventSynchronization, 2=Mutant,
//                                3=Process, 4=Queue, 5=Semaphore, 6=Thread,
//                                8/9=Timer)
//   offset 0x1: uint8_t  (per-type flags union - not read here)
//   offset 0x2: uint8_t  (per-type union - not read here)
//   offset 0x3: uint8_t  (per-type union - not read here)
//   offset 0x4: uint32_t signal_state (big-endian on guest)
//   offset 0x8: uint32_t wait_list_flink (big-endian)
//   offset 0xC: uint32_t wait_list_blink (big-endian)
// Total size 0x10 (16) bytes. X_KSEMAPHORE additionally has an int32_t
// `limit` field immediately after the header, at offset 0x10 - real NT
// KSEMAPHORE is `{ DISPATCHER_HEADER Header; LONG Limit; }`.
//
// wait_list_flink/wait_list_blink are real kernel-internal wait-list
// linkage on hardware; guest (title) code never reads or writes them
// itself - only the kernel does, to link waiting threads. This makes them
// safe to repurpose, exactly as xenia-project/xenia does (independently
// verified, not copied - see xobject.cc's GetNativeObject/StashHandle): once
// a header has been associated with a host object, wait_list_flink is
// overwritten with a signature marker and wait_list_blink holds that
// object's real xenon::kernel::Handle in the owning KernelProcess's shared
// handle_table(), so a repeat call on the same guest address resolves in
// O(1) without recreating a new host object (which would silently reset the
// object's state - e.g. a second KeSetEvent's caller expects the SAME
// event, not a fresh unsignaled one).
//
// Deliberately does NOT support type 2 (Mutant): real NT KMUTANT has extra
// fields beyond the base header (a MutantListEntry LIST_ENTRY, OwnerThread
// pointer, Abandoned/ApcDisable bytes) whose exact Xbox 360 offsets are not
// independently verified here - fabricating a layout risks silently
// misinterpreting or corrupting real guest memory. See
// docs/kernel/THREADING_V2.md.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/kernel/object.hpp"

namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

struct DispatchHeaderLayout {
  static constexpr std::uint32_t kTypeOffset = 0x0;
  static constexpr std::uint32_t kSignalStateOffset = 0x4;
  static constexpr std::uint32_t kWaitListFlinkOffset = 0x8;
  static constexpr std::uint32_t kWaitListBlinkOffset = 0xC;
  static constexpr std::uint32_t kSize = 0x10;
  // X_KSEMAPHORE-only: real NT KSEMAPHORE's Limit field, immediately after
  // the base DISPATCHER_HEADER.
  static constexpr std::uint32_t kSemaphoreLimitOffset = 0x10;
};

// Real NT KOBJECTS enum values this resolver understands (see
// DispatchHeaderLayout's doc comment for the full enum - only Event and
// Semaphore are modeled).
enum class DispatchObjectType : std::uint8_t {
  EventNotification = 0,
  EventSynchronization = 1,
  Semaphore = 5,
};

// Resolves (creating and stashing on first use if necessary) the host-side
// kernel::KernelObject for a guest X_DISPATCH_HEADER-based kernel object
// embedded at header_address in guest memory. Returns nullptr and sets
// *error on any failure (null pointer, unsupported/corrupt type byte,
// invalid semaphore count/limit, or handle-table exhaustion) - never
// silently substitutes a different object or guesses a type it cannot
// verify.
[[nodiscard]] std::shared_ptr<kernel::KernelObject> resolve_dispatcher_object(
    kernel::KernelProcess& process, cpu::MemoryPort& memory, cpu::GuestAddress header_address,
    std::string* error);

// Writes type/signal_state into a guest X_DISPATCH_HEADER and clears its
// wait_list_flink/wait_list_blink fields (invalidating any previously
// stashed host-object association at this address, exactly matching real
// Xbox 360 Ke*Initialize* semantics - initializing an object always starts
// it fresh). For a Semaphore, also writes the limit field at its offset.
// Does not itself create/stash a host object - the next resolve_dispatcher_object()
// call for this address does that lazily, on whichever Ke* call touches it
// first (mirrors the verified reference behavior).
void initialize_dispatch_header(cpu::MemoryPort& memory, cpu::GuestAddress header_address,
                                DispatchObjectType type, std::uint32_t signal_state,
                                std::optional<std::int32_t> semaphore_limit = std::nullopt);

}  // namespace xenon::xbox
