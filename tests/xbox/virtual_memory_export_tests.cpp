// xboxkrnl virtual-memory and kernel-memory exports: NtFreeVirtualMemory,
// NtQueryVirtualMemory, NtProtectVirtualMemory, MmAllocatePhysicalMemory,
// MmMapIoSpace, MmIsAddressValid, MmCreateKernelStack/MmDeleteKernelStack,
// KeGetImagePageTableEntry, KeLockL2/KeUnlockL2 and the encrypted-memory pair.
//
// Every test drives the export through core::ExportRegistry::invoke() with the
// real ordinal and a real Memory V2 address space, exactly as a guest thunk
// does, and checks Xbox-visible behavior (guest-memory results and NTSTATUS),
// not host-side implementation details. Real-world context: NtFreeVirtualMemory
// and NtQueryVirtualMemory are imports of Ace Combat 6 that previously had no
// registration at all.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_memory_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kStatusSuccess = 0x00000000u;
constexpr std::uint32_t kStatusUnsuccessful = 0xC0000001u;
constexpr std::uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr std::uint32_t kStatusAccessDenied = 0xC0000022u;
constexpr std::uint32_t kStatusMemoryNotAllocated = 0xC00000A0u;
constexpr std::uint32_t kStatusInvalidPageProtection = 0xC0000045u;

constexpr std::uint32_t kMemCommit = 0x1000u;
constexpr std::uint32_t kMemReserve = 0x2000u;
constexpr std::uint32_t kMemDecommit = 0x4000u;
constexpr std::uint32_t kMemRelease = 0x8000u;
constexpr std::uint32_t kMemFree = 0x10000u;
constexpr std::uint32_t kMemPrivate = 0x20000u;
constexpr std::uint32_t kPageNoAccess = 0x01u;
constexpr std::uint32_t kPageReadOnly = 0x02u;
constexpr std::uint32_t kPageReadWrite = 0x04u;
constexpr std::uint32_t kPageExecuteReadWrite = 0x40u;

constexpr std::uint32_t kOrdNtAllocate = 0x0CCu;
constexpr std::uint32_t kOrdNtFree = 0x0DCu;
constexpr std::uint32_t kOrdNtQuery = 0x0EEu;
constexpr std::uint32_t kOrdNtProtect = 0x0E1u;
constexpr std::uint32_t kOrdMmAllocatePhysicalEx = 0x0BAu;
constexpr std::uint32_t kOrdMmAllocatePhysical = 0x0B9u;
constexpr std::uint32_t kOrdMmMapIoSpace = 0x0C2u;
constexpr std::uint32_t kOrdMmIsAddressValid = 0x0BFu;
constexpr std::uint32_t kOrdMmCreateKernelStack = 0x0BBu;
constexpr std::uint32_t kOrdMmDeleteKernelStack = 0x0BCu;
constexpr std::uint32_t kOrdKeGetImagePageTableEntry = 0x2A4u;
constexpr std::uint32_t kOrdKeLockL2 = 0x06Bu;
constexpr std::uint32_t kOrdKeUnlockL2 = 0x06Cu;
constexpr std::uint32_t kOrdNtAllocateEncrypted = 0x28Au;
constexpr std::uint32_t kOrdNtFreeEncrypted = 0x28Bu;

