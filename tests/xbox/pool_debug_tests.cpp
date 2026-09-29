// Kernel pool (ExAllocatePool/ExAllocatePoolWithTag/ExAllocatePoolTypeWithTag/
// ExFreePool/ExQueryPoolBlockSize) and the kernel debug exports (DbgBreakPoint*,
// DbgPrompt, KiApcNormalRoutineNop, KeBugCheck/KeBugCheckEx).
//
// The pool is checked directly (layout, alignment, coalescing, misuse) and
// through core::ExportRegistry::invoke() with real guest memory. Ace Combat 6
// imports ExAllocatePool, ExFreePool, KeBugCheck and KeBugCheckEx.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/pool.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_pool_exports.hpp"
#include "xenon/xbox/xboxkrnl_string_exports.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kOrdDbgBreakPoint = 0x01u;
constexpr std::uint32_t kOrdDbgBreakPointWithStatus = 0x02u;
constexpr std::uint32_t kOrdDbgPrompt = 0x04u;
constexpr std::uint32_t kOrdExAllocatePool = 0x09u;
constexpr std::uint32_t kOrdExAllocatePoolWithTag = 0x0Au;
constexpr std::uint32_t kOrdExAllocatePoolTypeWithTag = 0x0Bu;
constexpr std::uint32_t kOrdExFreePool = 0x0Fu;
constexpr std::uint32_t kOrdExQueryPoolBlockSize = 0x13u;
constexpr std::uint32_t kOrdKeBugCheck = 0x52u;
constexpr std::uint32_t kOrdKeBugCheckEx = 0x53u;
constexpr std::uint32_t kOrdKiApcNormalRoutineNop = 0x1DFu;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;
  std::vector<xbox::BugCheckInfo> bugchecks;

  using Handler = bool (*)(kernel::KernelProcess&, core::ExportCallContext&);

  void add(std::uint32_t ordinal, const char* name, Handler handler) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = name;
    descriptor.ordinal = ordinal;
    kernel::KernelProcess* raw = process.get();
    descriptor.handler = [raw, handler](core::ExportCallContext& ctx) { return handler(*raw, ctx); };
    const bool ok = registry.register_export(std::move(descriptor));
    assert(ok);
    static_cast<void>(ok);
  }

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    const bool ok = address_space->initialize();
    assert(ok);
    static_cast<void>(ok);
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    add(kOrdExAllocatePool, "ExAllocatePool", &xbox::ex_allocate_pool_export);
    add(kOrdExAllocatePoolWithTag, "ExAllocatePoolWithTag", &xbox::ex_allocate_pool_with_tag_export);
    add(kOrdExAllocatePoolTypeWithTag, "ExAllocatePoolTypeWithTag",
        &xbox::ex_allocate_pool_type_with_tag_export);
    add(kOrdExFreePool, "ExFreePool", &xbox::ex_free_pool_export);
    add(kOrdExQueryPoolBlockSize, "ExQueryPoolBlockSize", &xbox::ex_query_pool_block_size_export);
    const bool debug_ok = xbox::register_xboxkrnl_debug_exports(registry);
    const bool bugcheck_ok = xbox::register_xboxkrnl_bugcheck_exports(
        registry, [this](const xbox::BugCheckInfo& info) { bugchecks.push_back(info); });
    assert(debug_ok && bugcheck_ok);
    static_cast<void>(debug_ok);
    static_cast<void>(bugcheck_ok);
  }

  kernel::KernelPool& pool() { return process->pool(); }

  std::uint64_t call(std::uint32_t ordinal, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                     std::uint64_t r5 = 0, std::uint64_t r6 = 0, std::uint64_t r7 = 0) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    cpu.gpr[6] = r6;
    cpu.gpr[7] = r7;
    core::ExportCallContext ctx{cpu, *address_space, 0, 0};
    const auto result = registry.invoke("xboxkrnl", ordinal, ctx);
    assert(result.handled && result.success);
    return cpu.gpr[3];
  }
};

