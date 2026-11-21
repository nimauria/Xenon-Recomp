// Runs codegen/codegen_indirect_cache.cpp's indirect call and tail branch
// with a counting compiled-lookup: the same target repeatedly costs one
// lookup, alternating targets resolve correctly each time, a generation bump
// (or another lookup callback) forces a fresh lookup whose answer is used,
// and a lookup miss reaches the fallback path and is never cached.

#include <atomic>
#include <cassert>
#include <cstdint>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"

using namespace xenon::cpu;

#define XENON_IC(name)                                   \
  ExecutionResult name##_promoted_v2(ExecutionContext&); \
  ExecutionResult name##_plain_v2(ExecutionContext&);    \
  ExecutionResult name##_uncached_v2(ExecutionContext&);
XENON_IC(ic_call)
XENON_IC(ic_tail)
#undef XENON_IC

namespace {

constexpr GuestAddress kA = 0x20000u;
constexpr GuestAddress kB = 0x20100u;
constexpr GuestAddress kMiss = 0x20200u;
constexpr GuestAddress kReturn = 0x10008u;  // the instruction after the bctrl

// Each callee adds its own constant to r5 and returns through lr.
ExecutionResult callee_a(ExecutionContext& c) {
  c.state.gpr[5] += 1u;
  return {FlowReason::Return, static_cast<GuestAddress>(c.state.lr & ~3ull), 0u};
}
ExecutionResult callee_b(ExecutionContext& c) {
  c.state.gpr[5] += 100u;
  return {FlowReason::Return, static_cast<GuestAddress>(c.state.lr & ~3ull), 0u};
}
ExecutionResult callee_a2(ExecutionContext& c) {
  c.state.gpr[5] += 10000u;
  return {FlowReason::Return, static_cast<GuestAddress>(c.state.lr & ~3ull), 0u};
}

NativeCompiledEntry g_a = &callee_a;  // what kA currently resolves to
unsigned g_lookups = 0;
unsigned g_misses = 0;

NativeCompiledEntry counting_lookup(void*, ExecutionContext&, GuestAddress target, CompiledLookupKind) {
  ++g_lookups;
  if (target == kA) return g_a;
  if (target == kB) return &callee_b;
  return nullptr;
}
// A second lookup callback that answers differently for kA.
unsigned g_lookups_other = 0;
NativeCompiledEntry other_lookup(void*, ExecutionContext&, GuestAddress target, CompiledLookupKind) {
  ++g_lookups_other;
  return target == kA ? &callee_a2 : nullptr;
}
void miss_observer(void*, ExecutionContext&, GuestAddress, CompiledLookupKind) { ++g_misses; }

class Runtime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress target, CpuState& state, MemoryPort&) override {
    ++calls;
    last_call = target;
    state.gpr[5] += 1000000u;
    return {FlowReason::Fallthrough, target, 0};
  }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult trap(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
  unsigned calls{};
  GuestAddress last_call{};
};

struct Rig {
  FlatMemory memory{0x1000u, 0x1000u};
  Runtime runtime;
  CpuState state{};
  std::atomic<std::uint64_t> generation{1u};
  ExecutionContext context{state, memory, runtime};
  explicit Rig(bool with_generation = true, CompiledLookupCallback lookup = &counting_lookup) {
    context.compiled_lookup = lookup;
    context.compiled_lookup_miss = &miss_observer;
    if (with_generation) context.dispatch_generation = &generation;
  }
  // One indirect call through `fn`: returns what r5 gained.
  std::uint64_t call(ExecutionResult (*fn)(ExecutionContext&), GuestAddress target) {
    state.gpr[3] = target;
    state.gpr[5] = 0;
    state.lr = 0x9000u;
    const auto r = fn(context);
    // The callee returns to the instruction after the bctrl; the blr then
    // returns to LR, which the bctrl set to exactly that address.
    assert(r.reason == FlowReason::Return && r.next_address == kReturn);
    return state.gpr[5];
  }
  // One tail branch (`bctr`) through `fn`.
  ExecutionResult tail(ExecutionResult (*fn)(ExecutionContext&), GuestAddress target) {
    state.gpr[3] = target;
    state.gpr[5] = 0;
    state.lr = 0x7000u;
    return fn(context);
  }
};