struct MemoryBasicInformation {
  std::uint32_t base_address, allocation_base, allocation_protect, region_size, state, protect,
      type;
};

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;

  using Handler = bool (*)(kernel::KernelProcess&, core::ExportCallContext&);

  void add(std::uint32_t ordinal, const char* name, Handler handler) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = name;
    descriptor.ordinal = ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    kernel::KernelProcess* raw = process.get();
    descriptor.handler = [raw, handler](core::ExportCallContext& ctx) {
      return handler(*raw, ctx);
    };
    const bool registered = registry.register_export(std::move(descriptor));
    assert(registered);
    static_cast<void>(registered);
  }

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    const bool ok = address_space->initialize();
    assert(ok);
    static_cast<void>(ok);
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    add(kOrdNtAllocate, "NtAllocateVirtualMemory", &xbox::nt_allocate_virtual_memory_export);
    add(kOrdNtFree, "NtFreeVirtualMemory", &xbox::nt_free_virtual_memory_export);
    add(kOrdNtQuery, "NtQueryVirtualMemory", &xbox::nt_query_virtual_memory_export);
    add(kOrdNtProtect, "NtProtectVirtualMemory", &xbox::nt_protect_virtual_memory_export);
    add(kOrdMmAllocatePhysicalEx, "MmAllocatePhysicalMemoryEx",
        &xbox::mm_allocate_physical_memory_ex_export);
    add(kOrdMmAllocatePhysical, "MmAllocatePhysicalMemory",
        &xbox::mm_allocate_physical_memory_export);
    add(kOrdMmMapIoSpace, "MmMapIoSpace", &xbox::mm_map_io_space_export);
    add(kOrdMmIsAddressValid, "MmIsAddressValid", &xbox::mm_is_address_valid_export);
    add(kOrdMmCreateKernelStack, "MmCreateKernelStack", &xbox::mm_create_kernel_stack_export);
    add(kOrdMmDeleteKernelStack, "MmDeleteKernelStack", &xbox::mm_delete_kernel_stack_export);
    add(kOrdKeGetImagePageTableEntry, "KeGetImagePageTableEntry",
        &xbox::ke_get_image_page_table_entry_export);
    add(kOrdNtAllocateEncrypted, "NtAllocateEncryptedMemory",
        &xbox::nt_allocate_encrypted_memory_export);
    add(kOrdNtFreeEncrypted, "NtFreeEncryptedMemory", &xbox::nt_free_encrypted_memory_export);
    // KeLockL2/KeUnlockL2 need no process: register the production table.
    const bool memory_registered = xbox::register_xboxkrnl_memory_exports(registry);
    assert(memory_registered);
    static_cast<void>(memory_registered);
  }

  // Invokes `ordinal` with r3..r8 = args and returns r3 (NTSTATUS / value).
  std::uint64_t call(std::uint32_t ordinal, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                     std::uint64_t r5 = 0, std::uint64_t r6 = 0, std::uint64_t r7 = 0,
                     std::uint64_t r8 = 0) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    cpu.gpr[6] = r6;
    cpu.gpr[7] = r7;
    cpu.gpr[8] = r8;
    core::ExportCallContext ctx{cpu, *address_space, 0, 0};
    const auto result = registry.invoke("xboxkrnl", ordinal, ctx);
    assert(result.handled && result.success);
    return cpu.gpr[3];
  }

  // Scratch guest memory for in/out parameters comes from one dedicated arena
  // allocated up front and never freed, so a region a test releases can never
  // be handed back out as a later parameter buffer (which would make a
  // "the freed range is now MEM_FREE" check observe the buffer instead).
  memory::GuestAddress scratch(std::uint32_t bytes = 64) {
    if (arena_base == 0u) {
      const bool ok = address_space->allocate(kArenaSize, 0x1000u, memory::kReadWrite, true,
                                              arena_base);
      assert(ok);
      static_cast<void>(ok);
    }
    const auto aligned = (bytes + 15u) & ~15u;
    assert(arena_used + aligned <= kArenaSize);
    const auto addr = arena_base + arena_used;
    arena_used += aligned;
    return addr;
  }

  static constexpr std::uint32_t kArenaSize = 0x10000u;
  memory::GuestAddress arena_base{};
  std::uint32_t arena_used{};

  // NtAllocateVirtualMemory(base=0, size, type, protect) -> allocation base.
  std::uint32_t alloc(std::uint32_t size, std::uint32_t type, std::uint32_t protect,
                      std::uint32_t* actual_size = nullptr) {
    const auto base_ptr = scratch();
    const auto size_ptr = scratch();
    address_space->write32_be(base_ptr, 0u);
    address_space->write32_be(size_ptr, size);
    const auto status = call(kOrdNtAllocate, base_ptr, size_ptr, type, protect, 0u);
    assert(status == kStatusSuccess);
    static_cast<void>(status);
    if (actual_size) *actual_size = address_space->read32_be(size_ptr);
    return address_space->read32_be(base_ptr);
  }

  std::uint64_t free_region(std::uint32_t base, std::uint32_t size, std::uint32_t type,
                            std::uint32_t* out_base = nullptr, std::uint32_t* out_size = nullptr) {
    const auto base_ptr = scratch();
    const auto size_ptr = scratch();
    address_space->write32_be(base_ptr, base);
    address_space->write32_be(size_ptr, size);
    const auto status = call(kOrdNtFree, base_ptr, size_ptr, type, 0u);
    if (out_base) *out_base = address_space->read32_be(base_ptr);
    if (out_size) *out_size = address_space->read32_be(size_ptr);
    return status;
  }

  std::uint64_t query(std::uint32_t address, MemoryBasicInformation& info) {
    const auto ptr = scratch(28);
    const auto status = call(kOrdNtQuery, address, ptr);
    if (status == kStatusSuccess) {
      info.base_address = address_space->read32_be(ptr + 0u);
      info.allocation_base = address_space->read32_be(ptr + 4u);
      info.allocation_protect = address_space->read32_be(ptr + 8u);
      info.region_size = address_space->read32_be(ptr + 12u);
      info.state = address_space->read32_be(ptr + 16u);
      info.protect = address_space->read32_be(ptr + 20u);
      info.type = address_space->read32_be(ptr + 24u);
    }
    return status;
  }
};

