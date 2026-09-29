// NtDuplicateObject (Ob* family export file). Drives it through
// core::ExportRegistry::invoke() exactly as a guest thunk would, exercising
// the real handle-table duplication/insertion paths - not by calling the
// handler C++ function directly.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/event.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xboxkrnl_ob_exports.hpp"
#include "xenon/xbox/xboxkrnl_sync_exports.hpp"

using namespace xenon;

namespace {

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  std::shared_ptr<kernel::KernelMemory> kernel_memory;
  std::unique_ptr<kernel::KernelProcess> process;
  core::ExportRegistry registry;

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    assert(address_space->initialize());
    kernel_memory = std::make_shared<kernel::KernelMemory>(address_space);
    process = std::make_unique<kernel::KernelProcess>(kernel_memory);

    core::ExportDescriptor ob_ref{};
    ob_ref.library = "xboxkrnl.exe";
    ob_ref.name = "ObReferenceObjectByHandle";
    ob_ref.ordinal = 0x110u;
    auto* raw_process = process.get();
    ob_ref.handler = [raw_process](core::ExportCallContext& ctx) {
      return xbox::ob_reference_object_by_handle_export(*raw_process, ctx);
    };
    assert(registry.register_export(std::move(ob_ref)));

    core::ExportDescriptor dup{};
    dup.library = "xboxkrnl.exe";
    dup.name = "NtDuplicateObject";
    dup.ordinal = 0x0DAu;
    dup.handler = [raw_process](core::ExportCallContext& ctx) {
      return xbox::nt_duplicate_object_export(*raw_process, ctx);
    };
    assert(registry.register_export(std::move(dup)));

    assert(xbox::register_xboxkrnl_sync_exports(registry, *process));
  }

  [[nodiscard]] core::ExportCallResult invoke(std::uint32_t ordinal, cpu::CpuState& cpu) {
    core::ExportCallContext call{cpu, *address_space, 0, 0};
    return registry.invoke("xboxkrnl", ordinal, call);
  }

  [[nodiscard]] memory::GuestAddress alloc32() {
    memory::GuestAddress addr{};
    assert(address_space->allocate(4, 4, memory::kReadWrite, false, addr));
    return addr;
  }
};

void test_nt_duplicate_object_real_handle_round_trips() {
  Fixture f;
  const auto handle_out = f.alloc32();
  cpu::CpuState create{};
  create.gpr[3] = handle_out;
  create.gpr[4] = 0;
  create.gpr[5] = 1;  // auto-reset
  create.gpr[6] = 0;  // initially not signaled
  assert(f.invoke(0x0D1u, create).success);
  const auto handle = f.address_space->read32_be(handle_out);
  assert(handle != 0u);

  const auto new_handle_out = f.alloc32();
  f.address_space->write32_be(new_handle_out, 0xDEADBEEFu);
  cpu::CpuState dup{};
  dup.gpr[3] = handle;
  dup.gpr[4] = new_handle_out;
  dup.gpr[5] = 0;  // no options
  auto dup_result = f.invoke(0x0DAu, dup);
  assert(dup_result.handled && dup_result.success);
  assert(dup.gpr[3] == 0u);  // STATUS_SUCCESS
  const auto new_handle = f.address_space->read32_be(new_handle_out);
  assert(new_handle != 0u);
  assert(new_handle != handle);  // a genuinely distinct handle-table entry

  // The original handle must still be independently valid - duplication
  // without DUPLICATE_CLOSE_SOURCE does not consume the source.
  cpu::CpuState wait_original{};
  wait_original.gpr[3] = handle;
  wait_original.gpr[6] = 0;  // NULL timeout would block forever if unsignaled,
  // so signal both first via the new handle to prove they refer to the same
  // underlying object without ever calling a real blocking wait in a test.
  kernel::HandleView view{};
  assert(f.process->handle_table().lookup(new_handle, view) == kernel::KernelIoCode::Success);
  static_cast<kernel::KernelEvent&>(*view.object).set();
  kernel::HandleView original_view{};
  assert(f.process->handle_table().lookup(handle, original_view) == kernel::KernelIoCode::Success);
  assert(original_view.object.get() == view.object.get());
  assert(static_cast<kernel::KernelEvent&>(*original_view.object).signaled());
}

void test_nt_duplicate_object_close_source_option_closes_original() {
  Fixture f;
  const auto handle_out = f.alloc32();
  cpu::CpuState create{};
  create.gpr[3] = handle_out;
  create.gpr[4] = 0;
  create.gpr[5] = 1;
  create.gpr[6] = 0;
  assert(f.invoke(0x0D1u, create).success);
  const auto handle = f.address_space->read32_be(handle_out);

  cpu::CpuState dup{};
  dup.gpr[3] = handle;
  dup.gpr[4] = 0;  // caller does not want the new handle value back
  dup.gpr[5] = 1;  // DUPLICATE_CLOSE_SOURCE
  assert(f.invoke(0x0DAu, dup).success);
  assert(dup.gpr[3] == 0u);

  kernel::HandleView view{};
  assert(f.process->handle_table().lookup(handle, view) == kernel::KernelIoCode::InvalidHandle);
}

void test_nt_duplicate_object_pseudo_handle_materializes_real_handle() {
  // NtDuplicateObject's most common real use: turning the current-thread
  // pseudo-handle into a real, closeable handle-table entry - see
  // xboxkrnl_ob_exports.cpp's kCurrentThreadPseudoHandle comment.
  Fixture f;
  kernel::ThreadCreationParams params{};
  params.create_suspended = true;  // never actually runs the trivial entry
  auto thread = f.process->thread_manager().create_thread([] { return 0u; }, params);
  f.process->thread_manager().set_current_thread(thread);

  const auto new_handle_out = f.alloc32();
  cpu::CpuState dup{};
  dup.gpr[3] = 0xFFFFFFFEu;  // NtCurrentThread() pseudo-handle
  dup.gpr[4] = new_handle_out;
  dup.gpr[5] = 0;
  auto result = f.invoke(0x0DAu, dup);
  assert(result.handled && result.success);
  assert(dup.gpr[3] == 0u);
  const auto new_handle = f.address_space->read32_be(new_handle_out);
  assert(new_handle != 0u);

  kernel::HandleView view{};
  assert(f.process->handle_table().lookup(new_handle, view) == kernel::KernelIoCode::Success);
  assert(view.object.get() == thread.get());
}

void test_nt_duplicate_object_invalid_handle_is_safe() {
  Fixture f;
  cpu::CpuState dup{};
  dup.gpr[3] = 0xFFFFu;
  auto result = f.invoke(0x0DAu, dup);
  assert(result.handled && result.success);
  assert(dup.gpr[3] == 0xC0000008u);  // STATUS_INVALID_HANDLE
}

}  // namespace

int main() {
  std::cout << "Testing xboxkrnl NtDuplicateObject...\n";

  test_nt_duplicate_object_real_handle_round_trips();
  test_nt_duplicate_object_close_source_option_closes_original();
  test_nt_duplicate_object_pseudo_handle_materializes_real_handle();
  test_nt_duplicate_object_invalid_handle_is_safe();

  std::cout << "All NtDuplicateObject tests passed!\n";
  return 0;
}
