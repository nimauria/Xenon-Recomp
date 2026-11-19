// Proves the register-range RuntimeHelperKind native implementations
// (analysis::RuntimeHelperKind::{Save,Restore}{GprLr,Fpr,Vmx,Vmx128}) carry
// out the real PPC ABI-visible chain end to end, mirroring
// tests/cpu/x86_64/setjmp_longjmp.cpp's direct-call style rather than a full
// XEX/codegen round trip (these helpers dispatch straight to native code -
// see driver.cpp's runtime-helper discovery short-circuit - so no compiled
// guest code is needed to prove their guest-visible behavior):
//
//   guest registers initialized
//        -> register-save helper executes
//        -> guest stack/memory contains correct values
//        -> registers modified
//        -> restore helper executes
//        -> original values restored
//
// Exercises the lowest, a middle, and the highest supported register-start
// variant for each family, plus LR/big-endian guest memory layout for the
// GPR family and the r12-relative (not r1) addressing the real VMX/VMX128
// convention uses.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/cpu/runtime.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"
#include "xenon/recomp/native_replacements.hpp"
#include "xenon/recomp/runtime_helpers.hpp"

using namespace xenon::cpu;
using namespace xenon::recomp::runtime_helpers;

namespace {

class TestRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress target, CpuState&, MemoryPort&) override {
    return {FlowReason::Branch, target, 0u};
  }
  ExecutionResult syscall(std::uint32_t level, CpuState& state, MemoryPort&) override {
    return {FlowReason::Syscall, state.cia + 4u, level};
  }
  ExecutionResult trap(std::uint32_t code, CpuState& state, MemoryPort&) override {
    return {FlowReason::Trap, state.cia, code};
  }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0u; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState& state) override { return state.time_base; }
};

// GPR/LR family: entered via `bl` (save) / tail `b` (restore). r12 (not r0 -
// confirmed against the real AC6 binary's `mfspr r12,LR` / `stw r12,-8(r1)`
// save sequence) carries the caller's own mflr'd LR value into save; restore
// must load the STACK value back into LR (not whatever is currently live),
// then return through it - proving the two are genuinely independent, not
// aliased.
template <std::uint32_t RegisterStart>
void test_gpr_lr(xenon::memory::AddressSpace& memory, TestRuntime& runtime,
                  std::uint64_t stack_pointer) {
  CpuState state{};
  state.gpr[1] = stack_pointer;
  constexpr std::uint64_t kOriginalCallerLr = 0x80010004ull;  // what save_v2 must return to
  state.gpr[12] = kOriginalCallerLr;                          // mflr'd by the caller before `bl`
  state.lr = 0x80010004ull;  // the `bl`'s own return-into-caller address
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    state.gpr[reg] = 0x1000000000000000ull | reg;
  const auto original = state.gpr;  // full snapshot for later comparison

  ExecutionContext save_context(state, memory, runtime);
  const auto save_result = save_gpr_lr_v2<RegisterStart>(save_context);
  assert(save_result.reason == FlowReason::Return);
  assert(save_result.next_address == kOriginalCallerLr &&
         "save must return into the caller's remaining prologue, not clobber flow");

  // Guest stack memory now holds every saved register at the exact
  // real-toolchain-confirmed offset, plus the LR slot.
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    const auto offset = -0x98 + static_cast<std::int32_t>((reg - 14u) * 8u);
    const auto address = static_cast<GuestAddress>(stack_pointer + offset);
    assert(memory.read64_be(address) == original[reg]);
  }
  // The LR slot is a plain 32-bit word (confirmed against the real AC6
  // binary's paired `stw`/`lwz -8(r1)` sequence), not a 64-bit field like
  // the GPR slots above it.
  assert(memory.read32_be(static_cast<GuestAddress>(stack_pointer - 0x08)) ==
         static_cast<std::uint32_t>(kOriginalCallerLr));

  // Clobber every saved register and LR to prove restore genuinely reads
  // back from guest memory rather than trusting live state.
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) state.gpr[reg] = 0xBAADF00Dull;
  state.lr = 0xBAADF00Dull;

  ExecutionContext restore_context(state, memory, runtime);
  const auto restore_result = restore_gpr_lr_v2<RegisterStart>(restore_context);
  assert(restore_result.reason == FlowReason::Return);
  assert(restore_result.next_address == kOriginalCallerLr &&
         "restore is a tail substitute for the epilogue - it must return to the ORIGINAL "
         "caller, recovered from the stack, not whatever LR happened to hold live");
  assert(state.lr == kOriginalCallerLr);
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    assert(state.gpr[reg] == original[reg] && "restore must reproduce the exact saved value");
}