// MEM_RELEASE frees the whole allocation, reports its real size, and the
// address range then queries as MEM_FREE and can be re-released only once.
void test_free_release_whole_allocation() {
  Fixture f;
  std::uint32_t size = 0;
  const auto base = f.alloc(0x3000u, kMemCommit | kMemReserve, kPageReadWrite, &size);
  assert(base != 0u && size >= 0x3000u);
  f.address_space->write32_be(base, 0x11223344u);

  std::uint32_t out_base = 0, out_size = 0;
  assert(f.free_region(base, 0u, kMemRelease, &out_base, &out_size) == kStatusSuccess);
  assert(out_base == base);
  assert(out_size == size && "MEM_RELEASE must report the whole allocation size");

  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess);
  assert(info.state == kMemFree && info.allocation_base == 0u);
  assert(info.protect == kPageNoAccess && info.type == 0u);

  assert(f.free_region(base, 0u, kMemRelease) == kStatusUnsuccessful &&
         "a second release of the same allocation must fail");
}

// A caller-supplied non-zero RegionSize on MEM_RELEASE is tolerated (xenia and
// rexglue both accept it) and the real allocation size is still written back.
void test_free_release_ignores_caller_region_size() {
  Fixture f;
  std::uint32_t size = 0;
  const auto base = f.alloc(0x2000u, kMemCommit | kMemReserve, kPageReadWrite, &size);
  std::uint32_t out_size = 0;
  assert(f.free_region(base, 0x10u, kMemRelease, nullptr, &out_size) == kStatusSuccess);
  assert(out_size == size);
}

// MEM_RELEASE must name the allocation base: an interior address fails and the
// allocation stays intact.
void test_free_release_requires_allocation_base() {
  Fixture f;
  const auto base = f.alloc(0x4000u, kMemCommit | kMemReserve, kPageReadWrite);
  assert(f.free_region(base + 0x1000u, 0u, kMemRelease) == kStatusUnsuccessful);
  f.address_space->write32_be(base, 0xAABBCCDDu);
  assert(f.address_space->read32_be(base) == 0xAABBCCDDu);
  assert(f.free_region(base, 0u, kMemRelease) == kStatusSuccess);
}

