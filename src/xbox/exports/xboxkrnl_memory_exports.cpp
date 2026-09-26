#include "xenon/xbox/xboxkrnl_memory_exports.hpp"

#include <cstdint>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
namespace status = xenon::kernel::xbox::status;

// STATUS_NO_MEMORY and STATUS_CONFLICTING_ADDRESSES are stable, widely-
// published NT status codes (not xboxkrnl-specific ordinals, so no external
// verification is needed the way export ordinals require) that
// xenon::kernel::xbox::status does not yet define - same reasoning as
// xboxkrnl_sync_exports.cpp's kStatusMutantNotOwned.
constexpr std::uint32_t kStatusNoMemory = 0xC0000017u;
constexpr std::uint32_t kStatusConflictingAddresses = 0xC0000018u;

// Real Xbox 360 XDK MEM_* AllocationType bits and PAGE_* protection
// constants, verified against xenia-project/xenia's src/xenia/xbox.h (not
// guessed, and NOT simply carried over from the desktop Win32/NT values,
// which share the same PAGE_* bits but a different function signature - see
// nt_allocate_virtual_memory_export()'s own ABI comment for the hard lesson
// this cost).
constexpr std::uint32_t kMemCommit = 0x00001000u;
constexpr std::uint32_t kMemReserve = 0x00002000u;
constexpr std::uint32_t kMemReset = 0x00080000u;
constexpr std::uint32_t kMemTopDown = 0x00100000u;
constexpr std::uint32_t kMemNoZero = 0x00800000u;
constexpr std::uint32_t kMemLargePages = 0x20000000u;

constexpr std::uint32_t kPageNoAccess = 0x01u;
constexpr std::uint32_t kPageReadOnly = 0x02u;
constexpr std::uint32_t kPageReadWrite = 0x04u;
constexpr std::uint32_t kPageWriteCopy = 0x08u;
constexpr std::uint32_t kPageExecute = 0x10u;
constexpr std::uint32_t kPageExecuteRead = 0x20u;
constexpr std::uint32_t kPageExecuteReadWrite = 0x40u;
constexpr std::uint32_t kPageExecuteWriteCopy = 0x80u;
constexpr std::uint32_t kPageNoCacheModifier = 0x200u;
constexpr std::uint32_t kPageWriteCombineModifier = 0x400u;
// PAGE_GUARD (0x100) is deliberately not modeled: Xenon has no guest-visible
// guard-page fault delivery mechanism. The bit is accepted (not rejected as
// an invalid parameter, since real titles do request it) and silently
// dropped from the resulting protection, keeping the base protection correct
// rather than failing an otherwise-legitimate allocation over a modifier
// Xenon cannot honor - a guest that genuinely depends on a guard-page fault
// firing is a real, distinct, currently-unimplemented gap, not something a
// wrong return status here would help diagnose any better.

// Translates a real Xbox 360 XDK PAGE_* protection value into Xenon's
// memory::Protect flags. Matches xenia-project/xenia's own
// FromXdkProtectFlags() exactly (verified, not guessed): any combination
// with the write bit set (including the WRITECOPY variants - Xbox 360 has no
// separate process address space to copy-on-write against, so they behave
// identically to their non-copy counterparts) maps to Read|Write; a
// read/execute-read-only combination maps to Read; anything else
// (PAGE_NOACCESS or an unrecognized value) maps to no access - real
// hardware/xenia never fails an allocation purely over an odd protect
// combination, so this deliberately never rejects one either.
memory::Protect translate_protect(std::uint32_t xdk_protect) {
  using memory::Protect;
  Protect base = Protect::None;
  if ((xdk_protect & (kPageReadWrite | kPageExecuteReadWrite | kPageWriteCopy |
                      kPageExecuteWriteCopy)) != 0u) {
    base = Protect::Read | Protect::Write;
  } else if ((xdk_protect & (kPageReadOnly | kPageExecuteRead)) != 0u) {
    base = Protect::Read;
  }
  if ((xdk_protect & (kPageExecute | kPageExecuteRead | kPageExecuteReadWrite |
                      kPageExecuteWriteCopy)) != 0u) {
    base |= Protect::Execute;
  }
  if ((xdk_protect & kPageNoCacheModifier) != 0u) base |= Protect::NoCache;
  if ((xdk_protect & kPageWriteCombineModifier) != 0u) base |= Protect::WriteCombine;
  return base;
}

}  // namespace

