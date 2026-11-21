// Runs codegen/codegen_register_spills.cpp's scenarios: the same guest
// function once calling the register save/restore helper (plain) and once
// with the helper's loads and stores emitted inline on promoted locals, from
// identical registers and memory. Registers, memory, LR and the result must
// match - including when a load or store faults partway - and must be what
// the helper is specified to do.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#include "xenon/cpu/aot_semantics.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/recomp/runtime_helpers.hpp"

using namespace xenon::cpu;
namespace helpers = xenon::recomp::runtime_helpers;

struct SpillCase {
  const char* name;
  ExecutionResult (*call)(ExecutionContext&);
  ExecutionResult (*inlined)(ExecutionContext&);
  int family;
  int shape;
  unsigned first;
};
extern const SpillCase kSpillCases[];
extern const unsigned kSpillCaseCount;

namespace {

enum Family { kGpr = 0, kFpr = 1, kVmx = 2, kVmx128 = 3 };
enum Shape { kSave = 0, kRestore, kFrame, kCondCall, kCondTail };

constexpr GuestAddress kMemBase = 0x1000u;
constexpr std::size_t kMemSize = 0x2000u;
constexpr std::uint64_t kStack = 0x2800u;

class NullRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 1}; }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 2}; }
  ExecutionResult trap(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 3}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
};

struct Outcome {
  ExecutionResult result{};
  bool threw{};
  CpuState state{};
  std::vector<std::uint8_t> memory;
};

std::uint64_t mix(std::uint64_t x) {
  x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull; x ^= x >> 33;
  return x;
}

CpuState initial_state(bool eq) {
  CpuState state{};
  for (unsigned r = 0; r < 32u; ++r) {
    state.gpr[r] = mix(0x100u + r);
    state.fpr_bits[r] = mix(0x200u + r);
  }
  for (unsigned r = 0; r < 128u; ++r) {
    for (unsigned b = 0; b < 16u; ++b) state.vr[r].bytes[b] = static_cast<std::uint8_t>(mix(0x1000u + r * 16u + b));
  }
  state.gpr[1] = kStack;
  state.gpr[12] = kStack;
  state.lr = 0x9000u;
  state.set_cr_field(0, eq ? 0x2u : 0u);
  return state;
}

void fill_memory(FlatMemory& memory) {
  for (std::size_t i = 0; i < kMemSize; ++i)
    memory.write8(static_cast<GuestAddress>(kMemBase + i), static_cast<std::uint8_t>(mix(0x9000u + i)));
}

struct Params {
  bool eq{true};
  // When set: the helper's base register is placed so the slot of this
  // register straddles the end of memory and its access faults.
  bool fault{};
};

std::int32_t family_offset(int family) {
  switch (family) {
    case kGpr: return helpers::kGprLrBaseOffset;
    case kFpr: return helpers::kFprBaseOffset;
    case kVmx: return helpers::kVmxBaseOffset;
    default: return helpers::kVmx128BaseOffset;
  }
}
unsigned family_origin(int family) { return family == kVmx128 ? 64u : 14u; }
unsigned family_last(int family) { return family == kVmx128 ? 127u : 31u; }
unsigned family_width(int family) { return (family == kVmx || family == kVmx128) ? 16u : 8u; }
unsigned base_register(int family) { return (family == kVmx || family == kVmx128) ? 12u : 1u; }
GuestAddress slot(int family, std::uint64_t base, unsigned reg) {
  return helpers::effective_address(
      base, family_offset(family) + static_cast<std::int32_t>((reg - family_origin(family)) * family_width(family)));
}

Outcome run(const SpillCase& c, ExecutionResult (*fn)(ExecutionContext&), const Params& p) {
  FlatMemory memory(kMemSize, kMemBase);
  fill_memory(memory);
  NullRuntime runtime;
  Outcome outcome;
  outcome.state = initial_state(p.eq);
  auto& state = outcome.state;
  if (p.fault) {
    const unsigned k = std::min(c.first + 2u, family_last(c.family));
    const std::uint64_t target = kMemBase + kMemSize - 2u;
    const auto base = target - static_cast<std::uint64_t>(static_cast<std::int64_t>(family_offset(c.family)) +
                                                          static_cast<std::int64_t>((k - family_origin(c.family)) * family_width(c.family)));
    state.gpr[base_register(c.family)] = base;
  }
  ExecutionContext context(state, memory, runtime);
  try {
    outcome.result = fn(context);
  } catch (const std::exception&) {
    outcome.threw = true;
  }
  for (std::size_t i = 0; i < kMemSize; ++i) outcome.memory.push_back(memory.read8(static_cast<GuestAddress>(kMemBase + i)));
  return outcome;
}

