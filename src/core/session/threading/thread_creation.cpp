#include <algorithm>
#include <atomic>
#include <memory>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core {

// ExCreateThread (ordinal 0x0D)
// Guest ABI: r3 = PHANDLE out, r4 = stack size (0 = default), r5 = LPDWORD
// thread-id out (optional, nullable), r6 = XApiThreadStartup (ignored -
// Xenon calls start_address directly; it has no XAPI bootstrap trampoline to
// reproduce), r7 = start address, r8 = start context (the single argument
// passed to the thread function, matching PPC ABI gpr[3]), r9 = creation
// flags (bit 0 = suspended; bits 24..31 = processor affinity, informational)
// -> r3 = NTSTATUS.
bool XenonSession::export_ex_create_thread(ExportCallContext& context) {
  if (!kernel_process_ || !memory_ || !loaded_xex_) {
    // No title loaded - this export cannot possibly succeed; report a
    // real failure rather than silently doing nothing (ExportHandler
    // returning false surfaces as "unhandled export" to the caller).
    return false;
  }

  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  auto stack_size = static_cast<std::uint32_t>(context.cpu.gpr[4]);
  const auto thread_id_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  const auto start_address = static_cast<cpu::GuestAddress>(context.cpu.gpr[7]);
  const auto start_context = context.cpu.gpr[8];
  const auto creation_flags = static_cast<std::uint32_t>(context.cpu.gpr[9]);

  detail::log_title_thread_creation(start_address, context.cpu.lr, start_context,
                                    creation_flags);

  if (handle_out == 0u || start_address == 0u) {
    context.cpu.gpr[3] = kernel::xbox::status::InvalidParameter;
    return true;
  }

  // 0 means "inherit the default stack size" on real Xbox 360, matching the
  // same default create_guest_process() uses for the main thread.
  constexpr std::uint32_t kDefaultStackSize = 1u * 1024u * 1024u;
  if (stack_size == 0u) stack_size = kDefaultStackSize;
  constexpr std::uint32_t kMinimumStackSize = 4096u;
  if (stack_size < kMinimumStackSize) stack_size = kMinimumStackSize;

  memory::GuestAddress stack_base{};
  if (!memory_->allocate(stack_size, 16, memory::kReadWrite, /*top_down=*/true, stack_base)) {
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }

  GuestThreadTlsContext tls{};
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, loaded_xex_->image.tls, stack_base,
                                      stack_size, tls, &tls_error)) {
    static_cast<void>(memory_->release(stack_base));
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }

  kernel::ThreadCreationParams params{};
  params.stack_size = stack_size;
  params.name = "GuestThread";
  // ExCreateThread's own creation-flag word is NOT the Win32 CreateThread one:
  // the XAPI CreateThread wrapper folds Win32 CREATE_SUSPENDED (0x4) into bit 0
  // and puts the requested processor in the top byte (ExCreateThread flags =
  // suspended | cpu << 24; xenia: X_CREATE_SUSPENDED = 1). Reading Win32's 0x4
  // here made every suspended thread start immediately - Ace Combat 6's worker
  // pool then read event handles it had not stored yet and spun forever.
  params.create_suspended = (creation_flags & 0x1u) != 0u;
  const std::uint32_t _diag_creation_flags = creation_flags;

  // The ThreadEntry closure needs to know its own KernelThread's id (to
  // resolve the shared_ptr again via ThreadManager::get_thread() and
  // register current-thread identity in run_created_guest_thread()), but
  // ThreadManager::create_thread() has not returned that shared_ptr yet at
  // the point this closure is constructed - a real chicken-and-egg problem,
  // not an oversight. Solved with a small shared slot filled in immediately
  // below, strictly before start() is called (so thread_main() can never
  // observe it unset: the host OS thread that would read it is not created
  // until start()).
  auto thread_id_slot = std::make_shared<std::atomic<std::uint32_t>>(0u);
  auto thread = kernel_process_->thread_manager().create_thread(
      [this, start_address, start_context, stack_base, stack_size, tls,
       thread_id_slot]() -> std::uint32_t {
        auto self = kernel_process_->thread_manager().get_thread(
            thread_id_slot->load(std::memory_order_acquire));
        return run_created_guest_thread(std::move(self), start_address, start_context,
                                        stack_base, stack_size, tls);
      },
      params);
  if (!thread) {
    release_guest_thread_tls_context(*memory_, tls);
    static_cast<void>(memory_->release(stack_base));
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }
  thread_id_slot->store(thread->thread_id(), std::memory_order_release);
  write_guest_thread_id(*memory_, tls, thread->thread_id());
  thread->set_guest_kthread_address(tls.kthread_address);
  {
    logging::append_probe_log("thread_create_flags_diag.log", "ExCreateThread: thread_id=%u start_address=0x%08llX creation_flags=0x%08X create_suspended=%d\n",
                 thread->thread_id(), (unsigned long long)start_address, _diag_creation_flags,
                 params.create_suspended ? 1 : 0);
  }

  kernel::Handle handle{};
  const auto insert_code = kernel_process_->handle_table().insert(
      thread, /*granted_access=*/0xFFFFFFFFu, kernel::HandleFlags::None, handle);
  if (insert_code != kernel::KernelIoCode::Success) {
    // The thread object was already created (and, per real semantics,
    // observably exists) but could not be published as a guest handle -
    // terminate it immediately rather than leaking a runnable, unreachable
    // thread. terminate() before start() means thread_main() will observe
    // Terminated at its very first safepoint and exit without ever running
    // start_address (see thread_main()'s parked-terminate path).
    static_cast<void>(thread->terminate(0));
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }

  if (!thread->start()) {
    static_cast<void>(thread->terminate(0));
    context.cpu.gpr[3] = kernel::xbox::status::Unsuccessful;
    return true;
  }

  context.memory.write32_be(handle_out, handle);
  if (thread_id_out != 0u) {
    context.memory.write32_be(thread_id_out, thread->thread_id());
  }
  context.cpu.gpr[3] = kernel::xbox::status::Success;
  reach_boot_checkpoint(BootCheckpoint::FirstGuestThread);
  return true;
}