template <std::uint32_t RegisterStart>
void test_fpr(xenon::memory::AddressSpace& memory, TestRuntime& runtime,
              std::uint64_t stack_pointer) {
  CpuState state{};
  state.gpr[1] = stack_pointer;
  state.lr = 0x80020008ull;
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    state.fpr_bits[reg] = 0xF00D000000000000ull | reg;
  const auto original = state.fpr_bits;

  ExecutionContext save_context(state, memory, runtime);
  const auto save_result = save_fpr_v2<RegisterStart>(save_context);
  assert(save_result.reason == FlowReason::Return && save_result.next_address == state.lr);
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    assert(memory.read64_be(static_cast<GuestAddress>(
               stack_pointer + kFprBaseOffset + static_cast<std::int32_t>((reg - 14u) * 8u))) ==
           original[reg]);

  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) state.fpr_bits[reg] = 0xDEADBEEFull;
  ExecutionContext restore_context(state, memory, runtime);
  const auto restore_result = restore_fpr_v2<RegisterStart>(restore_context);
  assert(restore_result.reason == FlowReason::Return);
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    assert(state.fpr_bits[reg] == original[reg]);
}

// VMX (v14..v31): confirmed real convention addresses relative to r12, NOT
// r1 - proven here by putting a poison value in r1 that would make the test
// fail if the implementation used the wrong base register.
template <std::uint32_t RegisterStart>
void test_vmx(xenon::memory::AddressSpace& memory, TestRuntime& runtime,
              std::uint64_t frame_pointer) {
  CpuState state{};
  state.gpr[1] = 0xFFFFFFFFFFFF0000ull;  // poison: must never be used as the VMX base
  state.gpr[12] = frame_pointer;
  state.lr = 0x80030010ull;
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    for (std::size_t byte = 0; byte < state.vr[reg].bytes.size(); ++byte)
      state.vr[reg].bytes[byte] = static_cast<std::uint8_t>(reg * 3u + byte);
  const auto original = state.vr;

  ExecutionContext save_context(state, memory, runtime);
  const auto save_result = save_vmx_v2<RegisterStart>(save_context);
  assert(save_result.reason == FlowReason::Return && save_result.next_address == state.lr);
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg) {
    const auto address = static_cast<GuestAddress>(
        frame_pointer + kVmxBaseOffset + static_cast<std::int32_t>((reg - 14u) * 16u));
    assert(memory.read128(address).bytes == original[reg].bytes);
  }

  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    for (auto& byte : state.vr[reg].bytes) byte = 0xEEu;
  ExecutionContext restore_context(state, memory, runtime);
  const auto restore_result = restore_vmx_v2<RegisterStart>(restore_context);
  assert(restore_result.reason == FlowReason::Return);
  for (std::uint32_t reg = RegisterStart; reg <= 31u; ++reg)
    assert(state.vr[reg].bytes == original[reg].bytes);
}

