#pragma once

// Per-guest-thread KPCR (Processor Control Region) + compiler-emitted static
// TLS block setup. Free functions (not XenonSession methods) specifically so
// they can be unit-tested directly with two independent calls to prove two
// threads get distinct TLS instances from the same XexTls template - see
// tests/core/session_tests.cpp.
//
// This lives in xenon_core (not xenon_kernel) because it needs
// xenon::xbox::XexTls, and xenon_kernel must not depend on xenon_xbox_kernel_io
// (dependency runs the other way: XEX Loader V2 depends on the kernel, not
// vice versa).
//
// Real Xbox 360 titles' compiler-generated code accesses __declspec(thread)
// TLS variables through r13, which points at a per-thread KPCR structure
// (0x2D8 bytes on real hardware) whose tls_ptr field (offset 0x0) holds the
// actual TLS block address. Xenon does not need to reimplement that access
// pattern - static recompilation carries the original PPC instructions
// (lwz/stw against r13) forward unchanged - it only needs to lay out a KPCR
// with the fields real compiled code is known to read at the same offsets a
// real Xbox 360 kernel would use, and point r13 (CpuState::gpr[13]) at it.
// Field offsets below match xenia-project/xenia's X_KPCR (research reference
// per CLAUDE.md; independently reimplemented here against Xenon's own
// AddressSpace/XexTls types, not copied).

#include <cstdint>
#include <optional>

#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::core {

// Field layout of the guest-visible KPCR block. Xenon only ever writes/reads
// the subset of the real 0x2D8-byte structure that TLS setup and stack-limit
// queries need; the rest is zero-filled reserved space, matching what a real
// Xbox 360 kernel would leave for fields this pass does not model.
//
// `current_thread` (offset 0x100, the KPCR's X_KPRCB.current_thread field -
// see GuestKthreadLayout below) used to be one of those left-zero fields, on
// the assumption that nothing reads a guest-visible KTHREAD back through
// guest memory. That assumption is false: real AC6 compiled code contains a
// tiny, extremely hot accessor - `lwz r11, 0x100(r13); lwz r3, 0x14C(r11)` -
// that dereferences this exact pointer to read the current thread's
// thread_id (KTHREAD offset 0x14C in xenia-project/xenia's X_KTHREAD, our
// research reference per CLAUDE.md - independently reimplemented here, not
// copied). With current_thread left null, that load faults at guest address
// 0x14C and kills the session before any guest code past early thread setup
// can run. GuestKthreadLayout/kthread_address below close that gap.
struct GuestKpcrLayout {
  static constexpr std::uint32_t kSize = 0x2D8;
  static constexpr std::uint32_t kTlsPtrOffset = 0x000;
  static constexpr std::uint32_t kSelfOffset = 0x030;
  static constexpr std::uint32_t kStackBaseOffset = 0x070;  // high address
  static constexpr std::uint32_t kStackLimitOffset = 0x074;  // low address
  static constexpr std::uint32_t kCurrentThreadOffset = 0x100;
};

// Field layout of the guest-visible KTHREAD block `current_thread` points
// at. Sized and offset to match the real X_KTHREAD structure (xenia-project/
// xenia research reference); Xenon only populates the fields real compiled
// code is known to read (thread_id, plus stack_base/stack_limit since this
// same call already computes those exact values for the KPCR). The rest
// stays zero-filled reserved space, same policy as GuestKpcrLayout above.
struct GuestKthreadLayout {
  static constexpr std::uint32_t kSize = 0xAB0;
  static constexpr std::uint32_t kStackBaseOffset = 0x05C;   // high address
  static constexpr std::uint32_t kStackLimitOffset = 0x060;  // low address
  static constexpr std::uint32_t kThreadIdOffset = 0x14C;
};

struct GuestThreadTlsContext {
  memory::GuestAddress kpcr_address{};
  memory::GuestAddress kthread_address{};
  memory::GuestAddress tls_address{};
  std::uint32_t tls_size{};
};

// Allocates a KPCR block, a guest-visible KTHREAD block (current_thread's
// target - see GuestKthreadLayout), and a per-thread static-TLS block (sized
// from tls_info->data_size, or a minimal KPCR-only allocation with no TLS
// block if tls_info is empty - most Xbox 360 titles have no compiler-emitted
// TLS at all), copies tls_info->raw_data_size bytes from
// tls_info->raw_data_start (already an absolute guest virtual address per
// the XEX/PE TLS directory format - see xex_loader.cpp's
// parse_native_tls/parse_pe_tls_directory) and zero-fills the remainder, and
// writes tls_ptr/self/stack_base/stack_limit into the KPCR and
// current_thread/stack_base/stack_limit into the KTHREAD. thread_id is not
// written here - the calling KernelThread does not exist yet at this point
// in every call site (see write_guest_thread_id()). Returns the addresses to
// install into the new thread's CpuState::gpr[13] (the KPCR address).
[[nodiscard]] bool setup_guest_thread_tls_context(
    memory::AddressSpace& memory, const std::optional<xbox::XexTls>& tls_info,
    memory::GuestAddress stack_base, std::uint32_t stack_size,
    GuestThreadTlsContext& out_context, std::string* error = nullptr);

// Writes the now-known KernelThread id into the KTHREAD block
// setup_guest_thread_tls_context() already allocated. Every call site
// creates the KTHREAD/KPCR pair before kernel::ThreadManager::create_thread()
// exists to hand back a real id (the same chicken-and-egg
// create-before-you-know-your-own-id shape XenonSession::export_ex_create_
// thread() already solves for its own thread_id_slot), so this is always a
// second, deferred write - never folded into setup_guest_thread_tls_context()
// itself. A no-op if context.kthread_address is unset.
void write_guest_thread_id(memory::AddressSpace& memory,
                           const GuestThreadTlsContext& context,
                           std::uint32_t thread_id) noexcept;

// Releases the allocations setup_guest_thread_tls_context() made. Safe to
// call on a default-constructed (never-allocated) context.
void release_guest_thread_tls_context(memory::AddressSpace& memory,
                                      const GuestThreadTlsContext& context) noexcept;

}  // namespace xenon::core
