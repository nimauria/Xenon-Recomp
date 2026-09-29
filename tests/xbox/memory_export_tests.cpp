// xboxkrnl guest memory-management exports (KeFlushUserModeTb,
// NtAllocateVirtualMemory). Drives each export through
// core::ExportRegistry::invoke() exactly as a guest thunk would (real
// ordinal, real ExportCallContext), not by calling the handler C++ function
// directly.
//
// Real-world context: KeFlushUserModeTb is what AC6's real xboxkrnl.exe
// ordinal 101 (0x65) call at guest address 0x823D03AC resolves to - the
// first kernel export the real Ace Combat 6 title actually calls during
// boot that previously had no ExportRegistry registration at all.
// NtAllocateVirtualMemory (ordinal 204 / 0xCC) is the very next real export
// AC6's boot path calls once KeFlushUserModeTb and RtlImageXexHeaderField
// were both fixed (real guest address 0x823d037c) - both are
// correctly-recognized-but-previously-unimplemented imports, per
// src/core/session.cpp's Trap path (STATUS_PROCEDURE_NOT_FOUND).

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

constexpr std::uint32_t kMemCommit = 0x00001000u;
constexpr std::uint32_t kMemReserve = 0x00002000u;
constexpr std::uint32_t kMemReset = 0x00080000u;
constexpr std::uint32_t kPageReadWrite = 0x04u;
constexpr std::uint32_t kPageNoAccess = 0x01u;
constexpr std::uint32_t kStatusSuccess = 0x00000000u;
constexpr std::uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr std::uint32_t kStatusBufferTooSmall = 0xC0000023u;

struct NtAllocFixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;

  NtAllocFixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);

    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "NtAllocateVirtualMemory";
    descriptor.ordinal = 0x0CCu;
    descriptor.requirement = core::ExportRequirement::Required;
    kernel::KernelProcess* raw_process = process.get();
    descriptor.handler = [raw_process](core::ExportCallContext& ctx) {
      return xbox::nt_allocate_virtual_memory_export(*raw_process, ctx);
    };
    assert(registry.register_export(std::move(descriptor)));
  }

  [[nodiscard]] core::ExportCallResult invoke(cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, 0};
    return registry.invoke("xboxkrnl", 0x0CCu, call);
  }

  [[nodiscard]] memory::GuestAddress alloc32() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(4, 4, memory::kReadWrite, false, addr));
    return addr;
  }
};

// NtAllocateVirtualMemory with a NULL requested base (system chooses the
// address) and MEM_COMMIT|MEM_RESERVE must succeed, write back a real,
// page-aligned base address and rounded-up size, and the returned region
// must actually be writable guest memory (not merely "reported success").
void test_nt_allocate_virtual_memory_system_chosen_address() {
  NtAllocFixture fixture;

  const auto base_ptr = fixture.alloc32();
  const auto size_ptr = fixture.alloc32();
  fixture.address_space->write32_be(base_ptr, 0u);       // system chooses
  fixture.address_space->write32_be(size_ptr, 0x1000u);  // 4 KiB requested

  cpu::CpuState cpu{};
  cpu.gpr[3] = base_ptr;
  cpu.gpr[4] = size_ptr;
  cpu.gpr[5] = kMemCommit | kMemReserve;
  cpu.gpr[6] = kPageReadWrite;
  cpu.gpr[7] = 0u;  // DebugMemory (real titles always pass 0)

  const auto result = fixture.invoke(cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == kStatusSuccess);

  const auto returned_base = fixture.address_space->read32_be(base_ptr);
  const auto returned_size = fixture.address_space->read32_be(size_ptr);
  assert(returned_base != 0u && "system-chosen allocation must report a real base address");
  assert(returned_size >= 0x1000u && "returned size must never be smaller than requested");

  // The allocation must be real, committed, writable memory - not just a
  // reported success with no actual backing.
  fixture.address_space->write32_be(returned_base, 0xCAFEBABEu);
  assert(fixture.address_space->read32_be(returned_base) == 0xCAFEBABEu);
}