// A small allocation carries the 8-byte pool header (0xAA marker, big-endian
// tag) directly before the payload, in a 64-byte-aligned block, and is real,
// writable guest memory.
void test_small_allocation_layout() {
  Fixture f;
  const auto payload = f.pool().allocate(100, 0x54455354u);  // 'TEST'
  assert(payload != 0u);
  const auto block = payload - kernel::KernelPool::kHeaderSize;
  assert(block % kernel::KernelPool::kBlockAlignment == 0u && "the block is 64-byte aligned");
  assert(f.address_space->read8(block + 2u) == kernel::KernelPool::kPoolMarker);
  assert(f.address_space->read32_be(block + 4u) == 0x54455354u);
  for (std::uint32_t i = 0; i < 100u; i += 4u) f.address_space->write32_be(payload + i, i);
  for (std::uint32_t i = 0; i < 100u; i += 4u) assert(f.address_space->read32_be(payload + i) == i);
  assert(f.pool().usable_size(payload) >= 100u);
  assert(f.pool().outstanding_allocations() == 1u);
  assert(f.pool().free(payload));
  assert(f.pool().outstanding_allocations() == 0u);
  assert(f.address_space->read8(block + 2u) == 0u && "a freed block no longer reads as live");
}

// Distinct live allocations never overlap, and a zero-size request still yields
// a valid, distinct block.
void test_allocations_do_not_overlap() {
  Fixture f;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> blocks;
  for (std::uint32_t size : {0u, 1u, 8u, 56u, 57u, 100u, 500u, 0xFD8u, 64u, 1u}) {
    const auto payload = f.pool().allocate(size, size);
    assert(payload != 0u);
    blocks.emplace_back(payload, size);
    f.address_space->fill_bytes(payload, size ? size : 1u, static_cast<std::uint8_t>(size));
  }
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    for (std::size_t j = i + 1; j < blocks.size(); ++j) {
      const auto a0 = blocks[i].first - 8u, a1 = blocks[i].first + (blocks[i].second ? blocks[i].second : 1u);
      const auto b0 = blocks[j].first - 8u, b1 = blocks[j].first + (blocks[j].second ? blocks[j].second : 1u);
      assert(a1 <= b0 || b1 <= a0);
    }
  }
  for (const auto& [payload, size] : blocks) {
    for (std::uint32_t i = 0; i < (size ? size : 1u); ++i) {
      assert(f.address_space->read8(payload + i) == static_cast<std::uint8_t>(size));
    }
  }
  for (const auto& block : blocks) assert(f.pool().free(block.first));
}

// A request above the small limit is a stand-alone page-aligned allocation with
// no header; it frees back to the address space.
void test_large_allocation() {
  Fixture f;
  const auto big = f.pool().allocate(0x5000u, 1u);
  assert(big != 0u && big % 0x1000u == 0u);
  f.address_space->write32_be(big + 0x4FFCu, 0xA5A5A5A5u);
  assert(f.address_space->read32_be(big + 0x4FFCu) == 0xA5A5A5A5u);
  assert(f.pool().usable_size(big) == 0x5000u);
  const auto boundary = f.pool().allocate(kernel::KernelPool::kSmallLimit, 2u);
  const auto over = f.pool().allocate(kernel::KernelPool::kSmallLimit + 1u, 3u);
  assert(boundary % 0x1000u != 0u && "at the limit it is still a small block");
  assert(over % 0x1000u == 0u && "one byte over the limit is page-aligned");
  assert(f.pool().free(big) && f.pool().free(boundary) && f.pool().free(over));
  const auto mapping = f.address_space->query(big);
  assert(mapping && mapping->state == memory::PageState::Free);
}

