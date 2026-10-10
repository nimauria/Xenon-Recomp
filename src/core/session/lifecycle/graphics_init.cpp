#include <string>

#include "core/session/session_internal.hpp"

#if defined(XENON_HAS_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
#endif
#if defined(XENON_HAS_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif

namespace xenon::core {

using detail::ascii_lower;

bool XenonSession::init_gpu() {
  // Null is only ever selected here because the caller explicitly asked for
  // the literal backend name "null"/"none" - SessionConfig::graphics_backend
  // defaults to "null" for callers that never touch it (e.g. headless/unit
  // test sessions with enable_graphics left false, which never reach this
  // function at all - see initialize()). Every other requested backend
  // ("automatic", "vulkan", "d3d12") either constructs a real native backend
  // or fails initialization outright; it never silently falls back to Null.
  const std::string requested = ascii_lower(config_.graphics_backend);

  // The Xenos PM4 frontend is independent of which native Backend renders
  // its output, and is needed even with a Null backend (e.g. a headless test
  // session that still wants to exercise ring-buffer decode) - constructed
  // once here, before backend selection, rather than duplicated in each
  // branch below. init_memory() (called before init_gpu() in initialize())
  // guarantees memory_ is already non-null.
  graphics_system_ = std::make_unique<gpu::GraphicsSystem>(*memory_);

  // Map the Xenos register aperture. Titles talk to the GPU through it directly
  // (vsync status polling, CP_RB_WPTR writes that publish ring-buffer commands),
  // so it belongs to the session for every backend, including a Null one. A
  // CP_RB_WPTR store moves the same write index VdSwap advances, and the GPU
  // pump thread drains from there.
  register_aperture_ = std::make_unique<gpu::XenosRegisterAperture>(
      graphics_system_->registers(), [this](std::uint32_t write_index) {
        if (kernel_process_) kernel_process_->set_gpu_ring_buffer_write_index(write_index);
      });
  if (!register_aperture_->attach(*memory_)) {
    set_error("Failed to map the Xenos register aperture at 0x7FC80000");
    return false;
  }

  // PM4_INTERRUPT packets in the command stream raise the title's graphics
  // interrupt (source 1). Titles use them as GPU->CPU handshakes - the callback
  // acknowledges by clearing a scratch word the command stream then waits on - so
  // dropping them, as an unset callback did, parks the GPU behind that wait
  // forever. Runs on the GPU pump thread, like the vsync interrupt.
  graphics_system_->set_interrupt_callback([this](std::uint32_t cpu_index) {
    if (!kernel_process_) return;
    const auto interrupt = kernel_process_->gpu_interrupt_callback();
    if (interrupt.callback_address == 0u) return;
    if (config_.enable_logging && memory_ && gpu_interrupts_delivered_.load() < 100000u) {
      // Diagnostic (first few only): what the title's interrupt handler will see.
      detail::log_title_pm4_interrupt_handler(*memory_, *graphics_system_,
                                              interrupt.callback_address, interrupt.context);
    }
    if (invoke_gpu_interrupt_callback(interrupt.callback_address, interrupt.context, 1u,
                                     cpu_index)) {
      gpu_interrupts_delivered_.fetch_add(1u, std::memory_order_relaxed);
    } else {
      gpu_interrupts_failed_.fetch_add(1u, std::memory_order_relaxed);
    }
  });

  // The GPU pump thread's vsync-driven guest interrupt callback executes on
  // its own dedicated PPC stack, exactly like the audio callback thread's
  // audio_callback_stack_base_ (see init_audio()) - allocated once here,
  // independent of which backend ends up selected below, since the pump
  // thread itself is started later in create_guest_process() regardless of
  // backend (even a Null backend still needs a live pump thread draining the
  // ring buffer for headless/test sessions).
  constexpr std::uint32_t kGpuPumpCallbackStackSize = 128u * 1024u;
  if (!memory_->allocate(kGpuPumpCallbackStackSize, 16, memory::kReadWrite,
                         /*top_down=*/true, gpu_pump_callback_stack_base_)) {
    set_error("Failed to allocate guest GPU pump callback stack");
    return false;
  }
  gpu_pump_callback_stack_size_ = kGpuPumpCallbackStackSize;

  if (requested == "null" || requested == "none") {
    gpu_ = std::make_unique<gpu::NullBackend>();
    return true;
  }

  bool want_vulkan = false;
  bool want_d3d12 = false;
  if (requested == "automatic" || requested == "auto" || requested.empty()) {
#if defined(_WIN32) && defined(XENON_HAS_D3D12)
    want_d3d12 = true;
#elif defined(XENON_HAS_VULKAN)
    want_vulkan = true;
#else
    set_error(
        "Automatic graphics backend selection failed: this Xenon build has "
        "no native graphics backend (Vulkan/D3D12) compiled in");
    return false;
#endif
  } else if (requested == "vulkan") {
    want_vulkan = true;
  } else if (requested == "d3d12" || requested == "direct3d12" || requested == "dx12") {
#if !defined(_WIN32)
    set_error("D3D12 graphics backend was requested but is only available on Windows");
    return false;
#endif
    want_d3d12 = true;
  } else {
    set_error("Unknown graphics backend requested: '" + config_.graphics_backend + "'");
    return false;
  }

  if (want_d3d12) {
#if defined(XENON_HAS_D3D12)
    auto backend = std::make_unique<gpu::d3d12::Backend>();
    if (!backend->initialize()) {
      set_error("D3D12 graphics backend initialization failed: " + backend->error());
      return false;
    }
    gpu_ = std::move(backend);
    return true;
#else
    set_error("D3D12 graphics backend was requested but this Xenon build was compiled without D3D12 support");
    return false;
#endif
  }

  if (want_vulkan) {
#if defined(XENON_HAS_VULKAN)
    auto backend = std::make_unique<gpu::vulkan::Backend>();
    if (!backend->initialize()) {
      set_error("Vulkan graphics backend initialization failed: " + backend->error());
      return false;
    }
    gpu_ = std::move(backend);
    return true;
#else
    set_error("Vulkan graphics backend was requested but this Xenon build was compiled without Vulkan support");
    return false;
#endif
  }

  set_error("Graphics backend selection failed for '" + config_.graphics_backend + "'");
  return false;
}

}  // namespace xenon::core