// MEM_RESET alone (no COMMIT/RESERVE) on an existing, already-allocated
// region is a real, valid discardable-memory hint this project deliberately
// does not implement as an optimization (see the function's own doc comment
// - even xenia's reference implementation does not) - it must still report
// success and the real existing size back, not fail or corrupt the region.
void test_nt_allocate_virtual_memory_reset_is_a_real_noop() {
  NtAllocFixture fixture;

  const auto base_ptr = fixture.alloc32();
  const auto size_ptr = fixture.alloc32();
  fixture.address_space->write32_be(base_ptr, 0u);
  fixture.address_space->write32_be(size_ptr, 0x1000u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = base_ptr;
  cpu.gpr[4] = size_ptr;
  cpu.gpr[5] = kMemCommit | kMemReserve;
  cpu.gpr[6] = kPageReadWrite;
  cpu.gpr[7] = 0u;
  assert(fixture.invoke(cpu).success && cpu.gpr[3] == kStatusSuccess);
  const auto allocated_base = fixture.address_space->read32_be(base_ptr);
  fixture.address_space->write32_be(allocated_base, 0x600DF00Du);

  // Now request MEM_RESET alone on that same region.
  fixture.address_space->write32_be(base_ptr, allocated_base);
  fixture.address_space->write32_be(size_ptr, 0x1000u);
  cpu::CpuState reset_cpu{};
  reset_cpu.gpr[3] = base_ptr;
  reset_cpu.gpr[4] = size_ptr;
  reset_cpu.gpr[5] = kMemReset;
  reset_cpu.gpr[6] = kPageReadWrite;
  reset_cpu.gpr[7] = 0u;

  const auto result = fixture.invoke(reset_cpu);
  assert(result.handled && result.success);
  assert(reset_cpu.gpr[3] == kStatusSuccess);
  // The region's actual contents must be untouched by an unimplemented
  // optimization hint - never silently zeroed or reallocated.
  assert(fixture.address_space->read32_be(allocated_base) == 0x600DF00Du);
}

// A caller-specified fixed base address must be honored exactly.
void test_nt_allocate_virtual_memory_fixed_address() {
  NtAllocFixture fixture;

  // memory::kXex64KBase is the real, documented base of the 64KB-granularity
  // Xex region (see tests/xbox/xex_loader_tests.cpp's identical use of
  // kXex64KBase + offset as a valid reserve_fixed() target in Compact mode)
  // - an address picked from wherever the system allocator happens to place
  // an unrelated allocation is not a reliable stand-in for it, since
  // RegionKind::Xex requires a mirrored alias address (+/- 0x10000000) to
  // also be free, and Compact mode's reduced address space does not
  // necessarily hold that mirror for an arbitrary chosen address.
  const memory::GuestAddress target = memory::kXex64KBase + 0x40000u;

  const auto base_ptr = fixture.alloc32();
  const auto size_ptr = fixture.alloc32();
  fixture.address_space->write32_be(base_ptr, target);
  fixture.address_space->write32_be(size_ptr, 0x1000u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = base_ptr;
  cpu.gpr[4] = size_ptr;
  cpu.gpr[5] = kMemCommit | kMemReserve;
  cpu.gpr[6] = kPageReadWrite;
  cpu.gpr[7] = 0u;

  const auto result = fixture.invoke(cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == kStatusSuccess);
  assert(fixture.address_space->read32_be(base_ptr) == target &&
         "a fixed-address request must be honored at exactly the requested base");
}

// PAGE_NOACCESS must actually deny access, not just be accepted and ignored.
void test_nt_allocate_virtual_memory_protection_is_real() {
  NtAllocFixture fixture;

  const auto base_ptr = fixture.alloc32();
  const auto size_ptr = fixture.alloc32();
  fixture.address_space->write32_be(base_ptr, 0u);
  fixture.address_space->write32_be(size_ptr, 0x1000u);

  cpu::CpuState cpu{};
  cpu.gpr[3] = base_ptr;
  cpu.gpr[4] = size_ptr;
  cpu.gpr[5] = kMemCommit | kMemReserve;
  cpu.gpr[6] = kPageNoAccess;
  cpu.gpr[7] = 0u;

  const auto result = fixture.invoke(cpu);
  assert(result.handled && result.success);
  assert(cpu.gpr[3] == kStatusSuccess);

  const auto returned_base = fixture.address_space->read32_be(base_ptr);
  const auto info = fixture.address_space->query(returned_base);
  assert(info.has_value());
  assert(!memory::has(info->current_protect, memory::Protect::Read) &&
         !memory::has(info->current_protect, memory::Protect::Write) &&
         "PAGE_NOACCESS must translate to a real, enforced no-access mapping");
}

// A malformed request (zero size, null in/out pointers, or an unrecognized
// base protection value) must fail loudly with STATUS_INVALID_PARAMETER,
// never silently succeed.
void test_nt_allocate_virtual_memory_rejects_invalid_parameters() {
  NtAllocFixture fixture;
  const auto base_ptr = fixture.alloc32();
  const auto size_ptr = fixture.alloc32();

  // Zero size.
  {
    fixture.address_space->write32_be(base_ptr, 0u);
    fixture.address_space->write32_be(size_ptr, 0u);
    cpu::CpuState cpu{};
    cpu.gpr[3] = base_ptr;
    cpu.gpr[4] = size_ptr;
    cpu.gpr[5] = kMemCommit | kMemReserve;
    cpu.gpr[6] = kPageReadWrite;
    const auto result = fixture.invoke(cpu);
    assert(result.handled && result.success);
    assert(cpu.gpr[3] == kStatusInvalidParameter);
  }

  // Neither MEM_COMMIT nor MEM_RESERVE (nor MEM_RESET) requested.
  {
    fixture.address_space->write32_be(base_ptr, 0u);
    fixture.address_space->write32_be(size_ptr, 0x1000u);
    cpu::CpuState cpu{};
    cpu.gpr[3] = base_ptr;
    cpu.gpr[4] = size_ptr;
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = kPageReadWrite;
    const auto result = fixture.invoke(cpu);
    assert(result.handled && result.success);
    assert(cpu.gpr[3] == kStatusInvalidParameter);
  }

  // Null PBaseAddress.
  {
    cpu::CpuState cpu{};
    cpu.gpr[3] = 0u;
    cpu.gpr[4] = size_ptr;
    cpu.gpr[5] = kMemCommit | kMemReserve;
    cpu.gpr[6] = kPageReadWrite;
    const auto result = fixture.invoke(cpu);
    assert(result.handled && result.success);
    assert(cpu.gpr[3] == kStatusInvalidParameter);
  }

  // MEM_RESET combined with MEM_COMMIT/MEM_RESERVE is invalid (verified
  // against xenia's real implementation: "If MEM_RESET is set only MEM_RESET
  // can be set").
  {
    fixture.address_space->write32_be(base_ptr, 0u);
    fixture.address_space->write32_be(size_ptr, 0x1000u);
    cpu::CpuState cpu{};
    cpu.gpr[3] = base_ptr;
    cpu.gpr[4] = size_ptr;
    cpu.gpr[5] = kMemCommit | kMemReset;
    cpu.gpr[6] = kPageReadWrite;
    const auto result = fixture.invoke(cpu);
    assert(result.handled && result.success);
    assert(cpu.gpr[3] == kStatusInvalidParameter);
  }
}

void test_ordinal_is_registered() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_memory_exports(registry));

  assert(registry.contains("xboxkrnl.exe", 0x065u));
  assert(registry.contains("xboxkrnl", 0x065u));  // ".exe" normalization
  assert(registry.contains("xboxkrnl", "KeFlushUserModeTb"));

  // Registering twice must remain safe (session re-init calls this again).
  assert(xbox::register_xboxkrnl_memory_exports(registry));
}