// Freed blocks are reused, and adjacent freed blocks coalesce so a larger
// request can reuse the merged space instead of growing the arena.
void test_free_reuses_and_coalesces() {
  Fixture f;
  const auto a = f.pool().allocate(56, 1u);   // one 64-byte block
  const auto b = f.pool().allocate(56, 1u);
  const auto c = f.pool().allocate(56, 1u);
  assert(b == a + 64u && c == b + 64u && "blocks are carved contiguously");
  assert(f.pool().free(b));
  assert(f.pool().allocate(56, 2u) == b && "the freed block is reused");
  assert(f.pool().free(b) && f.pool().free(a) && f.pool().free(c));
  // a, b, c are now one merged 192-byte run: a 184-byte request (192-byte
  // block) fits back at a.
  assert(f.pool().allocate(184, 3u) == a && "adjacent free blocks coalesce");
}

// More small allocations than one arena holds spill into a second arena and all
// stay valid; freeing everything releases them.
void test_arena_growth() {
  Fixture f;
  std::vector<std::uint32_t> blocks;
  std::set<std::uint32_t> unique;
  const std::uint32_t per_arena = kernel::KernelPool::kArenaSize / 4096u;
  for (std::uint32_t i = 0; i < per_arena + 40u; ++i) {
    const auto payload = f.pool().allocate(0xF00u, i);
    assert(payload != 0u && unique.insert(payload).second);
    f.address_space->write32_be(payload, i);
    blocks.push_back(payload);
  }
  for (std::uint32_t i = 0; i < blocks.size(); ++i) {
    assert(f.address_space->read32_be(blocks[i]) == i);
  }
  for (const auto payload : blocks) assert(f.pool().free(payload));
  assert(f.pool().outstanding_allocations() == 0u);
}

// Misuse is rejected without corrupting the pool: a double free, an interior
// pointer, a never-issued pointer.
void test_misuse_is_rejected() {
  Fixture f;
  const auto a = f.pool().allocate(100, 1u);
  const auto b = f.pool().allocate(100, 2u);
  assert(f.pool().free(a));
  assert(!f.pool().free(a) && "a double free");
  assert(!f.pool().free(b + 8u) && "an interior pointer");
  assert(!f.pool().free(0x12345678u) && "a pointer the pool never issued");
  assert(f.pool().free(0u) && "ExFreePool(NULL) is a no-op");
  assert(f.pool().usable_size(0x12345678u) == 0u);
  assert(f.pool().free(b));
  assert(f.pool().allocate(100, 3u) == a || f.pool().outstanding_allocations() == 1u);
}

// The exports: allocate through each ordinal, tag reaches the header, free, and
// query block size.
void test_pool_exports() {
  Fixture f;
  const auto plain = static_cast<std::uint32_t>(f.call(kOrdExAllocatePool, 200));
  assert(plain != 0u);
  assert(f.address_space->read32_be(plain - 4u) == 0x656E6F4Eu && "default tag is 'None'");
  const auto tagged = static_cast<std::uint32_t>(f.call(kOrdExAllocatePoolWithTag, 200, 0x41424344u));
  assert(f.address_space->read32_be(tagged - 4u) == 0x41424344u);
  const auto typed =
      static_cast<std::uint32_t>(f.call(kOrdExAllocatePoolTypeWithTag, 200, 0x45464748u, 1u));
  assert(f.address_space->read32_be(typed - 4u) == 0x45464748u);
  assert(plain != tagged && tagged != typed);

  const auto quota = f.address_space->read8(plain);  // scratch: any committed byte
  static_cast<void>(quota);
  assert(f.call(kOrdExQueryPoolBlockSize, tagged) >= 200u);
  assert(f.call(kOrdExQueryPoolBlockSize, 0x12345678u) == 0u);

  f.call(kOrdExFreePool, plain);
  f.call(kOrdExFreePool, tagged);
  f.call(kOrdExFreePool, typed);
  f.call(kOrdExFreePool, 0u);
  assert(f.pool().outstanding_allocations() == 0u);

  const auto big = static_cast<std::uint32_t>(f.call(kOrdExAllocatePool, 0x8000));
  assert(big % 0x1000u == 0u && f.call(kOrdExQueryPoolBlockSize, big) == 0x8000u);
  f.call(kOrdExFreePool, big);
}

