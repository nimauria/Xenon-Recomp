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
// Xbox 360 kernel would leave for fields this pass does not model (notably
// `current_thread`, the guest-visible thread-object pointer at offset
// 0x100 - Xenon has no guest-visible KTHREAD struct to point it at, so it is
// left zero; nothing else in Xenon reads it back through guest memory, only
// through the host-side kernel::ThreadManager::current_thread()).
struct GuestKpcrLayout {
  static constexpr std::uint32_t kSize = 0x2D8;
  static constexpr std::uint32_t kTlsPtrOffset = 0x000;
  static constexpr std::uint32_t kSelfOffset = 0x030;
  static constexpr std::uint32_t kStackBaseOffset = 0x070;  // high address
  static constexpr std::uint32_t kStackLimitOffset = 0x074;  // low address
  static constexpr std::uint32_t kCurrentThreadOffset = 0x100;
};

struct GuestThreadTlsContext {
  memory::GuestAddress kpcr_address{};
  memory::GuestAddress tls_address{};
  std::uint32_t tls_size{};
};

// Allocates a KPCR block and a per-thread static-TLS block (sized from
// tls_info->data_size, or a minimal KPCR-only allocation with no TLS block if
// tls_info is empty - most Xbox 360 titles have no compiler-emitted TLS at
// all), copies tls_info->raw_data_size bytes from tls_info->raw_data_start
// (already an absolute guest virtual address per the XEX/PE TLS directory
// format - see xex_loader.cpp's parse_native_tls/parse_pe_tls_directory) and
// zero-fills the remainder, and writes tls_ptr/self/stack_base/stack_limit
// into the KPCR. Returns the addresses to install into the new thread's
// CpuState::gpr[13] (the KPCR address).
[[nodiscard]] bool setup_guest_thread_tls_context(
    memory::AddressSpace& memory, const std::optional<xbox::XexTls>& tls_info,
    memory::GuestAddress stack_base, std::uint32_t stack_size,
    GuestThreadTlsContext& out_context, std::string* error = nullptr);

// Releases the allocations setup_guest_thread_tls_context() made. Safe to
// call on a default-constructed (never-allocated) context.
void release_guest_thread_tls_context(memory::AddressSpace& memory,
                                      const GuestThreadTlsContext& context) noexcept;

}  // namespace xenon::core