// KeFlushUserModeTb is a real, behaviorally-correct no-op on Xenon's
// host-VM-backed memory model (see xboxkrnl_memory_exports.cpp's rationale),
// not an unimplemented stub - it must report handled+success regardless of
// whatever register state a guest caller happens to leave, and must never
// touch guest memory or crash on an arbitrary/garbage argument.
void test_ke_flush_user_mode_tb_handles_and_leaves_memory_untouched() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_memory_exports(registry));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  memory::GuestAddress sentinel_address{};
  assert(memory.allocate(8, 8, memory::kReadWrite, false, sentinel_address));
  memory.write64_be(sentinel_address, 0xDEADBEEFCAFEF00Du);

  cpu::CpuState cpu{};
  cpu.gpr[3] = sentinel_address;  // arbitrary/garbage argument
  cpu.gpr[4] = 0x12345678u;
  core::ExportCallContext call{cpu, memory, 0, 0};

  const auto result = registry.invoke("xboxkrnl", 0x065u, call);
  assert(result.handled && result.success);
  assert(memory.read64_be(sentinel_address) == 0xDEADBEEFCAFEF00Du &&
         "KeFlushUserModeTb must never write to guest memory");
}

void test_mm_free_physical_memory_registered_and_handled() {
  core::ExportRegistry registry;
  assert(xbox::register_xboxkrnl_memory_exports(registry));

  assert(!registry.contains("xboxkrnl", 0x0BDu));
}