// Freeing a foreign pointer logs at Error level (real hardware bugchecks) and
// leaves the pool intact.
void test_free_of_foreign_pointer_is_reported() {
  Fixture f;
  auto& logger = logging::Logger::instance();
  const auto previous = logger.min_level();
  logger.set_min_level(logging::Level::Error);
  std::vector<std::string> messages;
  logger.set_sink([&](logging::Level level, std::string_view category, std::string_view message) {
    if (level == logging::Level::Error && category == "pool") messages.emplace_back(message);
  });
  const auto live = static_cast<std::uint32_t>(f.call(kOrdExAllocatePool, 64));
  f.call(kOrdExFreePool, 0x0BADF00Du);
  assert(messages.size() == 1u);
  assert(messages[0].find("0BADF00D") != std::string::npos);
  assert(f.pool().outstanding_allocations() == 1u && "the live block is untouched");
  f.call(kOrdExFreePool, live);
  logger.set_sink({});
  logger.set_min_level(previous);
}

void test_debug_exports() {
  Fixture f;
  // DbgBreakPoint*: returns to the caller (no guest debugger exists).
  assert(f.call(kOrdDbgBreakPoint, 0x1234u) == 0x1234u);
  assert(f.call(kOrdDbgBreakPointWithStatus, 0x5678u) == 0x5678u);

  // DbgPrompt reads nothing: empty response, 0 characters.
  memory::GuestAddress response{};
  const bool ok = f.address_space->allocate(0x100u, 0x10u, memory::kReadWrite, false, response);
  assert(ok);
  static_cast<void>(ok);
  f.address_space->write8(response, 'X');
  assert(f.call(kOrdDbgPrompt, 0u, response, 0x100u) == 0u);
  assert(f.address_space->read8(response) == 0u);
  f.address_space->write8(response, 'X');
  assert(f.call(kOrdDbgPrompt, 0u, response, 0u) == 0u);
  assert(f.address_space->read8(response) == 'X' && "a zero-length response buffer is not written");

  // KiApcNormalRoutineNop: registers are untouched.
  cpu::CpuState cpu{};
  cpu.gpr[3] = 0x11u;
  cpu.gpr[4] = 0x22u;
  core::ExportCallContext ctx{cpu, *f.address_space, 0, 0};
  assert(f.registry.invoke("xboxkrnl", kOrdKiApcNormalRoutineNop, ctx).handled);
  assert(cpu.gpr[3] == 0x11u && cpu.gpr[4] == 0x22u);
}

void test_bugcheck_exports() {
  Fixture f;
  f.call(kOrdKeBugCheck, 0x1Eu, 0xAAu);
  assert(f.bugchecks.size() == 1u);
  assert(f.bugchecks[0].code == 0x1Eu);
  assert(f.bugchecks[0].parameters[0] == 0u && "the plain form has no parameters");
  assert(f.bugchecks[0].description.find("0x0000001E") != std::string::npos);

  f.call(kOrdKeBugCheckEx, 0xC2u, 7u, 0x100u, 0x200u, 0x300u);
  assert(f.bugchecks.size() == 2u);
  assert(f.bugchecks[1].code == 0xC2u);
  assert(f.bugchecks[1].parameters[0] == 7u && f.bugchecks[1].parameters[1] == 0x100u &&
         f.bugchecks[1].parameters[2] == 0x200u && f.bugchecks[1].parameters[3] == 0x300u);
  assert(f.bugchecks[1].description.find("0x000000C2") != std::string::npos);
  assert(f.bugchecks[1].description.find("0x00000300") != std::string::npos);
}

}  // namespace

int main() {
  std::cout << "Testing kernel pool and debug exports...\n";
  test_small_allocation_layout();
  test_allocations_do_not_overlap();
  test_large_allocation();
  test_free_reuses_and_coalesces();
  test_arena_growth();
  test_misuse_is_rejected();
  test_pool_exports();
  test_free_of_foreign_pointer_is_reported();
  test_debug_exports();
  test_bugcheck_exports();
  std::cout << "All kernel pool and debug export tests passed!\n";
  return 0;
}
