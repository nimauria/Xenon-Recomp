// Every vector case of the xenia-project/xenia PPC instruction tests, run as
// real generated code and in the dynamic-fallback interpreter, against the
// register and memory values recorded on Xbox 360 hardware
// (tests/cpu/xenia_ppc). Each mismatch is reported by case, path and value.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/memory/address_space.hpp"
#include "../xenia_ppc/xenia_ppc_corpus.hpp"

namespace {
#include "../xenia_ppc/xenia_ppc_vector_corpus.inc"
#include "../xenia_ppc/xenon_vmx128_operands.inc"
const XeniaPpcCorpus kCorpora[] = {{kXeniaPpcCases, kXeniaPpcCaseCount},
                                   {kXenonPpcCases, kXenonPpcCasesCount}};
}  // namespace

using namespace xenon::cpu;

struct XeniaPpcCompiled {
  ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&);
  const char* failure;
};
extern const XeniaPpcCompiled kXeniaPpcCompiled[];

namespace {

constexpr std::uint32_t kCode = 0x00640000u;
constexpr std::uint32_t kData = 0x10000000u;
constexpr std::uint32_t kDataSize = 0x10000u;

std::uint32_t whole_cr(const CpuState& state) {
  std::uint32_t cr = 0;
  for (unsigned field = 0; field < 8; ++field)
    cr |= std::uint32_t(state.cr_field(field) & 0xFu) << (28u - 4u * field);
  return cr;
}

void apply_inputs(const XeniaPpcCase& c, CpuState& state, xenon::memory::AddressSpace& guest) {
  for (std::uint32_t a = kData; a < kData + kDataSize; a += 4u) guest.write32_be(a, 0u);
  for (unsigned i = 0; i < c.memory_in_count; ++i)
    for (unsigned b = 0; b < c.memory_in[i].size; ++b)
      guest.write8(c.memory_in[i].address + b, c.memory_in[i].bytes[b]);
  for (unsigned i = 0; i < c.in_count; ++i) {
    const auto& v = c.in[i];
    if (v.kind == 'r') state.gpr[v.index] = v.scalar;
    if (v.kind == 'f') state.set_fpr(v.index, v.real);
    if (v.kind == 'v')
      for (unsigned lane = 0; lane < 4; ++lane) state.vr[v.index].set_u32_be(lane, v.lanes[lane]);
  }
}

unsigned check(const char* path, const XeniaPpcCase& c, const CpuState& state,
               xenon::memory::AddressSpace& guest) {
  unsigned failures = 0;
  for (unsigned i = 0; i < c.out_count; ++i) {
    const auto& v = c.out[i];
    if (v.kind == 'r' && state.gpr[v.index] != v.scalar) {
      std::fprintf(stderr, "%s %s: r%u = %016llX, want %016llX\n", path, c.name, v.index,
                   static_cast<unsigned long long>(state.gpr[v.index]),
                   static_cast<unsigned long long>(v.scalar));
      ++failures;
    }
    if (v.kind == 'f' && !(state.fpr(v.index) == v.real)) {
      std::fprintf(stderr, "%s %s: f%u = %.17g, want %.17g\n", path, c.name, v.index,
                   state.fpr(v.index), v.real);
      ++failures;
    }
    if (v.kind == 'c' && whole_cr(state) != std::uint32_t(v.scalar)) {
      std::fprintf(stderr, "%s %s: cr = %08X, want %08X\n", path, c.name, whole_cr(state),
                   unsigned(v.scalar));
      ++failures;
    }
    if (v.kind == 'v') {
      bool same = true;
      for (unsigned lane = 0; lane < 4; ++lane) same &= state.vr[v.index].u32_be(lane) == v.lanes[lane];
      if (!same) {
        std::fprintf(stderr, "%s %s: v%u = %08X %08X %08X %08X, want %08X %08X %08X %08X\n", path,
                     c.name, v.index, state.vr[v.index].u32_be(0), state.vr[v.index].u32_be(1),
                     state.vr[v.index].u32_be(2), state.vr[v.index].u32_be(3), v.lanes[0],
                     v.lanes[1], v.lanes[2], v.lanes[3]);
        ++failures;
      }
    }
  }
  for (unsigned i = 0; i < c.memory_out_count; ++i) {
    const auto& m = c.memory_out[i];
    for (unsigned b = 0; b < m.size; ++b) {
      const auto got = guest.read8(m.address + b);
      if (got != m.bytes[b]) {
        std::fprintf(stderr, "%s %s: memory %08X = %02X, want %02X\n", path, c.name,
                     m.address + b, unsigned(got), unsigned(m.bytes[b]));
        ++failures;
        break;
      }
    }
  }
  return failures;
}

}  // namespace

int main() {
  NullRuntimeServices runtime;
  xenon::memory::AddressSpace guest;
  if (!guest.initialize() ||
      !guest.commit_fixed(kCode, xenon::memory::kBasePageSize, xenon::memory::kReadWriteExecute) ||
      !guest.commit_fixed(kData, kDataSize, xenon::memory::kReadWrite))
    return 2;
  unsigned failed_cases = 0, unlifted = 0, total = 0;
  unsigned n = 0;
  for (const auto& corpus : kCorpora)
  for (unsigned k = 0; k < corpus.count; ++k, ++n) {
    const auto& c = corpus.cases[k];
    const auto& compiled = kXeniaPpcCompiled[n];
    ++total;
    unsigned failures = 0;
    if (!compiled.fn) {
      std::fprintf(stderr, "native %s: %s\n", c.name, compiled.failure);
      ++unlifted;
      ++failures;
    } else {
      CpuState native{};
      apply_inputs(c, native, guest);
      compiled.fn(native, guest, runtime);
      failures += check("native", c, native, guest);
    }

    CpuState interpreted{};
    apply_inputs(c, interpreted, guest);
    for (unsigned i = 0; i < c.word_count; ++i) guest.write32_be(kCode + i * 4u, c.words[i]);
    interpreted.lr = kCode + 0xF000u;
    DynamicFallbackExecutor fallback;
    ExecutionContext context(interpreted, guest, runtime);
    fallback.bind(context);
    const auto result = context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
    if (!result.handled || result.result.reason != FlowReason::Return) {
      std::fprintf(stderr, "fallback %s: did not run to its blr\n", c.name);
      ++failures;
    } else {
      failures += check("fallback", c, interpreted, guest);
    }
    if (failures) ++failed_cases;
  }
  std::cout << "xenon_cpu_xenia_ppc: " << total - failed_cases << " of " << total
            << " cases match (" << kXeniaPpcCaseCount << " recorded on hardware, "
            << kXenonPpcCasesCount << " VMX128 operand cases; " << unlifted << " not lifted)\n";
  return failed_cases ? 1 : 0;
}