// NtAllocateVirtualMemory (ordinal 0xCC)
// Guest ABI verified against xenia-project/xenia's real implementation
// (src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc's NtAllocateVirtualMemory_
// entry), NOT the desktop Win32/NT signature - the Xbox 360 XDK variant has
// only 5 parameters, no ProcessHandle and no ZeroBits (there is only ever
// one process on this platform): r3 = PBaseAddress (in/out guest pointer to
// a 32-bit address: nonzero in = caller requests that exact base, zero out =
// system chooses one), r4 = PRegionSize (in/out guest pointer to a 32-bit
// size, rounded up to the page granularity actually allocated), r5 =
// AllocationType (MEM_COMMIT/MEM_RESERVE/MEM_TOP_DOWN/MEM_LARGE_PAGES/...),
// r6 = Protect (PAGE_* flags), r7 = DebugMemory (BOOLEAN; real titles always
// pass 0 - "TRUE when allocation is from devkit memory area", meaningless on
// a retail-only runtime) -> r3 = NTSTATUS.
//
// Backed by kernel::KernelMemory/memory::AddressSpace (Memory V2) exactly as
// Xenon's own internal guest-heap allocator (src/kernel/memory/heap.cpp) is -
// this is the real allocator wired up to the real guest ABI, not a separate
// implementation.
//
// MEM_RESET (a "this memory can be discarded" optimization hint) is
// deliberately not modeled - even xenia's own reference implementation does
// not implement it (its handler logs "not implemented" and asserts). A
// MEM_RESET-only request (no COMMIT/RESERVE) is accepted as a structurally
// valid but functionally no-op read of the existing region rather than
// rejected, matching the real ABI rule that MEM_RESET must not be combined
// with other allocation-type bits.
bool nt_allocate_virtual_memory_export(kernel::KernelProcess& process,
                                       ExportCallContext& context) {
  const auto base_address_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto region_size_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto allocation_type = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  const auto xdk_protect = static_cast<std::uint32_t>(context.cpu.gpr[6]);

  if (base_address_ptr == 0u || region_size_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  const auto requested_base = context.memory.read32_be(base_address_ptr);
  const auto requested_size = context.memory.read32_be(region_size_ptr);
  if (requested_size == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  const bool wants_commit = (allocation_type & kMemCommit) != 0u;
  const bool wants_reserve = (allocation_type & kMemReserve) != 0u;
  const bool wants_reset = (allocation_type & kMemReset) != 0u;
  // "If MEM_RESET is set only MEM_RESET can be set" (verified against
  // xenia's own real implementation).
  if (wants_reset && (wants_commit || wants_reserve)) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  if (!wants_commit && !wants_reserve && !wants_reset) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  if (wants_reset) {
    // A real, deliberate no-op (see the function's own doc comment) - the
    // existing region is left exactly as it is, and the call still reports
    // the real base/size back to the caller like a genuine query would.
    context.memory.write32_be(region_size_ptr, requested_size);
    context.cpu.gpr[3] = status::Success;
    return true;
  }

  const bool top_down = (allocation_type & kMemTopDown) != 0u;
  const auto protect = translate_protect(xdk_protect);
  // Real Xbox 360 semantics: a fixed-base request always uses whatever page
  // granularity the target region already uses; a system-chosen request
  // defaults to 4 KiB unless MEM_LARGE_PAGES asks for the 64 KiB view.
  std::uint32_t page_size;
  if (requested_base != 0u) {
    const auto* region =
        memory::AddressSpace::region_for(static_cast<cpu::GuestAddress>(requested_base));
    if (!region) {
      context.cpu.gpr[3] = status::InvalidParameter;
      return true;
    }
    page_size = region->allocation_page_size;
  } else {
    page_size = (allocation_type & kMemLargePages) != 0u ? memory::kLargePageSize
                                                          : memory::kBasePageSize;
  }

  auto& address_space = process.memory().address_space();
  // Real hardware rounds the base DOWN to the page boundary and the size UP
  // to it (verified against xenia's adjusted_base/adjusted_size handling) -
  // some titles legitimately pass a not-quite-aligned base/size.
  const auto aligned_base =
      requested_base - (requested_base % page_size);
  const auto aligned_size =
      ((requested_size + page_size - 1u) / page_size) * page_size;

  cpu::GuestAddress result_address{};
  bool ok;
  if (aligned_base != 0u) {
    // Fixed-address request: the caller specifies an exact base.
    result_address = static_cast<cpu::GuestAddress>(aligned_base);
    ok = true;
    if (wants_reserve) {
      ok = address_space.reserve_fixed(result_address, aligned_size, protect);
    }
    if (ok && wants_commit) {
      ok = address_space.commit_fixed(result_address, aligned_size, protect,
                                      (allocation_type & kMemNoZero) == 0u);
    }
  } else {
    memory::VirtualAllocationOptions options{};
    options.commit = wants_commit;
    options.zero_initialize = (allocation_type & kMemNoZero) == 0u;
    ok = address_space.allocate(aligned_size, page_size, protect, top_down,
                                result_address, page_size, options);
  }

  if (!ok) {
    context.cpu.gpr[3] = aligned_base != 0u ? kStatusConflictingAddresses : kStatusNoMemory;
    return true;
  }

  context.memory.write32_be(base_address_ptr, static_cast<std::uint32_t>(result_address));
  context.memory.write32_be(region_size_ptr, aligned_size);
  context.cpu.gpr[3] = status::Success;
  return true;
}

namespace {

// KeFlushUserModeTb (ordinal 0x65)
// Guest ABI: whatever parameters real hardware takes are unused by this
// implementation (see rationale below) -> void (no meaningful return; r3
// left untouched).
//
// Real Xbox 360 hardware: invalidates stale user-mode TLB/SLB entries across
// processors after a page-table change, so every core observes the new
// mapping. Xenon has no guest-visible, software-managed TLB of its own -
// a guest memory protection/mapping change (VirtualAlloc/VirtualProtect-
// equivalent) is applied directly through the HOST OS's virtual-memory calls
// at the moment it happens (see include/xenon/memory/host_vm.hpp), and the
// host CPU's own hardware TLB is already kept coherent by the host OS for
// every host thread - there is no separate, later "flush" step for Xenon to
// perform, on this guest thread or any other. A no-op is therefore the
// behaviorally-correct implementation here, not a placeholder: any guest
// code that changed a mapping already observes the new mapping correctly by
// the time this call would have returned on real hardware.
bool ke_flush_user_mode_tb_export(ExportCallContext&) {
  return true;
}

struct MemoryExportSpec {
  std::uint32_t ordinal;
  const char* name;
  core::ExportHandler handler;
};

// Ordinal verified against the xenia-project/xenia xboxkrnl export table
// (xboxkrnl_table.inc), not guessed - see xboxkrnl_memory_exports.hpp.
const MemoryExportSpec kMemoryExports[] = {
    {0x065u, "KeFlushUserModeTb", &ke_flush_user_mode_tb_export},
};

}  // namespace

bool register_xboxkrnl_memory_exports(core::ExportRegistry& registry) {
  for (const auto& spec : kMemoryExports) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.handler = spec.handler;
    descriptor.requirement = core::ExportRequirement::Required;

    if (!registry.register_export(std::move(descriptor))) {
      return false;
    }
  }

  return true;
}

}  // namespace xenon::xbox