struct MmFixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;

  MmFixture() {
    address_space = std::make_shared<memory::AddressSpace>(
        memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    register_one(0x0BAu, "MmAllocatePhysicalMemoryEx",
                 &xbox::mm_allocate_physical_memory_ex_export);
    register_one(0x0BDu, "MmFreePhysicalMemory",
                 &xbox::mm_free_physical_memory_export);
    register_one(0x0BEu, "MmGetPhysicalAddress",
                 &xbox::mm_get_physical_address_export);
    register_one(0x0C4u, "MmQueryAddressProtect",
                 &xbox::mm_query_address_protect_export);
    register_one(0x0C5u, "MmQueryAllocationSize",
                 &xbox::mm_query_allocation_size_export);
    register_one(0x0C6u, "MmQueryStatistics",
                 &xbox::mm_query_statistics_export);
    register_one(0x0C7u, "MmSetAddressProtect",
                 &xbox::mm_set_address_protect_export);
  }

  using Handler = bool (*)(kernel::KernelProcess&, core::ExportCallContext&);
  void register_one(std::uint32_t ordinal, const char* name, Handler handler) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = name;
    descriptor.ordinal = ordinal;
    auto* raw_process = process.get();
    descriptor.handler = [raw_process, handler](core::ExportCallContext& context) {
      return handler(*raw_process, context);
    };
    assert(registry.register_export(std::move(descriptor)));
  }

  core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext context{cpu, *address_space, 0, 11};
    return registry.invoke("xboxkrnl", ordinal, context);
  }

  memory::GuestAddress allocate_physical(std::uint32_t size = 0x1000u,
                                         std::uint32_t protect = kPageReadWrite) {
    cpu::CpuState cpu{};
    cpu.gpr[4] = size;
    cpu.gpr[5] = protect;
    cpu.gpr[6] = 0u;
    cpu.gpr[7] = UINT32_MAX;
    cpu.gpr[8] = 0u;
    assert(invoke(0x0BAu, cpu).success);
    return static_cast<memory::GuestAddress>(cpu.gpr[3]);
  }
};

void test_mm_physical_query_protect_free_and_reuse() {
  MmFixture fixture;
  cpu::CpuState invalid_physical{};
  invalid_physical.gpr[3] = 0x1000u;
  assert(fixture.invoke(0x0BEu, invalid_physical).success);
  assert(invalid_physical.gpr[3] == 0u);

  const auto alias = fixture.allocate_physical();
  assert(alias != 0u);
  const auto physical = fixture.address_space->get_physical_address(alias);
  assert(physical != UINT32_MAX);
  assert(fixture.address_space->query_physical_allocation(physical).has_value());

  cpu::CpuState get_physical{};
  get_physical.gpr[3] = alias + 0x100u;
  assert(fixture.invoke(0x0BEu, get_physical).success);
  assert(get_physical.gpr[3] == physical + 0x100u);

  cpu::CpuState query_size{};
  query_size.gpr[3] = alias + 0x800u;
  assert(fixture.invoke(0x0C5u, query_size).success);
  assert(query_size.gpr[3] == 0x1000u);

  cpu::CpuState query_protect{};
  query_protect.gpr[3] = alias;
  assert(fixture.invoke(0x0C4u, query_protect).success);
  assert(query_protect.gpr[3] == kPageReadWrite);

  cpu::CpuState set_read_only{};
  set_read_only.gpr[3] = alias;
  set_read_only.gpr[4] = 1u;
  set_read_only.gpr[5] = 0x02u;
  assert(fixture.invoke(0x0C7u, set_read_only).success);
  query_protect.gpr[3] = alias;
  assert(fixture.invoke(0x0C4u, query_protect).success);
  assert(query_protect.gpr[3] == 0x02u);
  bool write_faulted = false;
  try {
    fixture.address_space->write32_be(alias, 0x12345678u);
  } catch (const memory::MemoryFault&) {
    write_faulted = true;
  }
  assert(write_faulted);

  cpu::CpuState restore_rw{};
  restore_rw.gpr[3] = alias;
  restore_rw.gpr[4] = 0x1000u;
  restore_rw.gpr[5] = kPageReadWrite;
  assert(fixture.invoke(0x0C7u, restore_rw).success);
  fixture.address_space->write32_be(alias, 0x12345678u);

  cpu::CpuState free_cpu{};
  free_cpu.gpr[4] = alias + 0x100u;  // interior aliases canonicalize via metadata
  assert(fixture.invoke(0x0BDu, free_cpu).success);
  assert(!fixture.address_space->query_physical_allocation(physical).has_value());
  query_size.gpr[3] = alias;
  assert(fixture.invoke(0x0C5u, query_size).success);
  assert(query_size.gpr[3] == 0u);

  // The Xbox ABI is void: an invalid/double free is ignored, but it must not
  // release an unrelated range or corrupt the allocator.
  assert(fixture.invoke(0x0BDu, free_cpu).success);
  assert(!fixture.address_space->query_physical_allocation(physical).has_value());

  const auto reused_alias = fixture.allocate_physical();
  assert(fixture.address_space->get_physical_address(reused_alias) == physical);
}

