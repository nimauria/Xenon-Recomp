// XOVERLAPPED completion for XAM calls (src/xam/xam_overlapped.hpp).
//
// With an XOVERLAPPED a call returns ERROR_IO_PENDING and reports its result
// through the overlapped: Result, ExtendedError and Length are written, its
// event is signalled, and its completion routine is queued to the calling
// thread as CompletionRoutine(Result, Length, Overlapped). Without one the
// call is unchanged. XamGetOverlappedResult reads a completed overlapped
// back. Behaviour follows xenia and rexglue-sdk (CompleteOverlappedImmediate
// and XamGetOverlappedResult_entry). Exports are driven through
// core::ExportRegistry::invoke().

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/content_manager.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {
bool register_content_exports(core::ExportRegistry& registry, ContentManager& content_manager,
                              core::XenonSession& session);
bool register_enum_exports(core::ExportRegistry& registry, core::XenonSession& session);
bool register_msg_exports(core::ExportRegistry& registry, core::XenonSession& session);
}  // namespace xenon::xam

namespace xenon::core {
struct SessionExecutionTestAccess {
  static void configure(XenonSession& session,
                        std::shared_ptr<xenon::memory::AddressSpace> memory) {
    session.config_.enable_logging = false;
    session.memory_ = memory;
    session.kernel_memory_ = std::make_shared<kernel::KernelMemory>(memory);
    session.kernel_process_ = std::make_shared<kernel::KernelProcess>(session.kernel_memory_);
  }
};
}  // namespace xenon::core

using namespace xenon;

namespace {

constexpr std::uint32_t kXamEnumerate = 0x0250u;
constexpr std::uint32_t kXamContentCreateEnumerator = 0x025Cu;
constexpr std::uint32_t kXamContentGetDeviceState = 0x0265u;
constexpr std::uint32_t kXamContentCreateEx = 0x0259u;
constexpr std::uint32_t kXamGetOverlappedResult = 0x01FBu;
constexpr std::uint32_t kErrorIoPending = 0x3E5u;
constexpr std::uint32_t kErrorIoIncomplete = 0x3E4u;
constexpr std::uint32_t kErrorFunctionFailed = 0x65Bu;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::XenonSession session;
  xam::ContentManager content_manager;
  core::ExportRegistry registry;
  memory::GuestAddress stack_top{};

  Fixture() {
    address_space =
        std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    core::SessionExecutionTestAccess::configure(session, address_space);
    content_manager.initialize();
    assert(xam::register_content_exports(registry, content_manager, session));
    assert(xam::register_enum_exports(registry, session));
    assert(xam::register_msg_exports(registry, session));
    memory::GuestAddress stack{};
    assert(address_space->allocate(0x10000, 0x1000, memory::kReadWrite, false, stack));
    stack_top = stack + 0x10000u - 0x200u;
  }

  std::uint32_t call(std::uint32_t ordinal, cpu::CpuState& cpu) {
    if (cpu.gpr[1] == 0u) cpu.gpr[1] = stack_top;
    core::ExportCallContext context{cpu, *address_space, 0, 0};
    assert(registry.invoke("xam", ordinal, context).handled);
    return static_cast<std::uint32_t>(cpu.gpr[3]);
  }

  memory::GuestAddress alloc(std::uint32_t bytes) {
    memory::GuestAddress address{};
    assert(address_space->allocate(bytes, 8, memory::kReadWrite, false, address));
    return address;
  }

  // A zeroed XOVERLAPPED, optionally with an event and a completion routine.
  memory::GuestAddress overlapped(std::uint32_t event = 0, std::uint32_t routine = 0) {
    const auto o = alloc(28);
    for (std::uint32_t i = 0; i < 28; i += 4) address_space->write32_be(o + i, 0u);
    address_space->write32_be(o + 0x0C, event);
    address_space->write32_be(o + 0x10, routine);
    return o;
  }

  std::pair<kernel::Handle, std::shared_ptr<kernel::KernelEvent>> event() {
    auto ev = std::make_shared<kernel::KernelEvent>(true, false);
    kernel::Handle handle{};
    assert(session.kernel_process()->handle_table().insert(ev, 0xFFFFFFFFu,
                                                           kernel::HandleFlags::None, handle) ==
           kernel::KernelIoCode::Success);
    return {handle, ev};
  }

  std::uint32_t result_of(memory::GuestAddress o) { return address_space->read32_be(o); }
  std::uint32_t length_of(memory::GuestAddress o) { return address_space->read32_be(o + 4); }
  std::uint32_t extended_of(memory::GuestAddress o) { return address_space->read32_be(o + 0x18); }
};

void test_without_an_overlapped_the_call_is_synchronous() {
  Fixture f;
  cpu::CpuState cpu{};
  cpu.gpr[3] = xam::storage_device::HardDisk;
  assert(f.call(kXamContentGetDeviceState, cpu) == xam::result::Success);
}

void test_an_overlapped_call_completes_through_the_overlapped() {
  Fixture f;
  auto [handle, ev] = f.event();
  const auto o = f.overlapped(handle);
  cpu::CpuState cpu{};
  cpu.gpr[3] = xam::storage_device::HardDisk;
  cpu.gpr[4] = o;
  assert(f.call(kXamContentGetDeviceState, cpu) == kErrorIoPending);
  assert(f.result_of(o) == 0u && f.length_of(o) == 0u && f.extended_of(o) == 0u);
  assert(ev->signaled() && "the overlapped's event is signalled");

  // A missing device: the overlapped carries ERROR_FUNCTION_FAILED and the
  // device error as the extended error, as rexglue-sdk reports it.
  const auto failed = f.overlapped();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = 0x7777u;
  cpu.gpr[4] = failed;
  assert(f.call(kXamContentGetDeviceState, cpu) == kErrorIoPending);
  assert(f.result_of(failed) == kErrorFunctionFailed && f.extended_of(failed) != 0u);
}

