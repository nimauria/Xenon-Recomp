#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "xenon/cpu/executable_code_cache.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;
using namespace xenon::memory;

namespace {
constexpr GuestAddress kCode = 0x00600000u;
constexpr GuestAddress kCodeSecondPage = kCode + kBasePageSize;

ExecutionResult native_v1(CpuState& state, MemoryPort&, RuntimeServices&) {
  state.gpr[3] = 1u;
  return {FlowReason::Fallthrough, kCode + 4u, 0u};
}
ExecutionResult native_v2(CpuState& state, MemoryPort&, RuntimeServices&) {
  state.gpr[3] = 2u;
  return {FlowReason::Fallthrough, kCode + 4u, 0u};
}
ExecutionResult native_context(ExecutionContext& context) {
  context.state.gpr[4] += 1u;
  return {FlowReason::Fallthrough, kCode + 4u, 0u};
}

void seed_code(AddressSpace& memory) {
  memory.write32_be(kCode, 0x60000000u);  // nop-like source bytes for tracking.
}
}  // namespace

int main() {
  AddressSpace memory;
  assert(memory.initialize());
  assert(memory.commit_fixed(kCode, kBasePageSize * 2u, kReadWriteExecute));
  seed_code(memory);

  ExecutableCodeCache cache;
  assert(cache.register_current(memory, kCode, kCode, 4u, native_v1));
  assert(cache.size() == 1u);
  assert(cache.lookup(memory, kCode) == native_v1);
  // Part 14 of the AC6 Runtime Readiness pass ("Runtime Fallback
  // Accounting"): every successful lookup hit is the AOT-side half of the
  // AOT-vs-fallback ratio a capability report needs.
  assert(cache.aot_lookup_hits() == 1u);

  CpuState state{};
  NullRuntimeServices runtime;
  auto result = cache.execute(kCode, state, memory, runtime);
  assert(result && result->reason == FlowReason::Fallthrough);
  assert(state.gpr[3] == 1u);

  // CPU V2 entries bind directly into an ExecutionContext. Branch lookups may
  // consume any current V2 entry, while indirect calls require an explicit
  // call-safe registration (the compiler/linker must prove normal LR return
  // behaviour before opting in).
  ExecutableCodeCache v2_cache;
  assert(v2_cache.register_current_v2(memory, kCode, kCode, 4u,
                                      native_context));
  ExecutionContext v2_context(state, memory, runtime);
  v2_cache.bind(v2_context);
  assert(v2_context.lookup_compiled(kCode, CompiledLookupKind::Branch) ==
         native_context);
  assert(v2_cache.aot_lookup_hits() == 1u);
  assert(v2_context.lookup_compiled(kCode, CompiledLookupKind::Call) ==
         nullptr);
  // A rejected call-unsafe lookup must not count as an AOT hit.
  assert(v2_cache.aot_lookup_hits() == 1u);
  assert(v2_cache.register_current_v2(memory, kCode, kCode, 4u,
                                      native_context, true));
  assert(v2_context.lookup_compiled(kCode, CompiledLookupKind::Call) ==
         native_context);
  assert(v2_cache.aot_lookup_hits() == 2u);
  auto v2_result = v2_cache.execute_v2(kCode, v2_context);
  assert(v2_result && state.gpr[4] == 1u);
  assert(v2_cache.aot_lookup_hits() == 3u);

  // The lock-free hot entry never bypasses executable-generation validation.
  // SMC makes the next hot lookup miss and lazily evicts the authoritative
  // registry entry as well.
  memory.write32_be(kCode, 0x60000009u);
  assert(v2_context.lookup_compiled(kCode, CompiledLookupKind::Branch) ==
         nullptr);
  assert(v2_cache.size() == 0u && v2_cache.stale_evictions() == 1u);

  // CPU SMC advances the physical executable generation without invoking the
  // cache. The next dispatch lookup observes the mismatch and evicts lazily.
  const auto before_cpu_write = memory.executable_page_stamp(kCode);
  memory.write32_be(kCode, 0x60000001u);
  const auto after_cpu_write = memory.executable_page_stamp(kCode);
  assert(after_cpu_write.physical_page == before_cpu_write.physical_page);
  assert(after_cpu_write.generation != before_cpu_write.generation);
  assert(cache.lookup(memory, kCode) == nullptr);
  assert(cache.size() == 0u && cache.stale_evictions() == 1u);

  // Writes through an Xbox physical alias must invalidate the same native
  // translation because stamps are keyed by physical page identity.
  assert(cache.register_current(memory, kCode, kCode, 4u, native_v1));
  const auto physical = memory.get_physical_address(kCode);
  assert(physical != 0xFFFFFFFFu);
  memory.write32_be(kPhysical64KBase + physical, 0x60000002u);
  assert(cache.lookup(memory, kCode) == nullptr);

  // Controlled external/DMA/GPU writes participate in the same generation.
  assert(cache.register_current(memory, kCode, kCode, 4u, native_v1));
  const std::array<std::byte, 4> external_bytes{
      std::byte{0x60}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03}};
  assert(memory.write_physical(physical, external_bytes));
  assert(cache.lookup(memory, kCode) == nullptr);

  // icbi may arrive through a non-executable physical alias. It still bumps
  // the generation of physical code that has previously been executable.
  assert(cache.register_current(memory, kCode, kCode, 4u, native_v1));
  memory.instruction_cache_invalidate(kPhysical64KBase + physical);
  assert(cache.lookup(memory, kCode) == nullptr);

  // Execute protection transitions are their own code epoch. An old cached
  // entry must not resurrect after Execute -> NX -> Execute with no lookup in
  // between.
  assert(cache.register_current(memory, kCode, kCode, 4u, native_v1));
  const auto before_protect = memory.executable_page_stamp(kCode);
  assert(memory.protect(kCode, kBasePageSize, kReadWrite));
  assert(!memory.executable_page_stamp(kCode).executable());
  assert(memory.protect(kCode, kBasePageSize, kReadWriteExecute));
  const auto after_protect = memory.executable_page_stamp(kCode);
  assert(after_protect.executable());
  assert(after_protect.generation != before_protect.generation);
  assert(cache.lookup(memory, kCode) == nullptr);

  // Unmap/remap of the same guest address is protected against physical-page
  // and generation ABA, even if the allocator hands the same frame back.
  assert(cache.register_current(memory, kCode, kCode, 4u, native_v1));
  const auto before_decommit = memory.executable_page_stamp(kCode);
  assert(memory.decommit(kCode, kBasePageSize));
  assert(memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute));
  const auto after_recommit = memory.executable_page_stamp(kCode);
  assert(after_recommit.executable());
  assert(after_recommit != before_decommit);
  assert(cache.lookup(memory, kCode) == nullptr);

  // A multi-page native translation is invalid if any covered source page
  // changes, not merely the page containing its entry point.
  assert(memory.protect(kCodeSecondPage, kBasePageSize, kReadWriteExecute));
  assert(cache.register_current(memory, kCode, kCode,
                                kBasePageSize + 16u, native_v1));
  memory.write32_be(kCodeSecondPage, 0x60000004u);
  assert(cache.lookup(memory, kCode) == nullptr);

  // Physical allocation protection is also an executable epoch boundary.
  // This exercises the Xbox-facing MmSetAddressProtect-style path rather than
  // only virtual protect(): A/C/E aliases must all observe the same physical
  // generation and an Execute -> NX -> Execute cycle cannot revive old code.
  PhysicalAllocationOptions physical_code_options{};
  physical_code_options.protect = kReadWriteExecute;
  std::uint32_t physical_code{};
  assert(memory.allocate_physical(kBasePageSize, physical_code_options,
                                  physical_code));
  const auto physical_code_alias = AddressSpace::physical_guest_alias(
      physical_code, PhysicalPageClass::Page4K);
  assert(physical_code_alias);
  memory.write32_be(*physical_code_alias, 0x60000000u);
  ExecutableCodeCache physical_cache;
  assert(physical_cache.register_current(memory, *physical_code_alias,
                                         *physical_code_alias, 4u, native_v1));
  const auto physical_before =
      memory.executable_page_stamp(*physical_code_alias);
  assert(memory.protect_physical(physical_code, kBasePageSize, kReadWrite));
  assert(!memory.executable_page_stamp(*physical_code_alias).executable());
  assert(memory.protect_physical(physical_code, kBasePageSize,
                                 kReadWriteExecute));
  const auto physical_after =
      memory.executable_page_stamp(*physical_code_alias);
  assert(physical_after.executable());
  assert(physical_after.physical_page == physical_before.physical_page);
  assert(physical_after.generation != physical_before.generation);
  assert(physical_cache.lookup(memory, *physical_code_alias) == nullptr);
  assert(memory.free_physical(physical_code, kBasePageSize));

  // XEX 64K/4K aliases share physical executable identity as well. A write via
  // the alternate guest view invalidates code registered from the original.
  constexpr GuestAddress kXexCode = 0x82000000u;
  constexpr GuestAddress kXexAlias = 0x92000000u;
  assert(memory.commit_fixed(kXexCode, kLargePageSize, kReadWriteExecute));
  memory.write32_be(kXexCode, 0x60000000u);
  ExecutableCodeCache xex_cache;
  assert(xex_cache.register_current(memory, kXexCode, kXexCode, 4u, native_v1));
  assert(memory.executable_page_stamp(kXexCode) ==
         memory.executable_page_stamp(kXexAlias));
  memory.write32_be(kXexAlias, 0x60000007u);
  assert(xex_cache.lookup(memory, kXexCode) == nullptr);

  // A dynamic translator must provide the source snapshot it decoded. If the
  // bytes changed before installation, registration rejects the native result.
  auto stale_source = ExecutableCodeCache::capture_source(memory, kCode, 4u);
  assert(stale_source);
  memory.write32_be(kCode, 0x60000005u);
  assert(!cache.register_compiled(
      memory, CompiledTranslation{kCode, native_v1, *stale_source}));
  assert(cache.compilation_rejections() >= 1u);

  // Resolve can recompile stale dynamic pages without an interpreter. The
  // callback is invoked again after SMC and the new native entry replaces the
  // invalidated one only if its snapshot is still current.
  std::atomic<unsigned> compile_count{0u};
  ExecutableCodeCache dynamic_cache(
      [&](GuestAddress target, MemoryPort& port)
          -> std::optional<CompiledTranslation> {
        const auto source =
            ExecutableCodeCache::capture_source(port, target, 4u);
        if (!source) return std::nullopt;
        // A dynamic translator fetches through the Execute-aware MemoryPort
        // path, not an ordinary data read or interpreter fallback.
        (void)port.fetch32_be(target);
        const auto number = compile_count.fetch_add(1u) + 1u;
        return CompiledTranslation{target,
                                   number == 1u ? native_v1 : native_v2,
                                   *source};
      });
  assert(dynamic_cache.resolve(memory, kCode) == native_v1);
  memory.write32_be(kCode, 0x60000006u);
  assert(dynamic_cache.resolve(memory, kCode) == native_v2);
  assert(compile_count.load() == 2u);

  // Reset is also an executable epoch boundary. A code cache may outlive the
  // guest reset, so generations must not return to an old ABA value.
  const auto reset_snapshot =
      ExecutableCodeCache::capture_source(memory, kCode, 4u);
  assert(reset_snapshot);
  ExecutableCodeCache reset_cache;
  assert(reset_cache.register_current(memory, kCode, kCode, 4u, native_v1));
  memory.reset();
  assert(memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute));
  seed_code(memory);
  assert(reset_cache.lookup(memory, kCode) == nullptr);

  // Explicit range invalidation is retained for module unload/tooling without
  // being required on the scalar write path.
  ExecutableCodeCache explicit_cache;
  assert(explicit_cache.register_current(memory, kCode, kCode, 4u, native_v1));
  explicit_cache.invalidate_guest_range(kCode, 4u);
  assert(explicit_cache.size() == 0u);

  // Six concurrent guest-thread-shaped lookups can race a writer/registrar
  // without data races in the cache. A lookup may see either a current native
  // entry or a miss, but never a stale entry after its generation check.
  ExecutableCodeCache concurrent_cache;
  assert(concurrent_cache.register_current(memory, kCode, kCode, 4u, native_v1));
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  readers.reserve(6u);
  for (unsigned i = 0; i < 6u; ++i) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      while (!stop.load(std::memory_order_acquire)) {
        const auto function = concurrent_cache.lookup(memory, kCode);
        assert(function == nullptr || function == native_v1);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (unsigned i = 0; i < 64u; ++i) {
    memory.write32_be(kCode, 0x60000100u + i);
    (void)concurrent_cache.register_current(memory, kCode, kCode, 4u,
                                            native_v1);
  }
  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();

  std::cout << "Memory V2 Phase 19 executable/SMC tests passed\n";
  return 0;
}
