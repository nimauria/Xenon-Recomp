// User APCs through the guest-facing exports: NtQueueApcThread and
// NtSetTimerEx queue them; alertable KeWaitForSingleObject,
// NtWaitForSingleObjectEx and KeDelayExecutionThread run them and return
// STATUS_USER_APC, as xenia and rexglue-sdk do. Exports are driven through
// core::ExportRegistry::invoke() on a real KernelThread; the session's nested
// guest call is replaced by one that records the routine and its arguments.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_threading_exports.hpp"
#include "xenon/xbox/xboxkrnl_time_exports.hpp"

using namespace xenon;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kNtQueueApcThread = 0x0E3u;
constexpr std::uint32_t kKeInitializeEvent = 0x070u;
constexpr std::uint32_t kKeWaitForSingleObject = 0x0B0u;
constexpr std::uint32_t kKeDelayExecutionThread = 0x05Au;
constexpr std::uint32_t kNtCreateTimer = 0x0D7u;
constexpr std::uint32_t kNtSetTimerEx = 0x0FAu;
constexpr std::uint32_t kNtWaitForSingleObjectEx = 0x0FDu;
constexpr std::uint32_t kKeInitializeApc = 0x06Du;
constexpr std::uint32_t kKeInsertQueueApc = 0x07Au;
constexpr std::uint32_t kKeRemoveQueueApc = 0x2F0u;
constexpr std::uint32_t kKeAlertThread = 0x04Fu;
constexpr std::uint32_t kKeTestAlertThread = 0x0ACu;
constexpr std::uint32_t kStatusUserApc = 0x000000C0u;
constexpr std::uint32_t kStatusAlerted = 0x00000101u;
constexpr std::uint32_t kStatusTimeout = 0x00000102u;

struct Call {
  std::uint32_t routine;
  std::vector<std::uint64_t> arguments;
};

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;
  std::mutex calls_mutex;
  std::vector<Call> calls;
  // Runs in place of a guest routine; may write through pointer arguments as
  // a kernel routine would.
  std::function<void(std::uint32_t routine, std::span<const std::uint64_t>)> on_call;
  core::GuestCall guest_call = [this](cpu::CpuState&, cpu::GuestAddress routine,
                                      std::span<const std::uint64_t> arguments) {
    {
      std::scoped_lock lock(calls_mutex);
      calls.push_back({static_cast<std::uint32_t>(routine), {arguments.begin(), arguments.end()}});
    }
    if (on_call) on_call(static_cast<std::uint32_t>(routine), arguments);
    return true;
  };
  memory::GuestAddress stack_top{};

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);
    assert(xbox::register_xboxkrnl_sync_exports(registry, *process));
    assert(xbox::register_xboxkrnl_ke_sync_exports(registry, *process));
    assert(xbox::register_xboxkrnl_time_exports(registry));
    core::ExportDescriptor queue{};
    queue.library = "xboxkrnl.exe";
    queue.name = "NtQueueApcThread";
    queue.ordinal = kNtQueueApcThread;
    auto* raw = process.get();
    queue.handler = [raw](core::ExportCallContext& ctx) {
      return xbox::nt_queue_apc_thread_export(*raw, ctx);
    };
    assert(registry.register_export(std::move(queue)));
    using Handler = bool (*)(kernel::KernelProcess&, core::ExportCallContext&);
    const std::pair<std::uint32_t, Handler> kapc_exports[] = {
        {kKeInitializeApc, &xbox::ke_initialize_apc_export},
        {kKeInsertQueueApc, &xbox::ke_insert_queue_apc_export},
        {kKeRemoveQueueApc, &xbox::ke_remove_queue_apc_export},
        {kKeAlertThread, &xbox::ke_alert_thread_export},
        {kKeTestAlertThread, &xbox::ke_test_alert_thread_export},
    };
    for (const auto& [ordinal, handler] : kapc_exports) {
      core::ExportDescriptor descriptor{};
      descriptor.library = "xboxkrnl.exe";
      descriptor.name = "kapc";
      descriptor.ordinal = ordinal;
      descriptor.handler = [raw, handler = handler](core::ExportCallContext& ctx) {
        return handler(*raw, ctx);
      };
      assert(registry.register_export(std::move(descriptor)));
    }
    memory::GuestAddress stack{};
    assert(address_space->allocate(0x10000, 0x1000, memory::kReadWrite, false, stack));
    stack_top = stack + 0x10000u - 0x100u;
  }

  std::uint32_t call(std::uint32_t ordinal, std::uint32_t thread_id,
                     std::initializer_list<std::uint64_t> registers) {
    cpu::CpuState cpu{};
    cpu.gpr[1] = stack_top;
    std::size_t r = 3;
    for (const auto value : registers) cpu.gpr[r++] = value;
    core::ExportCallContext context{cpu, *address_space, 0, thread_id};
    context.guest_call = &guest_call;
    assert(registry.invoke("xboxkrnl", ordinal, context).success);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    memory::GuestAddress address{};
    assert(address_space->allocate(bytes, 8, memory::kReadWrite, false, address));
    return address;
  }

  // A guest thread that runs `body` once, with its handle in the table.
  struct GuestThread {
    std::shared_ptr<kernel::KernelThread> thread;
    kernel::Handle handle{};
  };
  GuestThread start(std::function<void(std::uint32_t thread_id)> body,
                    std::atomic<bool>& ready) {
    kernel::ThreadCreationParams params{};
    GuestThread guest{};
    guest.thread = process->thread_manager().create_thread(
        [this, body, &ready]() -> std::uint32_t {
          while (!ready.load()) std::this_thread::sleep_for(1ms);
          body(kernel::KernelThread::current()->thread_id());
          return 0;
        },
        params);
    assert(guest.thread);
    assert(process->handle_table().insert(guest.thread, 0xFFFFFFFFu, kernel::HandleFlags::None,
                                          guest.handle) == kernel::KernelIoCode::Success);
    assert(guest.thread->start());
    return guest;
  }
};