void test_mm_query_statistics_validation_and_dynamic_availability() {
  MmFixture fixture;
  memory::GuestAddress output{};
  assert(fixture.address_space->allocate(104u, 4u, memory::kReadWrite, false, output));

  cpu::CpuState null_cpu{};
  assert(fixture.invoke(0x0C6u, null_cpu).success);
  assert(null_cpu.gpr[3] == kStatusInvalidParameter);

  fixture.address_space->write32_be(output, 100u);
  cpu::CpuState short_cpu{};
  short_cpu.gpr[3] = output;
  assert(fixture.invoke(0x0C6u, short_cpu).success);
  assert(short_cpu.gpr[3] == kStatusBufferTooSmall);

  const auto read_available = [&] {
    fixture.address_space->write32_be(output, 104u);
    cpu::CpuState cpu{};
    cpu.gpr[3] = output;
    assert(fixture.invoke(0x0C6u, cpu).success);
    assert(cpu.gpr[3] == kStatusSuccess);
    assert(fixture.address_space->read32_be(output) == 104u);
    assert(fixture.address_space->read32_be(output + 4u) ==
           memory::kPhysicalMemorySize / memory::kBasePageSize);
    return fixture.address_space->read32_be(output + 12u);
  };

  const auto before = read_available();
  const auto alias = fixture.allocate_physical();
  const auto after_allocate = read_available();
  assert(after_allocate + 1u == before);
  cpu::CpuState free_cpu{};
  free_cpu.gpr[4] = alias;
  assert(fixture.invoke(0x0BDu, free_cpu).success);
  assert(read_available() == before);
}

void test_memory_export_overflow_rejected() {
  NtAllocFixture virtual_fixture;
  const auto base_ptr = virtual_fixture.alloc32();
  const auto size_ptr = virtual_fixture.alloc32();
  virtual_fixture.address_space->write32_be(base_ptr, 0u);
  virtual_fixture.address_space->write32_be(size_ptr, UINT32_MAX);
  cpu::CpuState virtual_cpu{};
  virtual_cpu.gpr[3] = base_ptr;
  virtual_cpu.gpr[4] = size_ptr;
  virtual_cpu.gpr[5] = kMemCommit | kMemReserve;
  virtual_cpu.gpr[6] = kPageReadWrite;
  assert(virtual_fixture.invoke(virtual_cpu).success);
  assert(virtual_cpu.gpr[3] == kStatusInvalidParameter);

  MmFixture physical_fixture;
  assert(physical_fixture.allocate_physical(UINT32_MAX) == 0u);
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl memory exports...\n";

  test_ordinal_is_registered();
  test_ke_flush_user_mode_tb_handles_and_leaves_memory_untouched();
  test_nt_allocate_virtual_memory_system_chosen_address();
  test_nt_allocate_virtual_memory_reset_is_a_real_noop();
  test_nt_allocate_virtual_memory_fixed_address();
  test_nt_allocate_virtual_memory_protection_is_real();
  test_nt_allocate_virtual_memory_rejects_invalid_parameters();
  test_mm_free_physical_memory_registered_and_handled();
  test_mm_physical_query_protect_free_and_reuse();
  test_mm_query_statistics_validation_and_dynamic_availability();
  test_memory_export_overflow_rejected();

  std::cout << "All xboxkrnl memory export tests passed!\n";
  return 0;
}
