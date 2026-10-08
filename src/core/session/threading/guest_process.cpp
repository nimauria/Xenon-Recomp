#include <iostream>
#include <mutex>
#include <string>

#include "xenon/core/session.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

bool XenonSession::create_guest_process() {
  if (!loaded_xex_ || !main_cpu_state_ || !memory_) {
    return false;
  }

  *main_cpu_state_ = cpu::CpuState{};
  main_cpu_state_->cia = loaded_xex_->image.entry_point;

  // Allocate the main thread's stack. 1 MiB matches the Xbox 360's typical
  // default thread stack size; games needing a different size do so via
  // their own thread creation once running, not the initial stack.
  constexpr std::uint32_t kDefaultStackSize = 1u * 1024u * 1024u;
  memory::GuestAddress stack_address{};
  if (!memory_->allocate(kDefaultStackSize, 16, memory::kReadWrite,
                        /*top_down=*/true, stack_address)) {
    set_error("Failed to allocate guest stack");
    return false;
  }
  stack_base_ = stack_address;
  stack_size_ = kDefaultStackSize;
  // PowerPC stacks grow downward; r1 starts at the top of the allocation,
  // less a small back-chain reserve as PPC ABI convention expects.
  main_cpu_state_->gpr[1] = stack_address + kDefaultStackSize - 64u;

  // Real guest process/thread model (XenonSession -> KernelProcess ->
  // KernelThread -> CPU V2), consuming XEX Loader V2's already-produced
  // output directly rather than reparsing default.xex. kernel_memory_ wraps
  // the same memory_ this session already uses (see session.hpp comment) -
  // no second Memory V2 instance or mapping set.
  kernel_memory_ = std::make_shared<kernel::KernelMemory>(memory_);
  kernel_process_ = std::make_shared<kernel::KernelProcess>(kernel_memory_);
  // One handle namespace per process: files, events and completion ports created by
  // the I/O manager live in the same table as threads, events and semaphores from
  // the sync exports. Two private tables let handle values collide and made
  // NtReadFile reject the event handle a title got from NtCreateEvent.
  if (kernel_io_) kernel_io_->share_handle_table(&kernel_process_->handle_table());

  const auto& image = loaded_xex_->image;
  const std::string module_name =
      !game_id_.empty() ? game_id_
      : !image.original_pe_name.empty() ? image.original_pe_name
                                        : std::string("default.xex");
  auto module = kernel_process_->module_manager().load_module(
      module_name, loaded_xex_->image_base,
      static_cast<std::uint32_t>(image.effective_image.size()));
  if (!module) {
    set_error("Failed to register guest module with KernelProcess");
    release_partial_guest_process();
    return false;
  }
  module->set_entry_point(image.entry_point);
  if (image.tls) {
    module->set_tls_info(image.tls->raw_data_start, image.tls->data_size,
                         image.tls->slot);
  }
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] XEX TLS descriptor: ";
    if (!image.tls) {
      std::cout << "absent";
    } else {
      std::cout << "present raw_data_start=0x" << std::hex
                << image.tls->raw_data_start
                << " raw_data_size=0x" << image.tls->raw_data_size
                << " data_size=0x" << image.tls->data_size
                << " slot=0x" << image.tls->slot
                << " index_address=0x" << image.tls->index_address
                << " callback_address=0x" << image.tls->callback_address
                << std::dec;
    }
    std::cout << std::endl;
  }

  // TLS: allocate this thread's KPCR + compiler-emitted static TLS block
  // from the XEX's already-parsed TLS metadata (image.tls), copy the raw
  // template, zero-fill the remainder, and point gpr[13] at the KPCR - see
  // guest_thread_context.hpp for exactly what real Xbox 360 semantics this
  // reproduces and what it deliberately does not model.
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, image.tls, stack_address,
                                      kDefaultStackSize, main_thread_tls_,
                                      &tls_error)) {
    set_error("Failed to set up main thread TLS: " + tls_error);
    release_partial_guest_process();
    return false;
  }
  main_cpu_state_->gpr[13] = main_thread_tls_.kpcr_address;