void exercise_call(ExecutionResult (*fn)(ExecutionContext&)) {
  g_a = &callee_a;
  g_lookups = 0;
  g_misses = 0;
  Rig rig;
  // The same target repeatedly: one lookup.
  for (int i = 0; i < 6; ++i) assert(rig.call(fn, kA) == 1u);
  assert(g_lookups == 1u);
  // Alternating targets: the right callee each time. A hit costs nothing,
  // every switch one lookup (a one-entry cache).
  g_lookups = 0;
  assert(rig.call(fn, kA) == 1u);    // hit
  assert(rig.call(fn, kB) == 100u);  // lookup
  assert(rig.call(fn, kA) == 1u);    // lookup
  assert(rig.call(fn, kB) == 100u);  // lookup
  assert(rig.call(fn, kB) == 100u);  // hit
  assert(g_lookups == 3u);
  // Until the generation moves, the cached answer stands even though the
  // registry now says otherwise; after the bump the new answer is used.
  assert(rig.call(fn, kA) == 1u);
  g_a = &callee_a2;
  g_lookups = 0;
  assert(rig.call(fn, kA) == 1u && g_lookups == 0u);
  rig.generation.fetch_add(1u);
  assert(rig.call(fn, kA) == 10000u && g_lookups == 1u);
  assert(rig.call(fn, kA) == 10000u && g_lookups == 1u);  // cached under the new generation
  // A miss reaches the fallback (runtime.call) every time and is never cached.
  g_lookups = 0;
  g_misses = 0;
  const auto calls_before = rig.runtime.calls;
  assert(rig.call(fn, kMiss) == 1000000u);
  assert(rig.call(fn, kMiss) == 1000000u);
  assert(g_lookups == 2u && g_misses == 2u && rig.runtime.calls == calls_before + 2u);
  assert(rig.runtime.last_call == kMiss);
  assert(rig.call(fn, kA) == 10000u);  // a hit after the misses still resolves
  // A context with another lookup callback never uses this site's cache.
  {
    Rig other(true, &other_lookup);
    g_lookups_other = 0;
    assert(other.call(fn, kA) == 10000u && g_lookups_other == 1u);
    assert(other.call(fn, kA) == 10000u && g_lookups_other == 1u);
    g_a = &callee_a;
    g_lookups = 0;
    assert(rig.call(fn, kA) == 1u && g_lookups == 1u);  // not the other callback's entry
  }
  // A context without a dispatch generation looks up on every call.
  {
    Rig plain(false);
    g_lookups = 0;
    for (int i = 0; i < 4; ++i) assert(plain.call(fn, kA) == 1u);
    assert(g_lookups == 4u);
  }
}

// The tail branch (`bctr`): the callee's result is the function's result.
void exercise_tail(ExecutionResult (*fn)(ExecutionContext&)) {
  g_a = &callee_a;
  g_lookups = 0;
  g_misses = 0;
  Rig rig;
  for (int i = 0; i < 4; ++i) {
    const auto r = rig.tail(fn, kA);
    assert(r.reason == FlowReason::Return && r.next_address == 0x7000u && rig.state.gpr[5] == 1u);
  }
  assert(g_lookups == 1u);
  {
    const auto r = rig.tail(fn, kB);
    assert(r.reason == FlowReason::Return && rig.state.gpr[5] == 100u);
  }
  g_a = &callee_a2;
  rig.generation.fetch_add(1u);
  {
    const auto r = rig.tail(fn, kA);
    assert(r.reason == FlowReason::Return && rig.state.gpr[5] == 10000u);
  }
  // A miss is handed back as a Branch, every time, with a lookup each time.
  g_lookups = 0;
  g_misses = 0;
  for (int i = 0; i < 2; ++i) {
    const auto r = rig.tail(fn, kMiss);
    assert(r.reason == FlowReason::Branch && r.next_address == kMiss);
  }
  assert(g_lookups == 2u && g_misses == 2u);
}

// The control: without the cache every call looks up.
void exercise_uncached_call(ExecutionResult (*fn)(ExecutionContext&)) {
  g_a = &callee_a;
  g_lookups = 0;
  Rig rig;
  for (int i = 0; i < 5; ++i) assert(rig.call(fn, kA) == 1u);
  assert(g_lookups == 5u && "without the cache every call looks up");
}
void exercise_uncached_tail(ExecutionResult (*fn)(ExecutionContext&)) {
  g_a = &callee_a;
  g_lookups = 0;
  Rig rig;
  for (int i = 0; i < 5; ++i) {
    const auto r = rig.tail(fn, kA);
    assert(r.reason == FlowReason::Return && rig.state.gpr[5] == 1u);
  }
  assert(g_lookups == 5u);
}

}  // namespace

int main() {
  exercise_call(&ic_call_promoted_v2);
  exercise_call(&ic_call_plain_v2);
  exercise_uncached_call(&ic_call_uncached_v2);
  exercise_tail(&ic_tail_promoted_v2);
  exercise_tail(&ic_tail_plain_v2);
  exercise_uncached_tail(&ic_tail_uncached_v2);
  return 0;
}