bool XenonSession::export_xam_task_schedule(ExportCallContext& context) {
  if (!kernel_process_ || !memory_ || !loaded_xex_) {
    return false;
  }

  const auto callback = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  const auto message_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto handle_out = static_cast<cpu::GuestAddress>(context.cpu.gpr[6]);

  if (callback == 0u || handle_out == 0u) {
    context.cpu.gpr[3] = kernel::xbox::status::InvalidParameter;
    return true;
  }

  // XamTaskSchedule's real ABI has no stack-size argument - real hardware
  // sizes the task thread's stack the same way the title's own main thread
  // is sized (rounded up to a 16 KiB page, minimum one page), so this reuses
  // the session's own default guest thread stack size rather than inventing
  // a second, separate constant.
  auto stack_size = stack_size_;
  constexpr std::uint32_t kTaskStackPage = 0x4000u;
  stack_size = (std::max)(kTaskStackPage, (stack_size + (kTaskStackPage - 1u)) & ~(kTaskStackPage - 1u));

  memory::GuestAddress stack_base{};
  if (!memory_->allocate(stack_size, 16, memory::kReadWrite, /*top_down=*/true, stack_base)) {
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }

  GuestThreadTlsContext tls{};
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, loaded_xex_->image.tls, stack_base, stack_size, tls,
                                      &tls_error)) {
    static_cast<void>(memory_->release(stack_base));
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }

  kernel::ThreadCreationParams params{};
  params.stack_size = stack_size;
  params.name = "XamTask";
  params.create_suspended = false;

  // Same chicken-and-egg resolution as export_ex_create_thread() above - see
  // its comment for why this slot exists.
  auto thread_id_slot = std::make_shared<std::atomic<std::uint32_t>>(0u);
  auto thread = kernel_process_->thread_manager().create_thread(
      [this, callback, message_ptr, stack_base, stack_size, tls, thread_id_slot]() -> std::uint32_t {
        auto self =
            kernel_process_->thread_manager().get_thread(thread_id_slot->load(std::memory_order_acquire));
        return run_created_guest_thread(std::move(self), callback,
                                        static_cast<std::uint64_t>(message_ptr), stack_base,
                                        stack_size, tls);
      },
      params);
  if (!thread) {
    release_guest_thread_tls_context(*memory_, tls);
    static_cast<void>(memory_->release(stack_base));
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }
  thread_id_slot->store(thread->thread_id(), std::memory_order_release);
  write_guest_thread_id(*memory_, tls, thread->thread_id());
  thread->set_guest_kthread_address(tls.kthread_address);

  kernel::Handle handle{};
  const auto insert_code = kernel_process_->handle_table().insert(
      thread, /*granted_access=*/0xFFFFFFFFu, kernel::HandleFlags::None, handle);
  if (insert_code != kernel::KernelIoCode::Success) {
    static_cast<void>(thread->terminate(0));
    context.cpu.gpr[3] = kernel::xbox::status::InsufficientResources;
    return true;
  }

  if (!thread->start()) {
    static_cast<void>(thread->terminate(0));
    context.cpu.gpr[3] = kernel::xbox::status::Unsuccessful;
    return true;
  }

  context.memory.write32_be(handle_out, handle);
  context.cpu.gpr[3] = kernel::xbox::status::Success;
  return true;
}

}  // namespace xenon::core