// Extended VMX128 (v64..v127): same r12-relative convention, different base
// offset and register range.
template <std::uint32_t RegisterStart>
void test_vmx128(xenon::memory::AddressSpace& memory, TestRuntime& runtime,
                 std::uint64_t frame_pointer) {
  CpuState state{};
  state.gpr[1] = 0xFFFFFFFFFFFF0000ull;
  state.gpr[12] = frame_pointer;
  state.lr = 0x80040020ull;
  for (std::uint32_t reg = RegisterStart; reg <= 127u; ++reg)
    for (std::size_t byte = 0; byte < state.vr[reg].bytes.size(); ++byte)
      state.vr[reg].bytes[byte] = static_cast<std::uint8_t>(reg * 5u + byte);
  const auto original = state.vr;

  ExecutionContext save_context(state, memory, runtime);
  const auto save_result = save_vmx128_v2<RegisterStart>(save_context);
  assert(save_result.reason == FlowReason::Return && save_result.next_address == state.lr);
  for (std::uint32_t reg = RegisterStart; reg <= 127u; ++reg) {
    const auto address = static_cast<GuestAddress>(
        frame_pointer + kVmx128BaseOffset + static_cast<std::int32_t>((reg - 64u) * 16u));
    assert(memory.read128(address).bytes == original[reg].bytes);
  }

  for (std::uint32_t reg = RegisterStart; reg <= 127u; ++reg)
    for (auto& byte : state.vr[reg].bytes) byte = 0x11u;
  ExecutionContext restore_context(state, memory, runtime);
  const auto restore_result = restore_vmx128_v2<RegisterStart>(restore_context);
  assert(restore_result.reason == FlowReason::Return);
  for (std::uint32_t reg = RegisterStart; reg <= 127u; ++reg)
    assert(state.vr[reg].bytes == original[reg].bytes);
}

// Forwards to an AddressSpace and counts every access made through the
// virtual MemoryPort interface; the access context it hands out is the
// address space's own, which never calls back into this port.
class CountingPort final : public MemoryPort {
 public:
  explicit CountingPort(xenon::memory::AddressSpace& inner) : inner_(inner) {}
  MemoryAccessContext access_context() noexcept override { return inner_.access_context(); }
  std::uint8_t read8(GuestAddress a) override { ++accesses; return inner_.read8(a); }
  std::uint16_t read16_be(GuestAddress a) override { ++accesses; return inner_.read16_be(a); }
  std::uint32_t read32_be(GuestAddress a) override { ++accesses; return inner_.read32_be(a); }
  std::uint64_t read64_be(GuestAddress a) override { ++accesses; return inner_.read64_be(a); }
  Vector128 read128(GuestAddress a) override { ++accesses; return inner_.read128(a); }
  void write8(GuestAddress a, std::uint8_t v) override { ++accesses; inner_.write8(a, v); }
  void write16_be(GuestAddress a, std::uint16_t v) override { ++accesses; inner_.write16_be(a, v); }
  void write32_be(GuestAddress a, std::uint32_t v) override { ++accesses; inner_.write32_be(a, v); }
  void write64_be(GuestAddress a, std::uint64_t v) override { ++accesses; inner_.write64_be(a, v); }
  void write128(GuestAddress a, const Vector128& v) override { ++accesses; inner_.write128(a, v); }
  std::uint16_t read16_le(GuestAddress a) override { ++accesses; return inner_.read16_le(a); }
  std::uint32_t read32_le(GuestAddress a) override { ++accesses; return inner_.read32_le(a); }
  std::uint64_t read64_le(GuestAddress a) override { ++accesses; return inner_.read64_le(a); }
  void write16_le(GuestAddress a, std::uint16_t v) override { ++accesses; inner_.write16_le(a, v); }
  void write32_le(GuestAddress a, std::uint32_t v) override { ++accesses; inner_.write32_le(a, v); }
  void write64_le(GuestAddress a, std::uint64_t v) override { ++accesses; inner_.write64_le(a, v); }
  void read_bytes(GuestAddress a, std::span<std::byte> d) override { ++accesses; inner_.read_bytes(a, d); }
  void write_bytes(GuestAddress a, std::span<const std::byte> s) override {
    ++accesses;
    inner_.write_bytes(a, s);
  }
  void fill_bytes(GuestAddress a, std::uint32_t n, std::uint8_t v) override {
    ++accesses;
    inner_.fill_bytes(a, n, v);
  }
  std::uint64_t reserve32(GuestAddress a, std::uint32_t& v) override { ++accesses; return inner_.reserve32(a, v); }
  std::uint64_t reserve64(GuestAddress a, std::uint64_t& v) override { ++accesses; return inner_.reserve64(a, v); }
  bool store_conditional32(GuestAddress a, std::uint64_t t, std::uint32_t v) override {
    ++accesses;
    return inner_.store_conditional32(a, t, v);
  }
  bool store_conditional64(GuestAddress a, std::uint64_t t, std::uint64_t v) override {
    ++accesses;
    return inner_.store_conditional64(a, t, v);
  }
  void barrier(BarrierKind kind) override { inner_.barrier(kind); }
  void zero_cache_block(GuestAddress a, std::uint32_t n) override { ++accesses; inner_.zero_cache_block(a, n); }
  void instruction_cache_invalidate(GuestAddress a) override { inner_.instruction_cache_invalidate(a); }