void same(const char* name, const Outcome& a, const Outcome& b) {
  const auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "%s: %s differs between the helper call and the inlined spill\n", name, what);
      std::abort();
    }
  };
  check(a.threw == b.threw, "fault");
  if (!a.threw) {
    check(a.result.reason == b.result.reason && a.result.next_address == b.result.next_address &&
              a.result.detail == b.result.detail,
          "result");
  }
  check(a.state.gpr == b.state.gpr, "gpr");
  check(a.state.fpr_bits == b.state.fpr_bits, "fpr");
  bool vr = true;
  for (std::size_t i = 0; i < a.state.vr.size(); ++i) vr = vr && a.state.vr[i].bytes == b.state.vr[i].bytes;
  check(vr, "vr");
  check(a.state.lr == b.state.lr && a.state.cr == b.state.cr, "lr/cr");
  check(a.memory == b.memory, "memory");
}

}  // namespace

int main() {
  assert(kSpillCaseCount > 20u);
  for (unsigned n = 0; n < kSpillCaseCount; ++n) {
    const auto& c = kSpillCases[n];
    const bool simple = c.shape == kSave || c.shape == kRestore;
    for (const bool eq : {true, false}) {
      if (!(c.shape == kCondCall || c.shape == kCondTail) && !eq) continue;
      const auto a = run(c, c.call, {eq, false});
      const auto b = run(c, c.inlined, {eq, false});
      same(c.name, a, b);

      // The helper's specified effect, from the untouched initial state.
      const auto start = initial_state(eq);
      const bool does_save = c.shape == kSave || c.shape == kFrame || (c.shape == kCondCall && eq);
      const bool does_restore = c.shape == kRestore || c.shape == kFrame || (c.shape == kCondTail && eq);
      assert(!b.threw);
      FlatMemory fresh(kMemSize, kMemBase);
      fill_memory(fresh);
      const auto base = start.gpr[base_register(c.family)];
      for (unsigned reg = c.first; reg <= family_last(c.family); ++reg) {
        const auto address = slot(c.family, base, reg);
        if (c.shape == kFrame) {
          // Saved, clobbered by li (GPR only), restored.
          assert(b.state.gpr[reg] == start.gpr[reg]);
        } else if (does_save) {
          if (c.family == kGpr) assert(b.memory[address - kMemBase + 7u] == static_cast<std::uint8_t>(start.gpr[reg]));
          if (c.family == kGpr) assert(b.state.gpr[reg] == start.gpr[reg]);
          if (c.family == kFpr) assert(b.memory[address - kMemBase + 7u] == static_cast<std::uint8_t>(start.fpr_bits[reg]));
          if (c.family >= kVmx) assert(b.memory[address - kMemBase + 15u] == start.vr[reg].bytes[15]);
        } else if (does_restore) {
          if (c.family == kGpr) assert(b.state.gpr[reg] == fresh.read64_be(address));
          if (c.family == kFpr) assert(b.state.fpr_bits[reg] == fresh.read64_be(address));
          if (c.family >= kVmx) assert(b.state.vr[reg].bytes == fresh.read128(address).bytes);
        } else {
          // Skipped conditional helper: nothing changes.
          assert(b.state.gpr[reg] == start.gpr[reg] && b.state.fpr_bits[reg] == start.fpr_bits[reg]);
        }
      }
      // Registers below the first are never touched.
      for (unsigned reg = family_origin(c.family); reg < c.first; ++reg) {
        if (c.family == kGpr) assert(b.state.gpr[reg] == start.gpr[reg]);
        if (c.family == kFpr) assert(b.state.fpr_bits[reg] == start.fpr_bits[reg]);
        if (c.family >= kVmx) assert(b.state.vr[reg].bytes == start.vr[reg].bytes);
      }
      if (c.family == kGpr && does_save && c.shape != kFrame) {
        // The LR slot is the 32-bit word of r12 (mflr r12 made it the LR).
        assert(c.shape == kSave || c.shape == kCondCall);
        const auto lr_slot = helpers::effective_address(base, helpers::kGprLrLrOffset);
        const auto word = static_cast<std::uint32_t>(
            (b.memory[lr_slot - kMemBase] << 24) | (b.memory[lr_slot - kMemBase + 1] << 16) |
            (b.memory[lr_slot - kMemBase + 2] << 8) | b.memory[lr_slot - kMemBase + 3]);
        assert(word == 0x9000u);
      }
      if (c.family == kGpr && does_restore && c.shape != kFrame) {
        const auto lr_slot = helpers::effective_address(base, helpers::kGprLrLrOffset);
        assert(b.state.lr == fresh.read32_be(lr_slot));
        assert(b.result.reason == FlowReason::Return &&
               b.result.next_address == static_cast<GuestAddress>(b.state.lr & ~3ull));
      }
      if (c.shape == kFrame) {
        assert(b.result.reason == FlowReason::Return && b.result.next_address == 0x9000u && b.state.lr == 0x9000u);
      }
    }
    if (simple) {
      // A load or store that faults partway leaves the same registers and
      // memory behind either way.
      const auto a = run(c, c.call, {true, true});
      const auto b = run(c, c.inlined, {true, true});
      assert(a.threw && b.threw && "the straddling slot must fault");
      same(c.name, a, b);
    }
  }
  return 0;
}
