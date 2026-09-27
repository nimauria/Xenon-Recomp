#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/cpu/runtime.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"
#include "xenon/recomp/runtime_helpers.hpp"

using namespace xenon::cpu;

ExecutionResult setjmp_parent_v2(ExecutionContext&);
ExecutionResult setjmp_nested_v2(ExecutionContext&);

namespace {

constexpr GuestAddress kNested = 0x1100u;
constexpr GuestAddress kSetJmp = 0x1200u;
constexpr GuestAddress kLongJmp = 0x1300u;

class TestRuntime final : public RuntimeServices {
 public:
  explicit TestRuntime(xenon::kernel::KernelProcess& process) : process_(process) {}

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
  xenon::kernel::KernelProcess* current_process() noexcept override { return &process_; }

 private:
  xenon::kernel::KernelProcess& process_;
};

NativeCompiledEntry lookup(void*, ExecutionContext&, GuestAddress target,
                           CompiledLookupKind) {
  switch (target) {
    case kNested: return &setjmp_nested_v2;
    case kSetJmp: return &xenon::recomp::runtime_helpers::setjmp_v2;
    case kLongJmp: return &xenon::recomp::runtime_helpers::longjmp_v2;
    default: return nullptr;
  }
}

void run_round(xenon::kernel::KernelProcess& process,
               xenon::memory::AddressSpace& address_space,
               TestRuntime& runtime, GuestAddress jump_buffer,
               std::uint64_t stack_pointer, std::uint64_t r14_value,
               std::uint32_t initial_cr) {
  CpuState state{};
  constexpr std::uint64_t kCallerLr = 0xCAFEBABCu;
  state.lr = kCallerLr;
  state.gpr[1] = stack_pointer;
  state.gpr[3] = jump_buffer;
  state.gpr[13] = 0x1313131313131313ull;
  state.gpr[14] = r14_value;
  state.gpr[30] = 0x3030303030303030ull;
  state.cr = initial_cr;
  for (std::uint32_t reg = 14; reg <= 31; ++reg)
    state.fpr_bits[reg] = 0xF000000000000000ull | reg;
  for (std::uint32_t reg = 64; reg <= 127; ++reg)
    for (std::size_t byte = 0; byte < state.vr[reg].bytes.size(); ++byte)
      state.vr[reg].bytes[byte] = static_cast<std::uint8_t>(reg + byte);

  const auto saved_f14 = state.fpr_bits[14];
  const auto saved_v64 = state.vr[64];

  ExecutionContext context(state, address_space, runtime);
  context.compiled_lookup = &lookup;
  const auto result = setjmp_parent_v2(context);

  assert(result.reason == FlowReason::Return);
  assert(result.next_address == kCallerLr);
  assert(state.gpr[3] == 7u);  // setjmp appears to return the longjmp value.
  assert(state.gpr[1] == stack_pointer);  // nested stack adjustment was undone.
  assert(state.gpr[13] == 0x1313131313131313ull);
  assert(state.gpr[14] == r14_value);  // nested r14 mutation was undone.
  assert(state.fpr_bits[14] == saved_f14);
  assert(state.vr[64].bytes == saved_v64.bytes);

  using namespace xenon::recomp::runtime_helpers;
  assert(address_space.read64_be(jump_buffer + kJumpBufferR1Offset) == stack_pointer);
  assert(address_space.read64_be(jump_buffer + kJumpBufferR13Offset) ==
         0x1313131313131313ull);
  assert(address_space.read32_be(jump_buffer + kJumpBufferCrOffset) == initial_cr);
  assert(address_space.read32_be(jump_buffer + kJumpBufferLrOffset) == 0x100Cu);
  assert(address_space.read64_be(jump_buffer + kJumpBufferFpr14Offset) == saved_f14);
  assert(address_space.read8(jump_buffer + kJumpBufferCrOffset) ==
         static_cast<std::uint8_t>(initial_cr >> 24));
}

}  // namespace

int main() {
  auto address_space = std::make_shared<xenon::memory::AddressSpace>(
      xenon::memory::GuestTranslationMode::Compact);
  assert(address_space->initialize());
  auto memory = std::make_shared<xenon::kernel::KernelMemory>(address_space);
  xenon::kernel::KernelProcess process(memory);
  TestRuntime runtime(process);

  std::uint32_t stack = 0;
  std::uint32_t jump_a = 0;
  std::uint32_t jump_b = 0;
  assert(memory->allocate_virtual(stack, 0x10000u, xenon::memory::kReadWrite));
  assert(memory->allocate_virtual(jump_a, 0x2000u, xenon::memory::kReadWrite));
  assert(memory->allocate_virtual(jump_b, 0x2000u, xenon::memory::kReadWrite));
  assert((jump_a & 0xFu) == 0u && (jump_b & 0xFu) == 0u && jump_a != jump_b);

  xenon::kernel::ThreadCreationParams params{};
  params.name = "setjmp-longjmp-test";
  const auto thread = process.thread_manager().create_thread([] { return 0u; }, params);
  assert(thread);
  process.thread_manager().set_current_thread(thread);
  assert(thread->set_tls(5u, 0x1122334455667788ull));
  int kpcr_sentinel = 42;
  thread->set_kernel_data(&kpcr_sentinel);

  run_round(process, *address_space, runtime, jump_a, stack + 0x8000u,
            0x1414141414141414ull, 0xA5A5A5A5u);
  assert(process.thread_manager().current_thread() == thread);
  assert(thread->get_tls(5u).value() == 0x1122334455667788ull);
  assert(thread->kernel_data() == &kpcr_sentinel);

  // Independent buffers retain independent guest contexts.
  run_round(process, *address_space, runtime, jump_b, stack + 0x7000u,
            0xDEADBEEFCAFED00Dull, 0x55AA33CCu);
  assert(address_space->read64_be(
             jump_a + xenon::recomp::runtime_helpers::kJumpBufferR1Offset) ==
         stack + 0x8000u);
  assert(address_space->read64_be(
             jump_b + xenon::recomp::runtime_helpers::kJumpBufferR1Offset) ==
         stack + 0x7000u);

  // Zero longjmp values normalize to one, without host-stack unwinding.
  CpuState direct_state{};
  direct_state.gpr[3] = jump_a;
  direct_state.gpr[4] = 0u;
  ExecutionContext direct_context(direct_state, *address_space, runtime);
  auto longjmp_result = xenon::recomp::runtime_helpers::longjmp_v2(direct_context);
  assert(longjmp_result.reason == FlowReason::LongJump);
  assert(direct_state.gpr[3] == 1u);

  // Invalid/unaligned and unmapped guest buffers fail as guest traps.
  CpuState invalid_state{};
  invalid_state.gpr[3] = jump_a + 1u;
  ExecutionContext invalid_context(invalid_state, *address_space, runtime);
  assert(xenon::recomp::runtime_helpers::setjmp_v2(invalid_context).reason ==
         FlowReason::Trap);
  invalid_state.gpr[3] = 0x33330000u;
  assert(xenon::recomp::runtime_helpers::setjmp_v2(invalid_context).reason ==
         FlowReason::Trap);

  process.thread_manager().set_current_thread(nullptr);
  process.terminate(0u);
  std::cout << "xenon_cpu_setjmp_longjmp: ok\n";
  return 0;
}
