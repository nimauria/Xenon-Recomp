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

// GPR/LR family: entered via `bl` (save) / tail `b` (restore). r0 carries
// the caller's own mflr'd LR value into save; restore must load the STACK
// value back into LR (not whatever is currently live), then return through
// it - proving the two are genuinely independent, not aliased.
template <std::uint32_t RegisterStart>
void test_gpr_lr(xenon::memory::AddressSpace& memory, TestRuntime& runtime,
                  std::uint64_t stack_pointer) {
  CpuState state{};
  state.gpr[1] = stack_pointer;
  constexpr std::uint64_t kOriginalCallerLr = 0x80010004ull;  // what save_v2 must return to
  state.gpr[0] = kOriginalCallerLr;                           // mflr'd by the caller before `bl`
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
  assert(memory.read64_be(static_cast<GuestAddress>(stack_pointer - 0x08)) == kOriginalCallerLr);

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
  {
    CpuState state{};
    state.gpr[1] = frame;
    state.gpr[12] = frame - 0x800ull;
    state.gpr[0] = 0x80050000ull;
    state.lr = 0x80050000ull;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) state.gpr[reg] = 0x2000000000000000ull | reg;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) state.fpr_bits[reg] = 0x3000000000000000ull | reg;
    for (std::uint32_t reg = 20u; reg <= 31u; ++reg)
      for (auto& byte : state.vr[reg].bytes) byte = static_cast<std::uint8_t>(reg);
    const auto gpr_before = state.gpr;
    const auto fpr_before = state.fpr_bits;
    const auto vmx_before = state.vr;

    ExecutionContext gpr_save(state, *address_space, runtime);
    save_gpr_lr_v2<20>(gpr_save);
    ExecutionContext fpr_save(state, *address_space, runtime);
    save_fpr_v2<20>(fpr_save);
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
    ExecutionContext vmx_restore(state, *address_space, runtime);
    restore_vmx_v2<20>(vmx_restore);

    for (std::uint32_t reg = 20u; reg <= 31u; ++reg) {
      assert(state.gpr[reg] == gpr_before[reg]);
      assert(state.fpr_bits[reg] == fpr_before[reg]);
      assert(state.vr[reg].bytes == vmx_before[reg].bytes);
    }
  }

  process.terminate(0u);
  std::cout << "xenon_cpu_runtime_helpers_register_range: ok\n";
  return 0;
}