// MEM_DECOMMIT of a middle page leaves the neighbours committed and turns the
// decommitted page into reserved-only address space (State MEM_RESERVE,
// Protect 0), and the range is widened to page granularity.
void test_free_decommit_middle_page() {
  Fixture f;
  const auto base = f.alloc(0x4000u, kMemCommit | kMemReserve, kPageReadWrite);
  f.address_space->write32_be(base, 1u);
  f.address_space->write32_be(base + 0x3000u, 2u);

  std::uint32_t out_base = 0, out_size = 0;
  // Unaligned request inside page 1: [base+0x1100, base+0x1100+0x100)
  assert(f.free_region(base + 0x1100u, 0x100u, kMemDecommit, &out_base, &out_size) ==
         kStatusSuccess);
  assert(out_base == base + 0x1000u && out_size == 0x1000u);

  MemoryBasicInformation info{};
  assert(f.query(base + 0x1000u, info) == kStatusSuccess);
  assert(info.state == kMemReserve && info.protect == 0u);
  assert(info.allocation_base == base);
  assert(f.query(base, info) == kStatusSuccess && info.state == kMemCommit);
  assert(f.query(base + 0x3000u, info) == kStatusSuccess && info.state == kMemCommit);
  assert(f.address_space->read32_be(base) == 1u);
  assert(f.address_space->read32_be(base + 0x3000u) == 2u);
}

// MEM_DECOMMIT with size 0 decommits the whole allocation, but only from its
// base; the allocation stays reserved (releasable afterwards).
void test_free_decommit_zero_size_means_whole_allocation() {
  Fixture f;
  std::uint32_t size = 0;
  const auto base = f.alloc(0x2000u, kMemCommit | kMemReserve, kPageReadWrite, &size);
  assert(f.free_region(base + 0x1000u, 0u, kMemDecommit) == kStatusInvalidParameter);
  std::uint32_t out_size = 0;
  assert(f.free_region(base, 0u, kMemDecommit, nullptr, &out_size) == kStatusSuccess);
  assert(out_size == size);
  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess && info.state == kMemReserve);
  assert(f.free_region(base, 0u, kMemRelease) == kStatusSuccess);
}

void test_free_rejects_invalid_requests() {
  Fixture f;
  const auto base = f.alloc(0x1000u, kMemCommit | kMemReserve, kPageReadWrite);
  // Null pointers.
  assert(f.call(kOrdNtFree, 0u, 0u, kMemRelease) == kStatusInvalidParameter);
  // Null base value.
  assert(f.free_region(0u, 0u, kMemRelease) == kStatusMemoryNotAllocated);
  // Neither / both free-type bits.
  assert(f.free_region(base, 0u, 0u) == kStatusInvalidParameter);
  assert(f.free_region(base, 0u, kMemDecommit | kMemRelease) == kStatusInvalidParameter);
  // XEX image heap (0x82000000) is not freeable through NtFreeVirtualMemory.
  assert(f.free_region(0x82000000u, 0u, kMemRelease) == kStatusInvalidParameter);
  // MMIO is a real architectural region but not a freeable virtual heap.
  assert(f.free_region(0xFFFF0000u, 0u, kMemRelease) == kStatusInvalidParameter);
  // A physical-alias address is likewise not freeable through this call.
  assert(f.free_region(0xA0000000u, 0u, kMemRelease) == kStatusInvalidParameter);
  // Free (never allocated) page inside a valid heap.
  assert(f.free_region(0x20000000u, 0u, kMemRelease) == kStatusUnsuccessful);
  // The valid allocation is untouched by all of the above.
  assert(f.free_region(base, 0u, kMemRelease) == kStatusSuccess);
}

// NtQueryVirtualMemory reports the exact MEMORY_BASIC_INFORMATION of a
// committed read/write allocation, including for an unaligned query address.
void test_query_committed_region() {
  Fixture f;
  std::uint32_t size = 0;
  const auto base = f.alloc(0x3000u, kMemCommit | kMemReserve, kPageReadWrite, &size);

  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess);
  assert(info.base_address == base && info.allocation_base == base);
  assert(info.allocation_protect == kPageReadWrite && info.protect == kPageReadWrite);
  assert(info.region_size == size && info.state == kMemCommit && info.type == kMemPrivate);

  // Querying mid-page: BaseAddress is the page base, RegionSize spans from it.
  assert(f.query(base + 0x1234u, info) == kStatusSuccess);
  assert(info.base_address == base + 0x1000u);
  assert(info.allocation_base == base);
  assert(info.region_size == size - 0x1000u);
}

