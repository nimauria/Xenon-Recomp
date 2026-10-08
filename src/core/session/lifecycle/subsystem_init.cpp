#include <iostream>
#include <mutex>

#include "xenon/core/session.hpp"
#include "xenon/kernel/xbox_io.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

bool XenonSession::init_memory() {
  memory_ = std::make_shared<memory::AddressSpace>(config_.memory_mode);
  if (!memory_->initialize()) {
    set_error("Memory subsystem initialization failed");
    return false;
  }
  return true;
}

bool XenonSession::init_filesystem() {
  filesystem_ = std::make_shared<filesystem::VirtualFileSystem>();
  return true;
}

bool XenonSession::init_kernel() {
  kernel_io_ = std::make_unique<kernel::KernelIoManager>(filesystem_);
  io_bridge_ = std::make_unique<kernel::xbox::GuestIoBridge>(*memory_, *kernel_io_);
  if (!xbox::register_xboxkrnl_io_imports(xbox_imports_)) {
    set_error("Failed to register xboxkrnl I/O import thunks");
    return false;
  }

  // Default handler for run_execution()'s guest exception dispatch (see its
  // MemoryFault/Trap catch clauses): logs a structured, thread-scoped record
  // rather than only surfacing a stringified last_error(). A native
  // extension or future debugger hook can register additional handlers via
  // exports() -> this is not exposed as a public API yet since nothing in
  // this pass needs to add a second handler.
  exception_dispatcher_.register_handler([this](const kernel::ExceptionRecord& record) {
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest exception 0x" << std::hex
                << static_cast<std::uint32_t>(record.code) << " at 0x" << record.address
                << std::dec << std::endl;
      // What the faulting thread last asked of the kernel is usually the cause.
      std::uint32_t thread_id = 0;
      if (kernel_process_) {
        if (auto current = kernel_process_->thread_manager().current_thread()) {
          thread_id = current->thread_id();
        }
      }
      for (const auto& trace : export_trace_.recent_for_thread(thread_id, 40u)) {
        std::cout << "[XenonSession]   recent: " << trace.library_view() << '!'
                  << (trace.name_view().empty() ? std::string("ord") + std::to_string(trace.ordinal)
                                                : std::string(trace.name_view()))
                  << " lr=0x" << std::hex << trace.lr << " r3=0x" << trace.arguments[0]
                  << " r4=0x" << trace.arguments[1] << " r5=0x" << trace.arguments[2] << " -> 0x"
                  << trace.result_r3 << std::dec << (trace.handled ? "" : " [UNHANDLED]")
                  << std::endl;
      }
    }
    return false;  // Do not suppress: run_execution() still reports failure.
  });
  return true;
}

bool XenonSession::init_cpu() {
  code_cache_ = std::make_unique<cpu::ExecutableCodeCache>();
  if (config_.enable_dynamic_fallback) {
    dynamic_fallback_ = std::make_unique<cpu::DynamicFallbackExecutor>(
        cpu::DynamicFallbackConfig{},
        [this](const cpu::DynamicFallbackObservation& observation) {
          record_dynamic_fallback_observation(observation);
        });
  }
  main_cpu_state_ = std::make_unique<cpu::CpuState>();
  return true;
}

bool XenonSession::init_audio() {
#if defined(XENON_HAS_AUDIO)
  if (!memory_) {
    set_error("Audio requires Memory V2");
    return false;
  }

  audio_ = std::make_unique<audio::AudioSystem>(*memory_);
  std::string error;
  // auto_start_callback_pump=false: the guest-callback pump must not run
  // until it has a real KernelThread/KPCR/TLS identity to run with, which
  // requires kernel_process_/loaded_xex_ - neither exists yet at session-init
  // time. start_audio_guest_thread() (called from create_guest_process(),
  // once a game is actually loaded) starts the pump for real.
  if (!audio_->initialize(&error, /*auto_start_callback_pump=*/false)) {
    set_error(error.empty() ? "Audio system initialization failed" : error);
    audio_.reset();
    return false;
  }
  audio_->set_master_volume(config_.audio_master_volume);

  // Render-driver callbacks execute guest code on the audio callback's own
  // KernelThread (see start_audio_guest_thread()). Give it a dedicated PPC
  // stack so it never races the main guest thread's register file.
  constexpr std::uint32_t kAudioCallbackStackSize = 128u * 1024u;
  if (!memory_->allocate(kAudioCallbackStackSize, 16, memory::kReadWrite,
                         /*top_down=*/true, audio_callback_stack_base_)) {
    set_error("Failed to allocate guest audio callback stack");
    audio_->shutdown();
    audio_.reset();
    return false;
  }
  audio_callback_stack_size_ = kAudioCallbackStackSize;
  return true;
#else
  set_error("Audio was requested, but this Xenon build has no production audio subsystem");
  return false;
#endif
}

void XenonSession::set_focused(bool focused) {
  if (input_) input_->set_focused(focused);
#if defined(XENON_HAS_AUDIO)
  if (audio_ && config_.audio_mute_unfocused) {
    audio_->set_muted(!focused);
  }
#endif
}

bool XenonSession::init_xam() {
  xam_ = std::make_unique<xam::XamSession>();
  if (!xam_->initialize()) {
    set_error("XAM subsystem initialization failed");
    return false;
  }
  return true;
}

}  // namespace xenon::core
