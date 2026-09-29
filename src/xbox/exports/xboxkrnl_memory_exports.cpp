#include "xenon/xbox/xboxkrnl_memory_exports.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

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
// NtFreeVirtualMemory FreeType bits and the MEMORY_BASIC_INFORMATION State/
// Type values (documented NT ABI, unchanged on the Xbox 360 kernel; the
// values match xenia-project/xenia's src/xenia/xbox.h).
constexpr std::uint32_t kMemDecommit = 0x00004000u;
constexpr std::uint32_t kMemRelease = 0x00008000u;
constexpr std::uint32_t kMemFree = 0x00010000u;
constexpr std::uint32_t kMemPrivate = 0x00020000u;
constexpr std::uint32_t kMemImage = 0x01000000u;
// STATUS_MEMORY_NOT_ALLOCATED / STATUS_INVALID_PAGE_PROTECTION: stable NT
// status codes not yet in xenon::kernel::xbox::status.
constexpr std::uint32_t kStatusMemoryNotAllocated = 0xC00000A0u;
constexpr std::uint32_t kStatusInvalidPageProtection = 0xC0000045u;

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

std::uint32_t to_xdk_protect(memory::Protect protect) {
  const bool read = memory::has(protect, memory::Protect::Read);
  const bool write = memory::has(protect, memory::Protect::Write);
  const bool execute = memory::has(protect, memory::Protect::Execute);
  std::uint32_t result;
  if (execute) {
    result = write ? kPageExecuteReadWrite
                   : read ? kPageExecuteRead : kPageExecute;
  } else {
    result = write ? kPageReadWrite : read ? kPageReadOnly : kPageNoAccess;
  }
  if (memory::has(protect, memory::Protect::NoCache)) {
    result |= kPageNoCacheModifier;
  }
  if (memory::has(protect, memory::Protect::WriteCombine)) {
    result |= kPageWriteCombineModifier;
  }
  return result;
}

bool checked_align_up(std::uint32_t value, std::uint32_t alignment,
                      std::uint32_t& result) {
  if (value == 0u || alignment == 0u ||
      (alignment & (alignment - 1u)) != 0u) {
    return false;
  }
  const auto aligned = (std::uint64_t{value} + alignment - 1u) &
                       ~(std::uint64_t{alignment} - 1u);
  if (aligned > UINT32_MAX) return false;
  result = static_cast<std::uint32_t>(aligned);
  return true;
}