// A reserve-only allocation is MEM_RESERVE with Protect 0 while
// AllocationProtect keeps the requested protection.
void test_query_reserved_region() {
  Fixture f;
  const auto base = f.alloc(0x2000u, kMemReserve, kPageReadWrite);
  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess);
  assert(info.state == kMemReserve && info.protect == 0u);
  assert(info.allocation_protect == kPageReadWrite && info.allocation_base == base);
}

void test_query_rejects_invalid_requests() {
  Fixture f;
  MemoryBasicInformation info{};
  assert(f.call(kOrdNtQuery, 0x40000000u, 0u) == kStatusInvalidParameter &&
         "a NULL MEMORY_BASIC_INFORMATION pointer is invalid");
  // An unallocated address inside a valid heap is a successful MEM_FREE query,
  // not an error (it describes the free run).
  assert(f.query(0x20000000u, info) == kStatusSuccess);
  assert(info.state == kMemFree && info.allocation_base == 0u && info.region_size != 0u);
  // The MMIO window is a real region and is reported as committed read/write.
  assert(f.query(0xFFFF0000u, info) == kStatusSuccess && info.state == kMemCommit);
}

// NtProtectVirtualMemory changes protection for real (RW -> RO is observable
// through NtQueryVirtualMemory), reports the old protection, and rounds the
// range to page granularity.
void test_protect_changes_protection_and_reports_old() {
  Fixture f;
  const auto base = f.alloc(0x2000u, kMemCommit | kMemReserve, kPageReadWrite);
  const auto base_ptr = f.scratch();
  const auto size_ptr = f.scratch();
  const auto old_ptr = f.scratch();
  f.address_space->write32_be(base_ptr, base + 0x10u);
  f.address_space->write32_be(size_ptr, 0x20u);
  assert(f.call(kOrdNtProtect, base_ptr, size_ptr, kPageReadOnly, old_ptr, 0u) == kStatusSuccess);
  assert(f.address_space->read32_be(base_ptr) == base);
  assert(f.address_space->read32_be(size_ptr) == 0x1000u);
  assert(f.address_space->read32_be(old_ptr) == kPageReadWrite);

  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess);
  assert(info.protect == kPageReadOnly && info.allocation_protect == kPageReadWrite);
  assert(f.query(base + 0x1000u, info) == kStatusSuccess && info.protect == kPageReadWrite);
}

void test_protect_rejects_invalid_requests() {
  Fixture f;
  const auto base = f.alloc(0x1000u, kMemCommit | kMemReserve, kPageReadWrite);
  const auto base_ptr = f.scratch();
  const auto size_ptr = f.scratch();
  f.address_space->write32_be(base_ptr, base);
  f.address_space->write32_be(size_ptr, 0x1000u);
  assert(f.call(kOrdNtProtect, 0u, size_ptr, kPageReadOnly, 0u) == kStatusInvalidParameter);
  assert(f.call(kOrdNtProtect, base_ptr, size_ptr, kPageExecuteReadWrite, 0u) ==
             kStatusAccessDenied &&
         "a title may never make memory executable through NtProtectVirtualMemory");
  assert(f.call(kOrdNtProtect, base_ptr, size_ptr, 0x200u, 0u) == kStatusInvalidPageProtection);
  f.address_space->write32_be(size_ptr, 0u);
  assert(f.call(kOrdNtProtect, base_ptr, size_ptr, kPageReadOnly, 0u) == kStatusInvalidParameter);
  f.address_space->write32_be(size_ptr, 0x1000u);
  f.address_space->write32_be(base_ptr, 0x82000000u);
  assert(f.call(kOrdNtProtect, base_ptr, size_ptr, kPageReadOnly, 0u) == kStatusInvalidParameter);
  // Protection unchanged after all the rejections.
  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess && info.protect == kPageReadWrite);
}