  std::uint64_t accesses{};

 private:
  xenon::memory::AddressSpace& inner_;
};

// Regression (AC6 spent most of its remaining startup CPU in AddressSpace::
// access_context): the prologue/epilogue spill helpers, setjmp/longjmp and
// the CRT replacements run in nearly every guest call, and each of their
// accesses went through the virtual MemoryPort, which builds a fresh access
// context per access. They must use the calling context's own memory_access.
void test_helpers_use_the_context_memory_access(xenon::memory::AddressSpace& memory,
                                                TestRuntime& runtime, std::uint64_t frame) {
  namespace crt = xenon::recomp::native_replacements;
  CountingPort port(memory);
  CpuState state{};
  state.gpr[1] = frame;
  state.gpr[12] = 0x80010004ull;
  state.lr = 0x80010004ull;
  for (std::uint32_t reg = 14u; reg <= 31u; ++reg) {
    state.gpr[reg] = 0x4000000000000000ull | reg;
    state.fpr_bits[reg] = 0x5000000000000000ull | reg;
  }
  const auto gprs = state.gpr;
  const auto run = [&](NativeCompiledEntry entry) {
    ExecutionContext context(state, port, runtime);
    return entry(context);
  };
  run(&save_gpr_lr_v2<14>);
  run(&save_fpr_v2<14>);
  for (std::uint32_t reg = 14u; reg <= 31u; ++reg) state.gpr[reg] = 0u;
  run(&restore_gpr_lr_v2<14>);
  run(&restore_fpr_v2<14>);
  for (std::uint32_t reg = 14u; reg <= 31u; ++reg) assert(state.gpr[reg] == gprs[reg]);
  state.gpr[12] = frame - 0x800ull;
  run(&save_vmx_v2<14>);
  run(&restore_vmx_v2<14>);
  run(&save_vmx128_v2<64>);
  run(&restore_vmx128_v2<64>);

  // CRT replacements: a string copied with memcpy, measured, compared and
  // copied again with strncpy, then cleared with memset.
  const auto text = static_cast<GuestAddress>(frame + 0x100u);
  const auto copy = static_cast<GuestAddress>(frame + 0x200u);
  const char hello[] = "xenon";
  for (std::uint32_t i = 0; i < sizeof(hello); ++i)
    memory.write8(text + i, static_cast<std::uint8_t>(hello[i]));
  state.lr = 0x80020000ull;
  state.gpr[3] = copy; state.gpr[4] = text; state.gpr[5] = sizeof(hello);
  run(&crt::memcpy_v2);
  state.gpr[3] = copy;
  run(&crt::strlen_v2);
  assert(state.gpr[3] == 5u);
  state.gpr[3] = text; state.gpr[4] = copy; state.gpr[5] = 8u;
  run(&crt::strncmp_v2);
  assert(state.gpr[3] == 0u);
  state.gpr[3] = copy + 0x20u; state.gpr[4] = text; state.gpr[5] = 8u;
  run(&crt::strncpy_v2);
  assert(memory.read8(copy + 0x24u) == 'n' && memory.read8(copy + 0x27u) == 0u);
  state.gpr[3] = copy; state.gpr[4] = 0u; state.gpr[5] = 16u;
  run(&crt::memset_v2);
  assert(memory.read8(copy) == 0u && memory.read8(copy + 4u) == 0u);

  assert(port.accesses == 0u && "helpers must not go through the virtual MemoryPort");
}

}  // namespace