bool valid_range(std::uint32_t base, std::uint32_t size) {
  return size != 0u && std::uint64_t{base} + size <= UINT64_C(0x100000000);
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
  std::uint32_t aligned_size{};
  if (!checked_align_up(requested_size, page_size, aligned_size) ||
      !valid_range(aligned_base, aligned_size)) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

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

// MmAllocatePhysicalMemoryEx (ordinal 0xBA)
// Guest ABI verified against xenia-project/xenia's real implementation: r3 =
// flags (unused - real hardware ignores it too), r4 = region size, r5 =
// Protect (PAGE_* flags, plus MEM_LARGE_PAGES/MEM_16MB_PAGES page-size
// selectors OR'd in - a real Xbox 360 XDK quirk, not a bug: this API packs
// page size into the same field NtAllocateVirtualMemory uses only for
// protection), r6 = minimum physical address, r7 = maximum physical address
// (inclusive), r8 = alignment -> r3 = guest-visible base address, or 0 on
// failure (NOT an NTSTATUS - real MmAllocatePhysicalMemoryEx returns a
// pointer-or-null, matching xenia's own reference).
//
// Backed by memory::AddressSpace::allocate_physical()/physical_guest_alias()
// exactly as Xenon's own physical-memory consumers already are (see
// audio/xma.cpp's XMA context-array allocation for the same
// allocate-then-alias pattern) - memory::PhysicalAllocationOptions'
// minimum_address/maximum_address fields exist specifically for this export
// (see their doc comment in memory/types.hpp), so the guest's requested
// physical range is genuinely honored, not silently dropped.
//
// Real AC6 repro: reached during startup with no case registered at all.
bool mm_allocate_physical_memory_ex_export(kernel::KernelProcess& process,
                                           ExportCallContext& context) {
  const auto region_size = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto protect_bits = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  const auto min_addr_range = static_cast<std::uint32_t>(context.cpu.gpr[6]);
  const auto max_addr_range = static_cast<std::uint32_t>(context.cpu.gpr[7]);
  const auto alignment_in = static_cast<std::uint32_t>(context.cpu.gpr[8]);

  if (region_size == 0u || (protect_bits & (kPageReadOnly | kPageReadWrite)) == 0u) {
    context.cpu.gpr[3] = 0u;
    return true;
  }

  memory::PhysicalAllocationOptions options{};
  // Xenia's current Xbox-facing implementation checks LARGE before 16MB;
  // preserve that observable precedence when a malformed caller sets both.
  std::uint32_t page_size;
  if ((protect_bits & kMemLargePages) != 0u) {
    options.page_class = memory::PhysicalPageClass::Page64K;
    page_size = memory::kLargePageSize;
  } else if ((protect_bits & 0x80000000u) != 0u) {
    options.page_class = memory::PhysicalPageClass::Page16M;
    page_size = memory::kHugePageSize;
  } else {
    options.page_class = memory::PhysicalPageClass::Page4K;
    page_size = memory::kBasePageSize;
  }
  if (alignment_in != 0u) {
    if (!checked_align_up(alignment_in, page_size, options.alignment)) {
      context.cpu.gpr[3] = 0u;
      return true;
    }
  } else {
    options.alignment = page_size;
  }
  // An unconstrained guest request (min=0, max=0xFFFFFFFF is the common
  // case) must not narrow the allocator below/above its own real usable
  // physical range - only a genuinely tighter guest-supplied bound should
  // shrink it, matching real hardware clamping the request into what
  // actually exists rather than treating "don't care" as "the whole 32-bit
  // space, including reserved regions".
  options.minimum_address = std::max(min_addr_range, options.minimum_address);
  options.maximum_address = std::min(max_addr_range, options.maximum_address);
  if (options.minimum_address > options.maximum_address) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  options.protect = translate_protect(protect_bits);
  options.top_down = true;
  options.zero_initialize = true;

  std::uint32_t aligned_size{};
  if (!checked_align_up(region_size, page_size, aligned_size)) {
    context.cpu.gpr[3] = 0u;
    return true;
  }

  std::uint32_t physical_base = 0u;
  if (!process.memory().address_space().allocate_physical(aligned_size, options,
                                                           physical_base)) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  const auto alias =
      memory::AddressSpace::physical_guest_alias(physical_base, options.page_class);
  if (!alias) {
    static_cast<void>(
        process.memory().address_space().free_physical(physical_base, aligned_size));
    context.cpu.gpr[3] = 0u;
    return true;
  }

  context.cpu.gpr[3] = static_cast<std::uint64_t>(*alias);
  return true;
}

// MmFreePhysicalMemory (ordinal 0xBD / 189)
// Guest ABI: r3 = type (unused, matches rexglue-sdk's MmFreePhysicalMemory_
// entry, which also ignores it), r4 = base address (the guest-visible
// physical alias address a prior MmAllocatePhysicalMemoryEx/VdPersistDisplay-
// style allocation returned) -> void (no meaningful return).
//
// Real hardware's heap allocator recovers the original allocation's size
// from the address itself (LookupHeap(base_address) walks its own free-list
// bookkeeping). Xenon's memory::AddressSpace::free_physical() instead
// requires the caller to already know the size, and there is currently no
// guest-alias-address -> allocation-size reverse lookup to recover it from
// here. This is a genuine, narrow gap: the call is accepted safely (there is
// no NTSTATUS for a caller to observe failure through either way - a real
// caller cannot tell the difference between "freed" and "accepted, not yet
// reclaimed" from this API), but the host-side physical page is not
// actually released back to the allocator. In practice this is bounded and
// rare (a handful of small blocks at boot, e.g. VdPersistDisplay's pairing
// call above), not a per-frame/per-allocation leak. Memory V2's reverse lookup
// canonicalizes an interior alias to the owning allocation before releasing it.
bool mm_free_physical_memory_export(kernel::KernelProcess& process,
                                    ExportCallContext& context) {
  const auto guest_alias = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  auto& address_space = process.memory().address_space();
  const auto physical = address_space.get_physical_address(guest_alias);
  if (physical == UINT32_MAX) return true;
  const auto allocation = address_space.query_physical_allocation(physical);
  if (!allocation) return true;
  static_cast<void>(address_space.free_physical(allocation->allocation_base,
                                                allocation->allocation_size));
  return true;
}

bool mm_get_physical_address_export(kernel::KernelProcess& process,
                                    ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto physical = process.memory().address_space().get_physical_address(address);
  context.cpu.gpr[3] = physical == UINT32_MAX ? 0u : physical;
  return true;
}

bool mm_query_address_protect_export(kernel::KernelProcess& process,
                                     ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto mapping = process.memory().address_space().query(address);
  context.cpu.gpr[3] =
      (!mapping || mapping->state == memory::PageState::Free)
          ? 0u
          : to_xdk_protect(mapping->current_protect);
  return true;
}

bool mm_query_allocation_size_export(kernel::KernelProcess& process,
                                     ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto& address_space = process.memory().address_space();
  const auto mapping = address_space.query(address);
  if (!mapping || mapping->state == memory::PageState::Free) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  if (mapping->kind == memory::RegionKind::PhysicalAlias ||
      mapping->kind == memory::RegionKind::GpuWriteback) {
    const auto physical = address_space.get_physical_address(address);
    const auto allocation = physical == UINT32_MAX
                                ? std::nullopt
                                : address_space.query_physical_allocation(physical);
    context.cpu.gpr[3] = allocation ? allocation->allocation_size : 0u;
    return true;
  }
  context.cpu.gpr[3] = mapping->allocation_size;
  return true;
}

bool mm_query_statistics_export(kernel::KernelProcess& process,
                                ExportCallContext& context) {
  constexpr std::uint32_t kStructureSize = 104u;
  const auto output = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (output == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  if (context.memory.read32_be(output) != kStructureSize) {
    context.cpu.gpr[3] = status::BufferTooSmall;
    return true;
  }

  const auto stats = process.memory().address_space().memory_statistics();
  for (std::uint32_t offset = 0; offset < kStructureSize; offset += 4u) {
    context.memory.write32_be(output + offset, 0u);
  }
  const auto write = [&](std::uint32_t offset, std::uint32_t value) {
    context.memory.write32_be(output + offset, value);
  };
  const auto title_physical = stats.anonymous_physical_pages +
                              stats.explicit_physical_pages;
  write(0u, kStructureSize);
  write(4u, stats.total_physical_pages);
  write(8u, stats.system_physical_pages);
  write(12u, stats.available_physical_pages);
  write(16u, memory::kGpuWritebackEnd + 1u);
  write(20u, stats.reserved_virtual_pages * memory::kBasePageSize);
  write(24u, title_physical);
  // Exact pool/stack/heap/page-table/cache attribution is not represented by
  // Memory V2. Leave those fields honestly zero rather than inventing detail.
  write(36u, stats.image_pages);
  write(44u, stats.committed_virtual_pages);
  write(68u, stats.system_physical_pages);
  write(100u, stats.total_physical_pages - 1u);
  context.cpu.gpr[3] = status::Success;
  return true;
}

bool mm_set_address_protect_export(kernel::KernelProcess& process,
                                   ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto size = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto protect_bits = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  if (!protect_bits || !valid_range(address, size)) return true;

  auto& address_space = process.memory().address_space();
  const auto mapping = address_space.query(address);
  if (!mapping || mapping->state == memory::PageState::Free) return true;
  const auto protect = translate_protect(protect_bits);
  if (mapping->kind == memory::RegionKind::PhysicalAlias ||
      mapping->kind == memory::RegionKind::GpuWriteback) {
    const auto physical = address_space.get_physical_address(address);
    if (physical != UINT32_MAX) {
      static_cast<void>(address_space.protect_physical(physical, size, protect));
    }
  } else {
    static_cast<void>(address_space.protect(address, size, protect));
  }
  return true;
}

// Rounds [base, base+size) out to whole `page_size` pages. Returns false if the
// rounded range does not fit in the 32-bit guest address space.
static bool page_round_range(std::uint32_t base, std::uint32_t size,
                             std::uint32_t page_size, std::uint32_t& out_base,
                             std::uint32_t& out_size) {
  if (page_size == 0u || (page_size & (page_size - 1u)) != 0u) return false;
  const auto aligned_base = base - (base % page_size);
  const auto end = std::uint64_t{base} + size;
  const auto aligned_end =
      (end + page_size - 1u) & ~(std::uint64_t{page_size} - 1u);
  if (aligned_end > UINT64_C(0x100000000) || aligned_end <= aligned_base) return false;
  out_base = aligned_base;
  out_size = static_cast<std::uint32_t>(aligned_end - aligned_base);
  return true;
}

// NtFreeVirtualMemory (ordinal 0xDC)
// Guest ABI (Xbox 360 XDK variant, verified against xenia's and rexglue-sdk's
// implementations): r3 = PBaseAddress (in/out guest pointer to a 32-bit
// address), r4 = PRegionSize (in/out guest pointer to a 32-bit size), r5 =
// FreeType (exactly one of MEM_DECOMMIT 0x4000 / MEM_RELEASE 0x8000), r6 =
// DebugMemory (devkit memory only; ignored) -> r3 = NTSTATUS.
//
// MEM_RELEASE: the base must be the allocation base returned by the original
// NtAllocateVirtualMemory; the whole allocation is released and its size is
// written back. The caller's input RegionSize is ignored rather than rejected
// when non-zero (both xenia and rexglue accept it, and titles that ship on
// real hardware are tested against a kernel that tolerates it).
// MEM_DECOMMIT: the range is widened to the region's page granularity; a zero
// size decommits the entire allocation and is only valid at its base.
bool nt_free_virtual_memory_export(kernel::KernelProcess& process,
                                   ExportCallContext& context) {
  const auto base_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto size_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto free_type = static_cast<std::uint32_t>(context.cpu.gpr[5]);

  if (base_ptr == 0u || size_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  const auto base = context.memory.read32_be(base_ptr);
  const auto size = context.memory.read32_be(size_ptr);
  if (base == 0u) {
    context.cpu.gpr[3] = kStatusMemoryNotAllocated;
    return true;
  }
  const bool decommit = free_type == kMemDecommit;
  const bool release = free_type == kMemRelease;
  if (!decommit && !release) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  auto& address_space = process.memory().address_space();
  const auto mapping = address_space.query(base);
  // Only guest-virtual heaps are freeable through this call; XEX images,
  // physical aliases and MMIO are owned by their own subsystems.
  if (!mapping || mapping->kind != memory::RegionKind::Virtual) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  if (mapping->state == memory::PageState::Free) {
    context.cpu.gpr[3] = status::Unsuccessful;
    return true;
  }

  if (release) {
    if (mapping->allocation_base != base) {
      context.cpu.gpr[3] = status::Unsuccessful;
      return true;
    }
    const auto allocation_size = mapping->allocation_size;
    if (!address_space.release(base)) {
      context.cpu.gpr[3] = status::Unsuccessful;
      return true;
    }
    context.memory.write32_be(base_ptr, base);
    context.memory.write32_be(size_ptr, allocation_size);
    context.cpu.gpr[3] = status::Success;
    return true;
  }

  std::uint32_t aligned_base = 0u;
  std::uint32_t aligned_size = 0u;
  if (size == 0u) {
    if (mapping->allocation_base != base) {
      context.cpu.gpr[3] = status::InvalidParameter;
      return true;
    }
    aligned_base = base;
    aligned_size = mapping->allocation_size;
  } else if (!page_round_range(base, size, mapping->page_size, aligned_base,
                               aligned_size)) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  if (!address_space.decommit(aligned_base, aligned_size)) {
    context.cpu.gpr[3] = status::Unsuccessful;
    return true;
  }
  context.memory.write32_be(base_ptr, aligned_base);
  context.memory.write32_be(size_ptr, aligned_size);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtQueryVirtualMemory (ordinal 0xEE)
// Guest ABI: r3 = BaseAddress (a VALUE, not a pointer), r4 = guest pointer to a
// MEMORY_BASIC_INFORMATION (seven big-endian u32s: BaseAddress,
// AllocationBase, AllocationProtect, RegionSize, State, Protect, Type) ->
// r3 = NTSTATUS. Verified against xenia's implementation (its comment records
// that Beautiful Katamari queries State before its loading screen).
//
// NT reports Protect as 0 for reserved-but-uncommitted pages and describes a
// free run with State=MEM_FREE, AllocationBase=0 and Protect=PAGE_NOACCESS.
// An address outside every architectural heap is STATUS_INVALID_PARAMETER.
bool nt_query_virtual_memory_export(kernel::KernelProcess& process,
                                    ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto info_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (info_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  const auto mapping = process.memory().address_space().query(address);
  if (!mapping) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }

  const auto page_size = mapping->page_size != 0u ? mapping->page_size : memory::kBasePageSize;
  const auto aligned = address - (address % page_size);
  // AddressSpace::query() measures a committed/reserved virtual run from the
  // start of the containing 4 KiB page, but a free run and a physical alias
  // from the exact queried byte. Normalize both to "bytes from `address`",
  // then extend back to the page base NT reports as BaseAddress.
  const bool run_starts_at_page =
      mapping->state != memory::PageState::Free &&
      (mapping->kind == memory::RegionKind::Virtual || mapping->kind == memory::RegionKind::Xex);
  const auto run_from_address =
      run_starts_at_page ? mapping->region_size - (address % memory::kBasePageSize)
                         : mapping->region_size;
  const auto region_size = run_from_address + (address - aligned);

  std::uint32_t allocation_base = 0u;
  std::uint32_t allocation_protect = 0u;
  std::uint32_t state = kMemFree;
  std::uint32_t protect = kPageNoAccess;
  std::uint32_t type = 0u;
  if (mapping->state != memory::PageState::Free) {
    allocation_base = mapping->allocation_base;
    allocation_protect = to_xdk_protect(mapping->allocation_protect);
    type = mapping->kind == memory::RegionKind::Xex ? kMemImage : kMemPrivate;
    if (mapping->state == memory::PageState::Committed) {
      state = kMemCommit;
      protect = to_xdk_protect(mapping->current_protect);
    } else {
      state = kMemReserve;
      protect = 0u;
    }
  }
  context.memory.write32_be(info_ptr + 0u, aligned);
  context.memory.write32_be(info_ptr + 4u, allocation_base);
  context.memory.write32_be(info_ptr + 8u, allocation_protect);
  context.memory.write32_be(info_ptr + 12u, region_size);
  context.memory.write32_be(info_ptr + 16u, state);
  context.memory.write32_be(info_ptr + 20u, protect);
  context.memory.write32_be(info_ptr + 24u, type);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// NtProtectVirtualMemory (ordinal 0xE1)
// Guest ABI: r3 = PBaseAddress (in/out), r4 = PRegionSize (in/out), r5 =
// NewProtect (PAGE_*), r6 = POldProtect (optional out), r7 = DebugMemory
// (ignored) -> r3 = NTSTATUS. Base is rounded down and size up to the region's
// page granularity and written back. A title may never make memory executable
// through this call (ACCESS_DENIED - verified against xenia); a protect value
// with no PAGE_* base bit is STATUS_INVALID_PAGE_PROTECTION.
bool nt_protect_virtual_memory_export(kernel::KernelProcess& process,
                                      ExportCallContext& context) {
  const auto base_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto size_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto new_protect = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  const auto old_protect_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[6]);
  if (base_ptr == 0u || size_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  const auto base = context.memory.read32_be(base_ptr);
  const auto size = context.memory.read32_be(size_ptr);
  if (size == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  if ((new_protect & (kPageExecute | kPageExecuteRead | kPageExecuteReadWrite |
                      kPageExecuteWriteCopy)) != 0u) {
    context.cpu.gpr[3] = status::AccessDenied;
    return true;
  }
  if ((new_protect & 0xFFu) == 0u) {
    context.cpu.gpr[3] = kStatusInvalidPageProtection;
    return true;
  }

  auto& address_space = process.memory().address_space();
  const auto mapping = address_space.query(base);
  if (!mapping || mapping->kind != memory::RegionKind::Virtual) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  std::uint32_t aligned_base = 0u;
  std::uint32_t aligned_size = 0u;
  if (!page_round_range(base, size, mapping->page_size, aligned_base, aligned_size)) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  memory::Protect old_protect = memory::Protect::None;
  if (!address_space.protect(aligned_base, aligned_size, translate_protect(new_protect),
                             &old_protect)) {
    context.cpu.gpr[3] = status::AccessDenied;
    return true;
  }
  context.memory.write32_be(base_ptr, aligned_base);
  context.memory.write32_be(size_ptr, aligned_size);
  if (old_protect_ptr != 0u) {
    context.memory.write32_be(old_protect_ptr, to_xdk_protect(old_protect));
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

// MmAllocatePhysicalMemory (ordinal 0xB9)
// Guest ABI: r3 = flags, r4 = region size, r5 = protect (with page-size
// selector bits) -> r3 = guest base, or 0 on failure. Real hardware treats it
// as MmAllocatePhysicalMemoryEx with an unconstrained physical range and
// default alignment (verified against xenia/rexglue), so it is implemented by
// forwarding exactly that argument set to the Ex handler.
bool mm_allocate_physical_memory_export(kernel::KernelProcess& process,
                                        ExportCallContext& context) {
  context.cpu.gpr[6] = 0u;
  context.cpu.gpr[7] = 0xFFFFFFFFu;
  context.cpu.gpr[8] = 0u;
  return mm_allocate_physical_memory_ex_export(process, context);
}

// MmMapIoSpace (ordinal 0xC2)
// Guest ABI: r3 = unknown (2 in every observed call), r4 = source (physical
// alias) address, r5 = size, r6 = flags -> r3 = mapped address, or 0 on failure.
// Xenon has no separate kernel-VA I/O window: every physical alias a title can
// legitimately map (the only observed use is an XMA context array from
// MmAllocatePhysicalMemoryEx) is already directly addressable, so the correct
// mapping of an existing, committed alias is that same address. An unmapped
// source is a real failure (NULL), like a failed page-table insertion.
bool mm_map_io_space_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto source = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto mapping = process.memory().address_space().query(source);
  const bool mapped = mapping && mapping->state == memory::PageState::Committed;
  context.cpu.gpr[3] = mapped ? static_cast<std::uint64_t>(source) : 0u;
  return true;
}

// MmIsAddressValid (ordinal 0xBF)
// Guest ABI: r3 = address -> r3 = 1 if the address is backed by committed,
// accessible memory, else 0 (rexglue: page access != no-access).
bool mm_is_address_valid_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto mapping = process.memory().address_space().query(address);
  const bool valid = mapping && mapping->state == memory::PageState::Committed &&
                     mapping->current_protect != memory::Protect::None;
  context.cpu.gpr[3] = valid ? 1u : 0u;
  return true;
}

// MmCreateKernelStack (ordinal 0xBB)
// Guest ABI: r3 = stack size, r4 = reserved (0) -> r3 = the TOP of the new
// stack (allocation base + the requested size, verified against xenia's and
// rexglue's implementations), or 0 on failure. The stack is a committed,
// zeroed read/write allocation whose alignment follows the size: 4 KiB when
// the size has bits in 0xF000, else 64 KiB.
bool mm_create_kernel_stack_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto stack_size = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  if (stack_size == 0u) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  const std::uint64_t rounded = (std::uint64_t{stack_size} + 0xFFFu) & ~std::uint64_t{0xFFFu};
  const std::uint32_t alignment = (stack_size & 0xF000u) != 0u ? memory::kBasePageSize
                                                                : memory::kLargePageSize;
  if (rounded > UINT32_MAX) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  cpu::GuestAddress base{};
  memory::VirtualAllocationOptions options{};
  options.commit = true;
  options.zero_initialize = true;
  if (!process.memory().address_space().allocate(static_cast<std::uint32_t>(rounded), alignment,
                                                 memory::Protect::Read | memory::Protect::Write,
                                                 false, base, alignment, options)) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  context.cpu.gpr[3] = static_cast<std::uint64_t>(base) + stack_size;
  return true;
}

// MmDeleteKernelStack (ordinal 0xBC)
// Guest ABI: r3 = stack base (top), r4 = stack end (the LOW address, which is
// the allocation base) -> r3 = NTSTATUS. Releases the allocation
// MmCreateKernelStack made.
bool mm_delete_kernel_stack_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto stack_end = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  context.cpu.gpr[3] = process.memory().address_space().release(stack_end)
                           ? status::Success
                           : status::Unsuccessful;
  return true;
}

// KeGetImagePageTableEntry (ordinal 0x2A4)
// Guest ABI: r3 = address -> r3 = the page-table-entry index for an address in
// a XEX image heap, else 0 (verified against rexglue). Index = page number
// within the heap; 4 KiB heaps set bit 30 to mark the small-page format.
bool ke_get_image_page_table_entry_export(kernel::KernelProcess&, ExportCallContext& context) {
  const auto address = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto* region = memory::AddressSpace::region_for(address);
  if (region == nullptr || region->kind != memory::RegionKind::Xex ||
      region->allocation_page_size == 0u) {
    context.cpu.gpr[3] = 0u;
    return true;
  }
  auto result = (address - region->base) / region->allocation_page_size;
  if (region->allocation_page_size < memory::kLargePageSize) result |= 0x40000000u;
  context.cpu.gpr[3] = result & 0x400FFFFFu;
  return true;
}

// NtAllocateEncryptedMemory (ordinal 0x28A) / NtFreeEncryptedMemory (0x28B)
// Guest ABI: r3 = reserved, r4 = region size, r5 = guest pointer receiving the
// base -> r3 = NTSTATUS. Encrypted memory is memory the console's memory
// controller transparently encrypts; the guest cannot observe the
// encryption, so the observable contract is a 64 KiB-aligned, committed,
// zeroed read/write block of at most 16 MiB.
bool nt_allocate_encrypted_memory_export(kernel::KernelProcess& process,
                                         ExportCallContext& context) {
  const auto region_size = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto base_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  std::uint32_t aligned{};
  if (region_size == 0u || base_ptr == 0u ||
      !checked_align_up(region_size, memory::kLargePageSize, aligned) ||
      aligned > 16u * 1024u * 1024u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  cpu::GuestAddress base{};
  memory::VirtualAllocationOptions options{};
  if (!process.memory().address_space().allocate(aligned, memory::kLargePageSize,
                                                 memory::Protect::Read | memory::Protect::Write,
                                                 false, base, memory::kLargePageSize, options)) {
    context.cpu.gpr[3] = status::Unsuccessful;
    return true;
  }
  context.memory.write32_be(base_ptr, base);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// Guest ABI: r3 = reserved, r4 = guest pointer to the base returned by
// NtAllocateEncryptedMemory -> r3 = NTSTATUS.
bool nt_free_encrypted_memory_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto base_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (base_ptr == 0u) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
  const auto base = context.memory.read32_be(base_ptr);
  auto& address_space = process.memory().address_space();
  const auto mapping = address_space.query(base);
  if (!mapping || mapping->state == memory::PageState::Free ||
      mapping->allocation_base != base || !address_space.release(base)) {
    context.cpu.gpr[3] = status::InvalidParameter;
    return true;
  }
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
// KeLockL2 (0x6B) / KeUnlockL2 (0x6C)
// Real hardware pins/unpins a physical range in the Xenon CPU's L2 cache as a
// pure performance hint (the title-visible memory contents are identical
// whether or not the lines are locked; xenia's and rexglue's implementations
// are likewise no-ops that return 0). Xenon runs on a host CPU cache it cannot
// and need not pin, so returning 0 (success) without side effects is the
// behaviorally-correct implementation, not a placeholder - exactly the
// argument KeFlushUserModeTb makes above.
bool ke_lock_l2_export(ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

const MemoryExportSpec kMemoryExports[] = {
    {0x065u, "KeFlushUserModeTb", &ke_flush_user_mode_tb_export},
    {0x06Bu, "KeLockL2", &ke_lock_l2_export},
    {0x06Cu, "KeUnlockL2", &ke_lock_l2_export},
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
