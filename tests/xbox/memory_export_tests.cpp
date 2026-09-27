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

  assert(registry.contains("xboxkrnl", 0x0BDu));
  assert(registry.contains("xboxkrnl", "MmFreePhysicalMemory"));

  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  cpu::CpuState cpu{};
  cpu.gpr[3] = 1u;           // type
  cpu.gpr[4] = 0xA0001000u;  // an arbitrary physical-alias-looking address
  core::ExportCallContext call{cpu, memory, 0, 0};
  const auto result = registry.invoke("xboxkrnl", 0x0BDu, call);
  assert(result.handled && result.success);
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

  std::cout << "All xboxkrnl memory export tests passed!\n";
  return 0;
}