void test_completion_routine_is_queued_to_the_calling_thread() {
  Fixture f;
  constexpr std::uint32_t kRoutine = 0x82005000u;
  const auto o = f.overlapped(0, kRoutine);
  std::vector<kernel::KernelThread::UserApc> queued;
  kernel::ThreadCreationParams params{};
  auto thread = std::make_shared<kernel::KernelThread>(
      [&]() -> std::uint32_t {
        cpu::CpuState cpu{};
        cpu.gpr[3] = xam::storage_device::HardDisk;
        cpu.gpr[4] = o;
        assert(f.call(kXamContentGetDeviceState, cpu) == kErrorIoPending);
        queued = kernel::KernelThread::current()->take_user_apcs();
        return 0;
      },
      params);
  assert(thread->start() && thread->join(5000));
  assert(queued.size() == 1);
  assert(queued[0].routine == kRoutine && queued[0].context == 0u && queued[0].arg1 == 0u &&
         queued[0].arg2 == o && "CompletionRoutine(Result, Length, Overlapped)");
}

void test_get_overlapped_result_reads_back_a_completed_call() {
  Fixture f;
  const auto o = f.overlapped();
  cpu::CpuState cpu{};
  cpu.gpr[3] = xam::storage_device::HardDisk;
  cpu.gpr[4] = o;
  assert(f.call(kXamContentGetDeviceState, cpu) == kErrorIoPending);
  const auto length = f.alloc(4);
  f.address_space->write32_be(length, 0xAAAAAAAAu);
  cpu = cpu::CpuState{};
  cpu.gpr[3] = o;
  cpu.gpr[4] = length;
  cpu.gpr[5] = 1;
  assert(f.call(kXamGetOverlappedResult, cpu) == 0u);
  assert(f.address_space->read32_be(length) == 0u);

  // An overlapped the guest still marks pending, with no event to wait on.
  const auto pending = f.overlapped();
  f.address_space->write32_be(pending, kErrorIoPending);
  cpu = cpu::CpuState{};
  cpu.gpr[3] = pending;
  cpu.gpr[5] = 0;
  assert(f.call(kXamGetOverlappedResult, cpu) == kErrorIoIncomplete);
}

void test_enumerate_reports_the_item_count_through_the_overlapped() {
  Fixture f;
  std::uint32_t content_id = 0;
  assert(f.content_manager.create_content(xam::storage_device::HardDisk, "save1",
                                           xam::ContentType::SavedGame, 0, content_id) ==
         xam::result::Success);
  const auto buffer_size = f.alloc(4);
  const auto handle_out = f.alloc(4);
  cpu::CpuState cpu{};
  cpu.gpr[4] = xam::storage_device::HardDisk;
  cpu.gpr[5] = static_cast<std::uint32_t>(xam::ContentType::SavedGame);
  cpu.gpr[7] = 10;
  cpu.gpr[8] = buffer_size;
  cpu.gpr[9] = handle_out;
  assert(f.call(kXamContentCreateEnumerator, cpu) == xam::result::Success);
  const auto handle = f.address_space->read32_be(handle_out);
  const auto buffer = f.alloc(306u * 10u);
  const auto o = f.overlapped();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[5] = buffer;
  cpu.gpr[6] = 306u * 10u;
  cpu.gpr[7] = 0;  // items_returned: none with an overlapped
  cpu.gpr[8] = o;
  assert(f.call(kXamEnumerate, cpu) == kErrorIoPending);
  assert(f.result_of(o) == 0u && f.length_of(o) == 1u && "Length is the item count");

  const auto exhausted = f.overlapped();
  cpu = cpu::CpuState{};
  cpu.gpr[3] = handle;
  cpu.gpr[5] = buffer;
  cpu.gpr[6] = 306u;
  cpu.gpr[8] = exhausted;
  assert(f.call(kXamEnumerate, cpu) == kErrorIoPending);
  assert(f.result_of(exhausted) == 0x12u && f.extended_of(exhausted) == 0x80070012u &&
         "ERROR_NO_MORE_FILES, extended HRESULT_FROM_WIN32");
}

// XamContentCreateEx takes its overlapped as its ninth argument, from the
// caller's stack parameter area ([r1 + 0x54]).
void test_a_stack_passed_overlapped_is_honoured() {
  Fixture f;
  const auto o = f.overlapped();
  cpu::CpuState sync{};
  sync.gpr[1] = f.stack_top;
  const auto sync_result = f.call(kXamContentCreateEx, sync);  // no content data: fails
  cpu::CpuState cpu{};
  cpu.gpr[1] = f.stack_top;
  f.address_space->write32_be(f.stack_top + 0x54u, o);
  assert(f.call(kXamContentCreateEx, cpu) == kErrorIoPending);
  assert(f.result_of(o) == sync_result && "the overlapped carries the synchronous result");
  f.address_space->write32_be(f.stack_top + 0x54u, 0u);
}

}  // namespace

int main() {
  std::cout << "Testing XAM XOVERLAPPED completion...\n";
  test_without_an_overlapped_the_call_is_synchronous();
  test_an_overlapped_call_completes_through_the_overlapped();
  test_completion_routine_is_queued_to_the_calling_thread();
  test_get_overlapped_result_reads_back_a_completed_call();
  test_enumerate_reports_the_item_count_through_the_overlapped();
  test_a_stack_passed_overlapped_is_honoured();
  std::cout << "All XAM XOVERLAPPED tests passed!\n";
  return 0;
}