void test_queued_apc_runs_at_an_alertable_wait() {
  Fixture f;
  const auto event = f.alloc(0x10);
  f.call(kKeInitializeEvent, 0, {event, 0 /*notification*/, 0});
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> non_alertable{0}, alertable{0};
  std::atomic<std::size_t> calls_after_non_alertable{99};
  const auto timeout = f.alloc(8);
  f.address_space->write64_be(timeout, static_cast<std::uint64_t>(-10'000LL * 20));  // 20 ms
  auto guest = f.start(
      [&](std::uint32_t id) {
        // r3 header, r4 reason, r5 mode, r6 alertable, r7 timeout
        non_alertable = f.call(kKeWaitForSingleObject, id, {event, 0, 1, 0, timeout});
        {
          std::scoped_lock lock(f.calls_mutex);
          calls_after_non_alertable = f.calls.size();
        }
        alertable = f.call(kKeWaitForSingleObject, id, {event, 0, 1, 1, 0 /*infinite*/});
      },
      ready);
  assert(f.call(kNtQueueApcThread, 0, {guest.handle, 0x82001000u, 11, 22, 33}) == 0u);
  ready = true;
  assert(guest.thread->join(5000));
  assert(non_alertable.load() == kStatusTimeout && calls_after_non_alertable.load() == 0 &&
         "a non-alertable wait neither ends for nor runs the APC");
  assert(alertable.load() == kStatusUserApc);
  assert(f.calls.size() == 1 && f.calls[0].routine == 0x82001000u);
  assert((f.calls[0].arguments == std::vector<std::uint64_t>{11, 22, 33}));
}

void test_queued_apc_ends_an_alertable_delay() {
  Fixture f;
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> result{0};
  const auto interval = f.alloc(8);
  f.address_space->write64_be(interval, static_cast<std::uint64_t>(-10'000LL * 600'000));  // 10 min
  auto guest = f.start(
      [&](std::uint32_t id) {
        // r3 mode, r4 alertable, r5 interval
        result = f.call(kKeDelayExecutionThread, id, {1, 1, interval});
      },
      ready);
  ready = true;
  std::this_thread::sleep_for(30ms);
  assert(f.call(kNtQueueApcThread, 0, {guest.handle, 0x82002000u, 1, 2, 3}) == 0u);
  assert(guest.thread->join(5000) && "the APC ended the ten-minute delay");
  assert(result.load() == kStatusUserApc);
  assert(f.calls.size() == 1 && f.calls[0].routine == 0x82002000u);
}

void test_timer_routine_is_delivered_to_the_setting_thread() {
  Fixture f;
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> result{0};
  auto guest = f.start(
      [&](std::uint32_t id) {
        const auto handle_out = f.alloc(4);
        assert(f.call(kNtCreateTimer, id, {handle_out, 0, 1 /*synchronization*/}) == 0u);
        const auto timer = f.address_space->read32_be(handle_out);
        const auto due = f.alloc(8);
        f.address_space->write64_be(due, static_cast<std::uint64_t>(-10'000LL * 30));  // 30 ms
        // r3 handle, r4 due, r5 routine, r6 unk, r7 argument, r8 resume, r9 period
        assert(f.call(kNtSetTimerEx, id, {timer, due, 0x82003000u, 0, 0x55u, 0, 0}) == 0u);
        const auto never = f.alloc(0x10);
        f.call(kKeInitializeEvent, id, {never, 0, 0});
        result = f.call(kKeWaitForSingleObject, id, {never, 0, 1, 1, 0});
      },
      ready);
  ready = true;
  assert(guest.thread->join(5000));
  assert(result.load() == kStatusUserApc);
  assert(f.calls.size() == 1 && f.calls[0].routine == 0x82003000u);
  assert(f.calls[0].arguments[0] == 0x55u && "Routine(Argument, TimeLow, TimeHigh)");
  assert(f.calls[0].arguments[1] != 0u || f.calls[0].arguments[2] != 0u);
}

// A guest KAPC: KeInitializeApc fills it, KeInsertQueueApc queues it, and the
// alertable wait runs KernelRoutine(Apc, &NormalRoutine, &NormalContext,
// &Argument1, &Argument2) and then NormalRoutine with whatever the kernel
// routine left there.
void test_guest_kapc_runs_kernel_then_normal_routine() {
  Fixture f;
  constexpr std::uint32_t kKthread = 0x00446600u;
  constexpr std::uint32_t kKernelRoutine = 0x82004000u;
  constexpr std::uint32_t kNormalRoutine = 0x82004100u;
  const auto apc = f.alloc(40);
  f.on_call = [&](std::uint32_t routine, std::span<const std::uint64_t> arguments) {
    if (routine != kKernelRoutine) return;
    assert(arguments.size() == 5 && arguments[0] == apc);
    // The kernel routine rewrites the normal context through its pointer.
    f.address_space->write32_be(static_cast<memory::GuestAddress>(arguments[2]), 0x99u);
  };
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> result{0};
  auto guest = f.start(
      [&](std::uint32_t id) {
        const auto never = f.alloc(0x10);
        f.call(kKeInitializeEvent, id, {never, 0, 0});
        result = f.call(kKeWaitForSingleObject, id, {never, 0, 1, 1, 0});
      },
      ready);
  guest.thread->set_guest_kthread_address(kKthread);
  // r3 apc, r4 thread, r5 kernel, r6 rundown, r7 normal, r8 mode, r9 context
  f.call(kKeInitializeApc, 0, {apc, kKthread, kKernelRoutine, 0, kNormalRoutine, 1, 0x10u});
  assert(f.address_space->read16_be(apc) == 18u && "ApcObject");
  assert(f.call(kKeInsertQueueApc, 0, {apc, 0x20u, 0x30u, 0}) == 1u);
  assert(f.call(kKeInsertQueueApc, 0, {apc, 0x20u, 0x30u, 0}) == 0u && "already queued");
  ready = true;
  assert(guest.thread->join(5000));
  assert(result.load() == kStatusUserApc);
  assert(f.calls.size() == 2);
  assert(f.calls[0].routine == kKernelRoutine);
  assert(f.calls[1].routine == kNormalRoutine);
  assert((f.calls[1].arguments == std::vector<std::uint64_t>{0x99u, 0x20u, 0x30u}));
  assert(f.address_space->read8(apc + 3u) == 0u && "no longer enqueued");
}

void test_removed_kapc_is_not_delivered() {
  Fixture f;
  constexpr std::uint32_t kKthread = 0x00446700u;
  const auto apc = f.alloc(40);
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> result{0};
  auto guest = f.start(
      [&](std::uint32_t id) {
        const auto never = f.alloc(0x10);
        f.call(kKeInitializeEvent, id, {never, 0, 0});
        const auto timeout = f.alloc(8);
        f.address_space->write64_be(timeout, static_cast<std::uint64_t>(-10'000LL * 30));
        result = f.call(kKeWaitForSingleObject, id, {never, 0, 1, 1, timeout});
      },
      ready);
  guest.thread->set_guest_kthread_address(kKthread);
  f.call(kKeInitializeApc, 0, {apc, kKthread, 0, 0, 0x82004100u, 1, 0});
  assert(f.call(kKeInsertQueueApc, 0, {apc, 0, 0, 0}) == 1u);
  assert(f.call(kKeRemoveQueueApc, 0, {apc}) == 1u);
  assert(f.call(kKeRemoveQueueApc, 0, {apc}) == 0u && "nothing left to remove");
  ready = true;
  assert(guest.thread->join(5000));
  assert(result.load() == kStatusTimeout && f.calls.empty());
}

// Kernel-mode APC delivery is not modelled (as in xenia and rexglue-sdk): the
// KAPC is accepted and marked queued, but no wait runs it.
void test_kernel_mode_kapc_is_queued_but_not_delivered() {
  Fixture f;
  constexpr std::uint32_t kKthread = 0x00446800u;
  const auto apc = f.alloc(40);
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> result{0};
  auto guest = f.start(
      [&](std::uint32_t id) {
        const auto never = f.alloc(0x10);
        f.call(kKeInitializeEvent, id, {never, 0, 0});
        const auto timeout = f.alloc(8);
        f.address_space->write64_be(timeout, static_cast<std::uint64_t>(-10'000LL * 30));
        result = f.call(kKeWaitForSingleObject, id, {never, 0, 1, 1, timeout});
      },
      ready);
  guest.thread->set_guest_kthread_address(kKthread);
  f.call(kKeInitializeApc, 0, {apc, kKthread, 0x82004000u, 0, 0x82004100u, 0 /*kernel*/, 0});
  assert(f.call(kKeInsertQueueApc, 0, {apc, 0, 0, 0}) == 1u);
  ready = true;
  assert(guest.thread->join(5000));
  assert(result.load() == kStatusTimeout && f.calls.empty());
}

void test_ke_alert_thread_ends_an_alertable_wait() {
  Fixture f;
  constexpr std::uint32_t kKthread = 0x00446900u;
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> result{0}, alerted_after{1};
  auto guest = f.start(
      [&](std::uint32_t id) {
        const auto never = f.alloc(0x10);
        f.call(kKeInitializeEvent, id, {never, 0, 0});
        result = f.call(kKeWaitForSingleObject, id, {never, 0, 1, 1, 0});
        alerted_after = f.call(kKeTestAlertThread, id, {1});
      },
      ready);
  guest.thread->set_guest_kthread_address(kKthread);
  ready = true;
  std::this_thread::sleep_for(30ms);
  assert(f.call(kKeAlertThread, 0, {kKthread, 1}) == 0u && "not previously alerted");
  assert(guest.thread->join(5000));
  assert(result.load() == kStatusAlerted && f.calls.empty());
  assert(alerted_after.load() == 0u && "the wait cleared the alert");
}

void test_queue_apc_rejects_bad_targets() {
  Fixture f;
  assert(f.call(kNtQueueApcThread, 0, {0x7777u, 0x82001000u, 0, 0, 0}) == 0xC0000008u);
  std::atomic<bool> ready{true};
  auto guest = f.start([](std::uint32_t) {}, ready);
  assert(guest.thread->join(5000));
  assert(f.call(kNtQueueApcThread, 0, {guest.handle, 0u, 0, 0, 0}) == 0xC000000Du &&
         "a null routine");
  assert(f.call(kNtQueueApcThread, 0, {guest.handle, 0x82001000u, 0, 0, 0}) == 0xC000004Bu &&
         "a finished thread");
}

}  // namespace

int main() {
  std::cout << "Testing user APC exports...\n";
  test_queued_apc_runs_at_an_alertable_wait();
  test_queued_apc_ends_an_alertable_delay();
  test_timer_routine_is_delivered_to_the_setting_thread();
  test_guest_kapc_runs_kernel_then_normal_routine();
  test_removed_kapc_is_not_delivered();
  test_kernel_mode_kapc_is_queued_but_not_delivered();
  test_ke_alert_thread_ends_an_alertable_wait();
  test_queue_apc_rejects_bad_targets();
  std::cout << "All user APC export tests passed!\n";
  return 0;
}