// MmAllocatePhysicalMemory (no Ex) is the unconstrained-range form: a real,
// writable physical alias that MmMapIoSpace maps to itself.
void test_mm_allocate_physical_and_map_io_space() {
  Fixture f;
  const auto alias = static_cast<std::uint32_t>(
      f.call(kOrdMmAllocatePhysical, 0u, 0x10000u, kPageReadWrite));
  assert(alias != 0u);
  f.address_space->write32_be(alias, 0x5A5A5A5Au);
  assert(f.address_space->read32_be(alias) == 0x5A5A5A5Au);

  assert(f.call(kOrdMmMapIoSpace, 2u, alias, 0x40u, 0x404u) == alias);
  assert(f.call(kOrdMmMapIoSpace, 2u, 0x20000000u, 0x40u, 0x404u) == 0u &&
         "mapping an unmapped source must fail with NULL");

  // Zero size and no protect bits are rejected as failure (0).
  assert(f.call(kOrdMmAllocatePhysical, 0u, 0u, kPageReadWrite) == 0u);
  assert(f.call(kOrdMmAllocatePhysical, 0u, 0x1000u, 0u) == 0u);
}

void test_mm_is_address_valid() {
  Fixture f;
  const auto base = f.alloc(0x2000u, kMemCommit | kMemReserve, kPageReadWrite);
  assert(f.call(kOrdMmIsAddressValid, base) == 1u);
  assert(f.call(kOrdMmIsAddressValid, base + 0x1FFCu) == 1u);
  assert(f.call(kOrdMmIsAddressValid, 0x20000000u) == 0u && "free page");
  assert(f.free_region(base + 0x1000u, 0x1000u, kMemDecommit) == kStatusSuccess);
  assert(f.call(kOrdMmIsAddressValid, base + 0x1000u) == 0u && "decommitted page");
  assert(f.call(kOrdMmIsAddressValid, base) == 1u);
}

// MmCreateKernelStack returns the TOP of a real committed stack (allocation
// base + requested size); MmDeleteKernelStack releases it via the low address.
void test_mm_kernel_stack_roundtrip() {
  Fixture f;
  constexpr std::uint32_t kSize = 0x10000u;
  const auto top = static_cast<std::uint32_t>(f.call(kOrdMmCreateKernelStack, kSize, 0u));
  assert(top != 0u);
  const auto low = top - kSize;
  f.address_space->write32_be(low, 0x01020304u);
  f.address_space->write32_be(top - 4u, 0x0A0B0C0Du);
  assert(f.address_space->read32_be(low) == 0x01020304u);
  assert(f.address_space->read32_be(top - 4u) == 0x0A0B0C0Du);
  MemoryBasicInformation info{};
  assert(f.query(low, info) == kStatusSuccess && info.state == kMemCommit &&
         info.protect == kPageReadWrite);

  assert(f.call(kOrdMmDeleteKernelStack, top, low) == kStatusSuccess);
  assert(f.query(low, info) == kStatusSuccess && info.state == kMemFree);
  assert(f.call(kOrdMmDeleteKernelStack, top, low) == kStatusUnsuccessful &&
         "a double delete must fail");
  assert(f.call(kOrdMmCreateKernelStack, 0u, 0u) == 0u);
}

// A 4 KiB-multiple stack size with bits in 0xF000 uses 4 KiB alignment and the
// returned top is base + the *unrounded* requested size.
void test_mm_kernel_stack_unaligned_size_top() {
  Fixture f;
  constexpr std::uint32_t kSize = 0x5800u;
  const auto top = static_cast<std::uint32_t>(f.call(kOrdMmCreateKernelStack, kSize, 0u));
  assert(top != 0u);
  const auto low = top - kSize;
  assert((low & 0xFFFu) == 0u);
  MemoryBasicInformation info{};
  assert(f.query(low, info) == kStatusSuccess && info.allocation_base == low);
  assert(f.call(kOrdMmDeleteKernelStack, top, low) == kStatusSuccess);
}