#if defined(XENON_HAS_AUDIO)
  // Give the audio render-driver callback a real guest thread identity
  // (KernelProcess -> KernelThread -> its own KPCR/TLS) now that
  // kernel_process_/loaded_xex_ exist, instead of leaving it unable to run
  // until start() creates the main thread - the audio callback thread is
  // independent of the main game thread and, once started, persists across
  // stop()/start() cycles until shutdown().
  if (config_.enable_audio && audio_ && !start_audio_guest_thread()) {
    release_partial_guest_process();
    return false;
  }
#endif

  // Give the GPU pump thread a real guest thread identity now that
  // kernel_process_/loaded_xex_ exist, same rationale as the audio callback
  // thread above: it independently drains the guest's PM4 ring buffer and,
  // at vsync, calls into the guest's registered graphics interrupt callback,
  // so it needs its own KernelThread/KPCR/TLS before either of those can
  // happen safely. No XENON_HAS_AUDIO-style compile-time gate - graphics is
  // always compiled in; config_.enable_graphics plus gpu_/graphics_system_
  // (both set up by init_gpu(), see initialize()) decide whether it runs.
  if (config_.enable_graphics && gpu_ && graphics_system_ && !start_gpu_pump_thread()) {
    release_partial_guest_process();
    return false;
  }

  // The main KernelThread object itself is (re)created per start() call (see
  // start()), matching the previous bare-std::thread model's "each start()
  // creates a fresh runnable thread" behavior - a kernel::KernelThread
  // cannot restart after terminating, so a process that has genuinely
  // finished/stopped needs a new thread object, not a resurrected one. The
  // process/module/TLS/KPCR set up above is per-process and stays fixed
  // across restarts, matching real Xbox process semantics.
  return true;
}

void XenonSession::release_partial_guest_process() noexcept {
  // kernel_process_ was created fresh by this same create_guest_process()
  // call and has not been published anywhere yet (load_game() only returns
  // success after this function returns true), so resetting it here is
  // enough to release its ModuleManager/ThreadManager and whatever module it
  // had registered - nothing else can be holding a reference to it.
  if (kernel_io_) kernel_io_->share_handle_table(nullptr);
  kernel_process_.reset();
  kernel_memory_.reset();
  if (memory_ && stack_base_) {
    static_cast<void>(memory_->release(stack_base_));
  }
  stack_base_ = 0;
  stack_size_ = 0;
  // Main thread TLS setup runs before the audio guest thread is started (see
  // create_guest_process()), so a later step in that same call (currently
  // only start_audio_guest_thread()) can fail with main_thread_tls_ already
  // allocated. release_guest_thread_tls_context() is a safe no-op on a
  // still-zeroed context, so this is correct whether or not TLS setup itself
  // ran yet.
  if (memory_) {
    release_guest_thread_tls_context(*memory_, main_thread_tls_);
  }
  main_thread_tls_ = {};
#if defined(XENON_HAS_AUDIO)
  // start_audio_guest_thread() cleans up its own audio_thread_/
  // audio_thread_tls_ on failure, but guard here too in case a future step is
  // ever inserted after it succeeds.
  if (audio_thread_) {
    if (audio_) audio_->stop_guest_callback_pump();
    static_cast<void>(audio_thread_->join());
    audio_thread_.reset();
  }
  if (memory_) {
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
  }
  audio_thread_tls_ = {};
#endif
  // start_gpu_pump_thread() cleans up its own gpu_pump_thread_/
  // gpu_pump_thread_tls_ on failure, but guard here too in case a future
  // step is ever inserted after it succeeds - same rationale as the audio
  // guard above. The callback stack itself (gpu_pump_callback_stack_base_)
  // was allocated in init_gpu(), not here, so - like audio_callback_stack_base_ -
  // it is released in shutdown(), not here.
  stop_gpu_pump_thread();
  if (memory_) {
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
  }
  gpu_pump_thread_tls_ = {};
}

}  // namespace xenon::core