int main() {
  auto address_space = std::make_shared<xenon::memory::AddressSpace>(
      xenon::memory::GuestTranslationMode::Compact);
  assert(address_space->initialize());
  auto memory = std::make_shared<xenon::kernel::KernelMemory>(address_space);
  xenon::kernel::KernelProcess process(memory);
  TestRuntime runtime;

  std::uint32_t stack = 0;
  assert(memory->allocate_virtual(stack, 0x10000u, xenon::memory::kReadWrite));
  const std::uint64_t frame = stack + 0x8000u;

  // Lowest, a middle, and the highest supported register-start variant, per
  // Part 6.
  test_gpr_lr<14>(*address_space, runtime, frame);
  test_gpr_lr<20>(*address_space, runtime, frame);
  test_gpr_lr<31>(*address_space, runtime, frame);

  test_fpr<14>(*address_space, runtime, frame);
  test_fpr<25>(*address_space, runtime, frame);
  test_fpr<31>(*address_space, runtime, frame);

  test_vmx<14>(*address_space, runtime, frame);
  test_vmx<22>(*address_space, runtime, frame);
  test_vmx<31>(*address_space, runtime, frame);

  test_vmx128<64>(*address_space, runtime, frame);
  test_vmx128<96>(*address_space, runtime, frame);
  test_vmx128<127>(*address_space, runtime, frame);

  // Nested/combined use: a single call frame concurrently holds valid GPR,
  // FPR and VMX spill data without one family corrupting another's slots
  // (the real-world case of a function using more than one register class).
  // r12 (the VMX/VMX128 base) is deliberately kept well clear of r1 (the
  // GPR/FPR base) here: these are fixed, shared helper routines reused by
  // every function in a real title, so whatever real relationship the
  // compiler maintains between r1 and r12 must itself avoid this exact
  // collision for its own frame layout to be sound - this test only needs
  // to prove Xenon's own read/write offsets for each family stay isolated
  // from one another when the two base registers are, in fact, distinct.
  //
  // r12 is also the GPR/LR family's own transient LR-carry register (see
  // test_gpr_lr above) - real compiled functions only hold r12 = mflr'd LR
  // for the brief window of the `bl __savegprlr_N`/`bl __restgprlr_N` call
  // itself, then immediately repurpose it as the VMX base for subsequent
  // vector spills. This test reproduces that same real ordering rather than
  // holding one fixed r12 value across every call.
  {
    CpuState state{};
    state.gpr[1] = frame;
    constexpr std::uint64_t kOriginalCallerLr = 0x80050000ull;
    const auto vmx_base = frame - 0x800ull;
    state.lr = kOriginalCallerLr;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) state.gpr[reg] = 0x2000000000000000ull | reg;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) state.fpr_bits[reg] = 0x3000000000000000ull | reg;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg)
      for (auto& byte : state.vr[reg].bytes) byte = static_cast<std::uint8_t>(reg);
    const auto gpr_before = state.gpr;
    const auto fpr_before = state.fpr_bits;
    const auto vmx_before = state.vr;

    state.gpr[12] = kOriginalCallerLr;  // mflr'd, transient, for this call only
    ExecutionContext gpr_save(state, *address_space, runtime);
    save_gpr_lr_v2<20>(gpr_save);
    ExecutionContext fpr_save(state, *address_space, runtime);
    save_fpr_v2<20>(fpr_save);
    state.gpr[12] = vmx_base;  // repurposed as the VMX base, as real code does
    ExecutionContext vmx_save(state, *address_space, runtime);
    save_vmx_v2<20>(vmx_save);

    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) state.gpr[reg] = 0;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) state.fpr_bits[reg] = 0;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg)
      for (auto& byte : state.vr[reg].bytes) byte = 0;

    ExecutionContext gpr_restore(state, *address_space, runtime);
    restore_gpr_lr_v2<20>(gpr_restore);
    ExecutionContext fpr_restore(state, *address_space, runtime);
    restore_fpr_v2<20>(fpr_restore);
    state.gpr[12] = vmx_base;  // still the VMX base at this point in the epilogue
    ExecutionContext vmx_restore(state, *address_space, runtime);
    restore_vmx_v2<20>(vmx_restore);

    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) {
      assert(state.gpr[reg] == gpr_before[reg]);
      assert(state.fpr_bits[reg] == fpr_before[reg]);
      assert(state.vr[reg].bytes == vmx_before[reg].bytes);
    }
  }

  test_helpers_use_the_context_memory_access(*address_space, runtime, frame);

  process.terminate(0u);
  std::cout << "xenon_cpu_runtime_helpers_register_range: ok\n";
  return 0;
}