// KeGetImagePageTableEntry: XEX heap addresses map to their page number within
// the heap (4 KiB heaps set bit 30); anything outside a XEX heap yields 0.
void test_ke_get_image_page_table_entry() {
  Fixture f;
  // 64 KiB XEX heap starting at 0x80000000: 0x82000000 is page 0x200.
  assert(f.call(kOrdKeGetImagePageTableEntry, 0x82000000u) == 0x200u);
  // 4 KiB XEX heap starting at 0x90000000: page 5 with the small-page flag.
  assert(f.call(kOrdKeGetImagePageTableEntry, 0x90005000u) == (0x40000000u | 5u));
  assert(f.call(kOrdKeGetImagePageTableEntry, 0x40000000u) == 0u);
  assert(f.call(kOrdKeGetImagePageTableEntry, 0u) == 0u);
}

// KeLockL2/KeUnlockL2 succeed (return 0) and touch nothing.
void test_ke_lock_l2_is_side_effect_free() {
  Fixture f;
  const auto base = f.alloc(0x1000u, kMemCommit | kMemReserve, kPageReadWrite);
  f.address_space->write32_be(base, 0xFEEDF00Du);
  assert(f.call(kOrdKeLockL2, 1u, 2u, 3u) == 0u);
  assert(f.call(kOrdKeUnlockL2, 1u, 2u, 3u) == 0u);
  assert(f.address_space->read32_be(base) == 0xFEEDF00Du);
}

void test_encrypted_memory_roundtrip() {
  Fixture f;
  const auto ptr = f.scratch();
  assert(f.call(kOrdNtAllocateEncrypted, 0u, 0x12345u, ptr) == kStatusSuccess);
  const auto base = f.address_space->read32_be(ptr);
  assert(base != 0u && (base & 0xFFFFu) == 0u);
  MemoryBasicInformation info{};
  assert(f.query(base, info) == kStatusSuccess && info.state == kMemCommit);
  assert(info.region_size == 0x20000u && "size is rounded up to 64 KiB");
  f.address_space->write32_be(base, 0xC0DEC0DEu);
  assert(f.address_space->read32_be(base) == 0xC0DEC0DEu);

  assert(f.call(kOrdNtFreeEncrypted, 0u, ptr) == kStatusSuccess);
  assert(f.query(base, info) == kStatusSuccess && info.state == kMemFree);
  assert(f.call(kOrdNtFreeEncrypted, 0u, ptr) == kStatusInvalidParameter);

  assert(f.call(kOrdNtAllocateEncrypted, 0u, 0u, ptr) == kStatusInvalidParameter);
  assert(f.call(kOrdNtAllocateEncrypted, 0u, 0x1000001u, ptr) == kStatusInvalidParameter &&
         "more than 16 MiB is rejected");
  assert(f.call(kOrdNtAllocateEncrypted, 0u, 0x1000u, 0u) == kStatusInvalidParameter);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl virtual-memory exports...\n";
  test_free_release_whole_allocation();
  test_free_release_ignores_caller_region_size();
  test_free_release_requires_allocation_base();
  test_free_decommit_middle_page();
  test_free_decommit_zero_size_means_whole_allocation();
  test_free_rejects_invalid_requests();
  test_query_committed_region();
  test_query_reserved_region();
  test_query_rejects_invalid_requests();
  test_protect_changes_protection_and_reports_old();
  test_protect_rejects_invalid_requests();
  test_mm_allocate_physical_and_map_io_space();
  test_mm_is_address_valid();
  test_mm_kernel_stack_roundtrip();
  test_mm_kernel_stack_unaligned_size_top();
  test_ke_get_image_page_table_entry();
  test_ke_lock_l2_is_side_effect_free();
  test_encrypted_memory_roundtrip();
  std::cout << "All xboxkrnl virtual-memory export tests passed!\n";
  return 0;
}
