#include "xenon/core/session.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/exports.hpp"
#include "xenon/audio/system.hpp"
#endif

#if defined(XENON_HAS_VULKAN)
#include "xenon/gpu/vulkan/backend.hpp"
#endif
#if defined(XENON_HAS_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif

#include "xenon/input/null_driver.hpp"
#include "xenon/input/sdl_driver.hpp"
#if defined(_WIN32)
#include "xenon/input/xinput_driver.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "xenon/core/import_classification.hpp"
#include "xenon/kernel/time.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xbox/xboxkrnl_rtl_exports.hpp"
#include "xenon/xbox/xboxkrnl_rtl_string_exports.hpp"
#include "xenon/xbox/xboxkrnl_ke_irql_exports.hpp"
#include "xenon/xbox/xboxkrnl_misc_exports.hpp"
#include "xenon/xbox/xboxkrnl_string_exports.hpp"
#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_threading_exports.hpp"
#include "xenon/xbox/xboxkrnl_memory_exports.hpp"
#include "xenon/xbox/xboxkrnl_pool_exports.hpp"
#include "xenon/xbox/xboxkrnl_ob_exports.hpp"
#include "xenon/xbox/xboxkrnl_process_exports.hpp"
#include "xenon/xbox/xboxkrnl_rtl_critical_section_exports.hpp"
#include "xenon/xbox/xboxkrnl_time_exports.hpp"
#include "xenon/xbox/xboxkrnl_tls_exports.hpp"
#include "xenon/xbox/xboxkrnl_device_io_exports.hpp"
#include "xenon/xbox/xboxkrnl_video_exports.hpp"
#include "xenon/xbox/xboxkrnl_xex_module_exports.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xenon::core {

namespace {

void* load_native_library(const std::string& path, std::string* error) {
#if defined(_WIN32)
  HMODULE handle = ::LoadLibraryA(path.c_str());
  if (!handle && error) *error = "LoadLibrary failed for '" + path + "'";
  return static_cast<void*>(handle);
#else
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle && error) *error = std::string("dlopen failed: ") + dlerror();
  return handle;
#endif
}

void* resolve_native_symbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return ::dlsym(handle, name);
#endif
}

void unload_native_library(void* handle) noexcept {
  if (!handle) return;
#if defined(_WIN32)
  ::FreeLibrary(static_cast<HMODULE>(handle));
#else
  ::dlclose(handle);
#endif
}

// Contract a native extension library exports so XenonSession can bind its
// recomp-driver-generated compiled-code registry into a CPU V2
// ExecutionContext. Documented in docs/runtime/RUNTIME_HOST.md; module authors
// (e.g. Project Gracemeria) implement this once around their generated
// registry.cpp's bind_compiled_registry(ExecutionContext&).
using XenonBindCompiledRegistryFn = void (*)(cpu::ExecutionContext&);
constexpr const char* kBindCompiledRegistrySymbol = "Xenon_BindCompiledRegistry";

// Optional, additive module-identity export: a module built by the Recomp
// Driver from a specific effective XEX (base, or base+title-update -
// generate_project() emits this automatically) may export this to declare
// which effective-image SHA1 hash(es) (xbox::format_effective_image_hash()
// hex form, semicolon-separated for more than one) its compiled registry is
// valid for. A module with no such export is not identity-checked - this is
// opt-in enforcement layered on top of the required
// Xenon_BindCompiledRegistry contract, not a requirement on every module
// (see docs/runtime/RUNTIME_HOST.md "Native extension contract"). Real hardware has
// no equivalent concept; this exists purely so Xenon itself never runs code
// generated from one guest executable revision against a different one.
using XenonSupportedExecutableRevisionsFn = const char* (*)();
constexpr const char* kSupportedExecutableRevisionsSymbol = "Xenon_SupportedExecutableRevisions";

std::string ascii_lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

// `declared` is a semicolon-separated list of hex SHA1 hashes (case-
// insensitive); an empty entry between separators is ignored rather than
// treated as a (never-matching) wildcard-less empty declaration.
bool declared_revisions_include(std::string_view declared, const std::string& effective_hash_hex) {
  std::size_t start = 0u;
  while (start <= declared.size()) {
    auto end = declared.find(';', start);
    if (end == std::string_view::npos) end = declared.size();
    const auto token = ascii_lower(declared.substr(start, end - start));
    if (!token.empty() && token == effective_hash_hex) return true;
    start = end + 1u;
  }
  return false;
}

}  // namespace

void XenonSession::record_compiled_lookup_miss(
    void* observer, cpu::ExecutionContext& context, cpu::GuestAddress target,
    cpu::CompiledLookupKind kind) {
  auto* session = static_cast<XenonSession*>(observer);
  if (!session) return;
  std::lock_guard<std::mutex> observation_lock(session->adaptive_observation_mutex_);
  // Runtime learning is evidence, not an unbounded telemetry sink. A hostile
  // or badly-corrupted target stream must not grow process memory forever.
  // 65k distinct misses is already vastly more than a normal title should
  // need before the next preparation pass incorporates the new entries.
  constexpr std::size_t kMaxAdaptiveObservationFactsPerSession = 65'536u;
  if (session->adaptive_observation_seen_.size() >= kMaxAdaptiveObservationFactsPerSession) return;
  const auto fact = std::make_tuple(target, context.state.cia, kind);
  if (!session->adaptive_observation_seen_.insert(fact).second) return;

  const auto write_observation = [&](const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    // JSONL is intentionally append-only: if execution terminates abruptly we
    // still retain every observation written before the failure. The ingest
    // side deduplicates identical records and accumulates hit counts.
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out) return;
    out << "{\"address\":" << target
        << ",\"site\":" << context.state.cia
        << ",\"kind\":\""
        << (kind == cpu::CompiledLookupKind::Call ? "indirect-call-target"
                                                  : "indirect-branch-target")
        << "\",\"hits\":1";
    if (session->effective_identity_)
      out << ",\"imageHash\":\""
          << xbox::format_effective_image_hash(session->effective_identity_->effective_image_hash)
          << "\"";
    out << "}\n";
  };
  write_observation(session->config_.adaptive_observation_path);
  if (session->config_.adaptive_observation_mirror_path !=
      session->config_.adaptive_observation_path)
    write_observation(session->config_.adaptive_observation_mirror_path);
}


void XenonSession::record_dynamic_fallback_observation(
    const cpu::DynamicFallbackObservation& observation) {
  std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
  constexpr std::size_t kMaxFallbackFactsPerSession = 65'536u;
  if (dynamic_fallback_observation_seen_.size() >= kMaxFallbackFactsPerSession)
    return;
  if (!dynamic_fallback_observation_seen_
           .emplace(observation.entry, observation.block_fingerprint)
           .second)
    return;

  const auto write_observation = [&](const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out) return;
    // `executed-entry` is already consumed by Gen 5/6 analysis. The extra Gen
    // 7 fields are intentionally additive: old readers ignore them, while a
    // later knowledge-base generation can consume fingerprints/reasons.
    out << "{\"address\":" << observation.entry
        << ",\"site\":" << observation.site
        << ",\"kind\":\"executed-entry\",\"hits\":1"
        << ",\"fallback\":true"
        << ",\"exit\":" << observation.exit
        << ",\"instructions\":" << observation.instructions
        << ",\"fingerprint\":" << observation.block_fingerprint
        << ",\"fallbackReason\":\""
        << cpu::dynamic_fallback_stop_reason_name(observation.reason) << "\""
        << ",\"transferKind\":\""
        << (observation.kind == cpu::CompiledLookupKind::Call ? "call" : "branch")
        << "\"";
    if (effective_identity_)
      out << ",\"imageHash\":\""
          << xbox::format_effective_image_hash(
                 effective_identity_->effective_image_hash)
          << "\"";
    out << "}\n";
  };
  write_observation(config_.adaptive_observation_path);
  if (config_.adaptive_observation_mirror_path !=
      config_.adaptive_observation_path)
    write_observation(config_.adaptive_observation_mirror_path);
}

void XenonSession::reach_boot_checkpoint(BootCheckpoint checkpoint) {
  if (!boot_checkpoints_.reach(checkpoint)) return;
  logging::Logger::instance().log_if_enabled(
      logging::Level::Info, "boot", [&] {
        return std::string("checkpoint: ") + std::string(to_string(checkpoint));
      });
}

XenonSession::XenonSession() = default;
XenonSession::~XenonSession() {
  shutdown();
  stop_memory_watch_poll();
}

SessionState XenonSession::state() const noexcept {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return state_;
}

std::string XenonSession::last_error() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return last_error_;
}

bool XenonSession::is_initialized() const noexcept {
  const auto current = state();
  return current != SessionState::Uninitialized && current != SessionState::Failed;
}

bool XenonSession::is_running() const noexcept {
  return state() == SessionState::Running;
}

filesystem::VirtualFileSystem* XenonSession::filesystem() noexcept {
  return filesystem_.get();
}

kernel::KernelIoManager* XenonSession::kernel_io() noexcept {
  return kernel_io_.get();
}

const xbox::LoadedXex* XenonSession::loaded_xex() const noexcept {
  return loaded_xex_ ? &(*loaded_xex_) : nullptr;
}

void XenonSession::set_state(SessionState new_state, std::string message) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    state_ = new_state;
  }
  if (config_.enable_logging && !message.empty()) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] " << message << std::endl;
  }
}

void XenonSession::set_error(std::string error) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    last_error_ = error;
    state_ = SessionState::Failed;
  }
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] " << error << std::endl;
  }
}

SessionResult XenonSession::initialize(const SessionConfig& config) {
  if (is_initialized()) {
    return SessionResult::failure("Session already initialized");
  }

  set_state(SessionState::Initializing, "Initializing session...");
  config_ = config;
  export_trace_.clear();
  export_trace_.set_enabled(config_.enable_export_trace);
  memory_watch_.configure(config_.memory_watch_addresses, config_.memory_watch_history);
  {
    std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
    adaptive_observation_seen_.clear();
    dynamic_fallback_observation_seen_.clear();
  }
  boot_checkpoints_.reset();

  if (!init_memory()) {
    return SessionResult::failure("Failed to initialize memory subsystem");
  }

  if (!init_filesystem()) {
    return SessionResult::failure("Failed to initialize filesystem subsystem");
  }

  if (!init_kernel()) {
    return SessionResult::failure("Failed to initialize kernel subsystem");
  }

  if (!init_cpu()) {
    return SessionResult::failure("Failed to initialize CPU subsystem");
  }

  if (config_.enable_graphics && !init_gpu()) {
    return SessionResult::failure("Failed to initialize GPU subsystem");
  }

  if (config_.enable_input && !init_input()) {
    return SessionResult::failure("Failed to initialize input subsystem");
  }

  if (config_.enable_audio && !init_audio()) {
    return SessionResult::failure("Failed to initialize audio subsystem");
  }

  if (!init_xam()) {
    return SessionResult::failure("Failed to initialize XAM subsystem");
  }

  if (!init_exports()) {
    return SessionResult::failure("Failed to initialize export registry");
  }

  set_state(SessionState::Ready, "Session initialized successfully");
  return SessionResult::ok("Session ready", SessionState::Ready);
}

void XenonSession::shutdown() {
  if (state() == SessionState::Uninitialized) {
    return;
  }

  set_state(SessionState::Stopping, "Shutting down session...");

  // Ask the guest execution thread to stop and wait for it. There is no
  // preemption for already-running native compiled code; a caller that needs
  // a hard timeout should terminate the hosting process instead of blocking
  // here indefinitely.
  const auto shutdown_step = [this](const char* step) {
    if (!config_.enable_logging) return;
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] shutdown: " << step << std::endl;
  };
  shutdown_step("joining the main guest thread");
  stop_requested_.store(true);
  if (main_thread_) {
    static_cast<void>(main_thread_->join());
  }
  // The poll thread reads guest memory, so it must be gone before memory_ is.
  stop_memory_watch_poll();
  shutdown_step("stopping the GPU pump thread");

#if defined(XENON_HAS_AUDIO)
  // Stop the guest-callback pump and join the audio callback's own
  // KernelThread before unloading the title's compiled registry or
  // destroying kernel_process_/memory_ below - see Part 4.8's "no leaked
  // audio worker" requirement. Order matters: stop_guest_callback_pump()
  // must run before audio_thread_->join() (the pump loop only exits once it
  // observes callback_running_ false), and both must happen before
  // kernel_process_.reset() (which would otherwise try to join the same
  // thread again from ThreadManager::shutdown() while nothing is left
  // draining its work).
  if (audio_) {
    audio_->stop_guest_callback_pump();
  }
  if (audio_thread_) {
    static_cast<void>(audio_thread_->join());
    audio_thread_.reset();
  }
  if (memory_) {
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
  }
  audio_thread_tls_ = {};
  if (audio_) {
    audio_->shutdown();
    audio_.reset();
  }
  if (memory_ && audio_callback_stack_base_) {
    (void)memory_->release(audio_callback_stack_base_);
    audio_callback_stack_base_ = 0;
    audio_callback_stack_size_ = 0;
  }
#endif

  // Stop the GPU pump thread before unloading the compiled registry or
  // destroying kernel_process_/gpu_/graphics_system_/memory_ below - same
  // ordering rationale as the audio callback thread above: it must be
  // stopped and joined before kernel_process_.reset() (which would otherwise
  // try to join the same KernelThread again from ThreadManager::shutdown()
  // while nothing is left draining its work) and before gpu_.reset()/
  // graphics_system_.reset() (the pump thread is the only thread that ever
  // touches those objects after start-up, and it must not be mid-drain or
  // mid-present when they are destroyed - see the class-level doc comment on
  // start_gpu_pump_thread() for why this must be one thread, not two).
  stop_gpu_pump_thread();
  if (memory_) {
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
  }
  gpu_pump_thread_tls_ = {};
  if (memory_ && gpu_pump_callback_stack_base_) {
    (void)memory_->release(gpu_pump_callback_stack_base_);
    gpu_pump_callback_stack_base_ = 0;
    gpu_pump_callback_stack_size_ = 0;
  }

  // KNOWN ISSUE: guest worker threads parked in unbounded kernel waits are still
  // alive here, so unloading the extension can take the process down on stop.
  // Terminating them first (ThreadManager::shutdown) currently hangs because such
  // waits are not interruptible by terminate(); that needs its own fix.
  shutdown_step("unloading the native extension");
  unload_native_extension();

  // Shutdown in reverse order of initialization
  xam_.reset();

  input_bridge_.reset();
  if (input_) {
    input_->shutdown();
    input_.reset();
  }

  gpu_.reset();
  register_aperture_.reset();  // references graphics_system_'s register file
  // graphics_system_ holds a reference (not ownership) to memory_ and is only
  // ever touched by the GPU pump thread, already stopped/joined above -
  // reset it alongside gpu_ rather than leaving it dangling until the next
  // init_gpu() overwrites it or the session is destroyed.
  graphics_system_.reset();
  dynamic_fallback_.reset();
  code_cache_.reset();

  // Tear down the guest process/thread model before the memory it lives in.
  // main_thread_ was already joined above; resetting kernel_process_ (and
  // with it its ThreadManager) is then safe/idempotent.
  shutdown_step("terminating and joining guest threads");
  main_thread_.reset();
  if (kernel_io_) kernel_io_->share_handle_table(nullptr);
  kernel_process_.reset();
  kernel_memory_.reset();
  if (memory_) {
    release_guest_thread_tls_context(*memory_, main_thread_tls_);
  }
  main_thread_tls_ = {};

  io_bridge_.reset();
  kernel_io_.reset();
  filesystem_.reset();

  if (memory_) {
    memory_->reset();
    memory_.reset();
  }

  loaded_xex_.reset();
  effective_identity_.reset();
  content_graph_.reset();
  main_cpu_state_.reset();
  export_registry_.clear();

  // Every subsystem pointer above is now reset, so the session object is
  // back in the same state as right after construction (not merely
  // "stopped" - stop() leaves subsystems alive and also lands on Stopped,
  // so Stopped alone cannot mean "torn down"). Landing on Uninitialized
  // here is what makes is_initialized() correctly report false post-
  // shutdown, and lets initialize() be called again on the same object.
  set_state(SessionState::Uninitialized, "Session shut down");
}

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
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4) {
        try {
          const auto handler = memory_->read32_be(interrupt.context + 0x2A94u);
          std::scoped_lock console_log_lock(console_log_mutex());
          std::cout << "[XenonSession] PM4 interrupt -> callback 0x" << std::hex
                    << interrupt.callback_address << " ctx=0x" << interrupt.context
                    << " handler=[ctx+0x2A94]=0x" << handler << " words:";
          for (std::uint32_t i = 0; i < 8u; ++i) {
            std::cout << ' ' << memory_->read32_be(handler + i * 4u);
          }
          std::cout << " SCRATCH_UMSK=0x" << graphics_system_->registers().read(0x1DCu)
                    << " SCRATCH_ADDR=0x" << graphics_system_->registers().read(0x1DDu)
                    << std::dec << std::endl;
        } catch (const memory::MemoryFault&) {
        }
      }
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

bool XenonSession::init_input() {
  input_ = std::make_unique<input::InputSystem>();

  std::vector<std::string> requested = config_.input_drivers;
  if (requested.empty()) requested.push_back("automatic");

  bool added_real_driver = false;
  bool explicit_null = false;

  for (const auto& name : requested) {
    const std::string driver = ascii_lower(name);
    if (driver == "null" || driver == "none") {
      explicit_null = true;
      continue;
    }
    if (driver == "automatic" || driver == "auto") {
#if defined(_WIN32)
      if (auto xinput = input::create_xinput_driver()) {
        added_real_driver |= input_->add_driver(std::move(xinput));
      }
#endif
      if (auto sdl = input::create_sdl_input_driver()) {
        added_real_driver |= input_->add_driver(std::move(sdl));
      }
      continue;
    }
    if (driver == "xinput") {
#if defined(_WIN32)
      auto xinput = input::create_xinput_driver();
      if (!xinput) {
        set_error("XInput input driver requested but unavailable on this build");
        return false;
      }
      added_real_driver |= input_->add_driver(std::move(xinput));
#else
      set_error("XInput input driver requested but is only available on Windows");
      return false;
#endif
      continue;
    }
    if (driver == "sdl") {
      auto sdl = input::create_sdl_input_driver();
      if (!sdl) {
        set_error("SDL input driver requested but unavailable on this build");
        return false;
      }
      added_real_driver |= input_->add_driver(std::move(sdl));
      continue;
    }
    set_error("Unknown input driver requested: '" + name + "'");
    return false;
  }

  // Normal Play may not end up with a fully empty (zero real provider) input
  // system: that would silently strand every game that reads a controller.
  // Only an explicit "null"/"none" entry in config_.input_drivers is allowed
  // to produce a driver-less (or Null-driver-only) session, for headless/
  // test/developer configurations.
  if (!added_real_driver) {
    if (!explicit_null) {
      set_error(
          "Input subsystem requested but no real input provider (SDL/XInput) "
          "could be created on this build/platform");
      return false;
    }
    if (!input_->add_driver(std::make_unique<input::NullInputDriver>())) {
      set_error("Failed to install the explicit null input driver");
      return false;
    }
  }

  auto result = input_->setup();
  if (result != input::Result::Success) {
    set_error("Input system setup failed");
    return false;
  }

  // Apply the launcher's focus policy after setup so a foreground-only
  // session never leaks input while its presentation window is unfocused.
  input_->set_background_input_policy(
      config_.input_background ? input::BackgroundInputPolicy::Always
                               : input::BackgroundInputPolicy::ForegroundOnly);

  // The launcher exposes one simple global deadzone.  Feed it into the
  // default runtime profile rather than duplicating deadzone math in the
  // session/runtime host.  Per-device/user profile bindings still override
  // this default through ProfileStore as usual.
  if (auto profile = input_->profiles().profile("default")) {
    const auto dz = static_cast<float>(std::clamp(config_.input_deadzone, 0.0, 0.95));
    profile->left_stick.inner_deadzone = dz;
    profile->right_stick.inner_deadzone = dz;
    if (!input_->profiles().upsert(std::move(*profile))) {
      set_error("Failed to apply default input deadzone profile");
      return false;
    }
  }

  // Load persistent input profiles when the launcher supplied its profile
  // store.  A missing file is allowed on first run; malformed existing files
  // remain a hard error because silently discarding user mappings is worse
  // than surfacing the configuration problem.
  if (!config_.input_profile_store_path.empty()) {
    const std::filesystem::path profile_path(config_.input_profile_store_path);
    std::error_code ec;
    if (std::filesystem::exists(profile_path, ec) && !ec &&
        !input_->profiles().load(profile_path)) {
      set_error("Failed to load input profile store: '" +
                config_.input_profile_store_path + "'");
      return false;
    }
  }

  const auto match_device = [this](std::string_view selector)
      -> std::optional<input::DeviceId> {
    if (selector.empty()) return std::nullopt;
    const auto wanted = ascii_lower(selector);
    if (wanted == "automatic" || wanted == "auto") return std::nullopt;
    for (const auto& device : input_->devices()) {
      if (!device.connected) continue;
      if (ascii_lower(device.identity_key) == wanted ||
          ascii_lower(device.persistent_key) == wanted ||
          ascii_lower(device.name) == wanted ||
          ascii_lower(device.driver_name) == wanted) {
        return device.id;
      }
    }
    return std::nullopt;
  };

  // Preferred device is an explicit user-0 override.  If the selector no
  // longer exists (device unplugged/renamed), retain InputSystem's automatic
  // assignment rather than failing the entire game launch.
  if (auto preferred = match_device(config_.input_preferred_device)) {
    static_cast<void>(input_->assign_user(0, *preferred));
  }

  // Add launcher-defined multi-source routes (controller + keyboard/HOTAS,
  // accessibility devices, etc.). Unknown selectors are intentionally
  // ignored here so hotplug can be reconciled by a later frontend refresh.
  // SessionConfig stores one selector list per Xbox user; runtime_host fills
  // the same fixed table by launch-config userIndex.
  for (std::uint32_t user_index = 0; user_index < input::kMaxUsers; ++user_index) {
    const auto& sources = config_.input_user_sources[user_index];
    bool primary_selected = input_->device_for_user(user_index).has_value();
    for (const auto& selector : sources) {
      const auto device = match_device(selector);
      if (!device) continue;
      if (!primary_selected) {
        if (input_->assign_user(user_index, *device) == input::Result::Success) {
          primary_selected = true;
        }
      } else {
        static_cast<void>(input_->add_user_source(user_index, *device));
      }
    }
  }

  // input_rumble is retained in SessionConfig for the launcher/runtime
  // contract.  The current InputSystem has no global force-feedback gate;
  // GuestInputBridge continues to use the per-device capability path.  This
  // avoids lying by pretending a session-level toggle is already enforced.
  // A dedicated InputSystem vibration policy can consume this field later.

  input_bridge_ = std::make_unique<input::xam::guest::GuestInputBridge>(*input_);
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

bool XenonSession::init_exports() {
  // Register core exports
  export_registry_.clear();
  if (memory_) {
    module_registry_ = std::make_unique<xbox::GuestModuleRegistry>(*memory_, export_registry_);
  }
  
  // Register xboxkrnl RTL exports (RtlImageXexHeaderField, etc.)
  if (!xbox::register_xboxkrnl_rtl_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl RTL exports");
    return false;
  }

  // Register the Rtl ANSI_STRING/UNICODE_STRING/character family. Conversions
  // that allocate their destination (and the Free*String exports) reach the
  // process's kernel pool through hooks that dereference kernel_process_ at CALL
  // time - the process does not exist yet when exports are registered.
  {
    xbox::RtlPoolHooks hooks;
    hooks.allocate = [this](std::uint32_t size) -> std::uint32_t {
      return kernel_process_ ? kernel_process_->pool().allocate(size, 0x656E6F4Eu) : 0u;
    };
    hooks.free = [this](std::uint32_t address) -> bool {
      return kernel_process_ && kernel_process_->pool().free(address);
    };
    if (!xbox::register_xboxkrnl_rtl_string_exports(export_registry_, std::move(hooks))) {
      set_error("Failed to register xboxkrnl Rtl string exports");
      return false;
    }
  }

  // Register xboxkrnl guest timebase/timing exports (Phase 1 of the AC6
  // Runtime Readiness pass): KeQueryPerformanceFrequency, KeQuerySystemTime,
  // KeDelayExecutionThread, KeStallExecutionProcessor.
  if (!xbox::register_xboxkrnl_time_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl time exports");
    return false;
  }

  // Register xboxkrnl guest memory-management exports: KeFlushUserModeTb.
  if (!xbox::register_xboxkrnl_memory_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl memory exports");
    return false;
  }

  // Register xboxkrnl IRQL/critical-region/spin-lock exports
  // (KeEnterCriticalRegion, KfAcquireSpinLock, etc.) - no KernelProcess
  // dependency, so these can register directly like RTL/time/memory above.
  if (!xbox::register_xboxkrnl_ke_irql_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl Ke IRQL/spin-lock exports");
    return false;
  }

  // Register xboxkrnl misc exports (XeCryptSha, ExGetXConfigSetting,
  // ExRegisterTitleTerminateNotification) - no KernelProcess dependency.
  if (!xbox::register_xboxkrnl_misc_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl misc exports");
    return false;
  }

  // Register the xboxkrnl formatted-output family (sprintf/_snprintf/vsprintf/...
  // and their wide variants) and DbgPrint - native host code, no KernelProcess.
  if (!xbox::register_xboxkrnl_string_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl string/format exports");
    return false;
  }

  // Register the process-free threading exports (interlocked SLists,
  // NtYieldExecution, KfRaiseIrql, KeEnableFpuExceptions).
  if (!xbox::register_xboxkrnl_threading_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl threading exports");
    return false;
  }

  // Register the kernel debug exports (DbgBreakPoint*, DbgPrompt,
  // KiApcNormalRoutineNop) and KeBugCheck/KeBugCheckEx. A bugcheck is fatal on
  // real hardware: fail the session with the stop code and request the same
  // cooperative stop HalReturnToFirmware uses.
  if (!xbox::register_xboxkrnl_debug_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl debug exports");
    return false;
  }
  if (!xbox::register_xboxkrnl_bugcheck_exports(
          export_registry_, [this](const xbox::BugCheckInfo& info) {
            if (config_.enable_logging) {
              std::scoped_lock console_log_lock(console_log_mutex());
              std::cout << "[XenonSession] " << info.description << std::endl;
            }
            set_error(info.description);
            stop_requested_.store(true, std::memory_order_relaxed);
          })) {
    set_error("Failed to register xboxkrnl bugcheck exports");
    return false;
  }

  // Register xboxkrnl handle-based (Nt*) synchronization exports (Phase 1/3
  // of the AC6 Runtime Readiness pass): NtCreateEvent, NtCreateSemaphore,
  // NtReleaseSemaphore, NtCreateMutant, NtReleaseMutant,
  // NtWaitForSingleObjectEx, NtWaitForMultipleObjectsEx, NtCreateTimer,
  // NtCancelTimer, NtSetTimerEx - plus the raw-guest-memory-object (Ke*)
  // variants (Event/Semaphore only - see xex_dispatcher_header.hpp for why
  // Mutant is not supported here): KeInitializeEvent, KeInitializeSemaphore,
  // KeResetEvent, KeReleaseSemaphore, KeSetEvent, KeWaitForMultipleObjects,
  // KeWaitForSingleObject. kernel_process_
  // does not exist yet at this point in a fresh session (it is created later
  // by create_guest_process(), once a title is loaded) - like io_bridge_
  // above, these lambdas capture `this` and dereference kernel_process_ at
  // CALL time, not at registration time; by the time guest code can actually
  // invoke one of these exports, create_guest_process() has already run.
  {
    using SyncHandler = bool (*)(kernel::KernelProcess&, ExportCallContext&);
    struct SyncExportBinding {
      std::uint32_t ordinal;
      const char* name;
      SyncHandler handler;
      bool partial{false};
      const char* partial_note{};
    };
    static constexpr SyncExportBinding kSyncBindings[] = {
        {0x0D1u, "NtCreateEvent", &xbox::nt_create_event_export},
        {0x0D5u, "NtCreateSemaphore", &xbox::nt_create_semaphore_export},
        {0x0F3u, "NtReleaseSemaphore", &xbox::nt_release_semaphore_export},
        {0x0D4u, "NtCreateMutant", &xbox::nt_create_mutant_export},
        {0x0F2u, "NtReleaseMutant", &xbox::nt_release_mutant_export},
        {0x0FDu, "NtWaitForSingleObjectEx", &xbox::nt_wait_for_single_object_ex_export},
        {0x0FEu, "NtWaitForMultipleObjectsEx", &xbox::nt_wait_for_multiple_objects_ex_export},
        {0x0D7u, "NtCreateTimer", &xbox::nt_create_timer_export},
        {0x0CDu, "NtCancelTimer", &xbox::nt_cancel_timer_export},
        {0x0FAu, "NtSetTimerEx", &xbox::nt_set_timer_ex_export},
        {0x070u, "KeInitializeEvent", &xbox::ke_initialize_event_export},
        {0x074u, "KeInitializeSemaphore", &xbox::ke_initialize_semaphore_export},
        {0x08Fu, "KeResetEvent", &xbox::ke_reset_event_export},
        {0x088u, "KeReleaseSemaphore", &xbox::ke_release_semaphore_export},
        {0x09Du, "KeSetEvent", &xbox::ke_set_event_export},
        {0x0AFu, "KeWaitForMultipleObjects", &xbox::ke_wait_for_multiple_objects_export},
        {0x0B0u, "KeWaitForSingleObject", &xbox::ke_wait_for_single_object_export},
        {0x0CCu, "NtAllocateVirtualMemory", &xbox::nt_allocate_virtual_memory_export},
        {0x0DCu, "NtFreeVirtualMemory", &xbox::nt_free_virtual_memory_export},
        {0x009u, "ExAllocatePool", &xbox::ex_allocate_pool_export},
        {0x011u, "ExInitializeReadWriteLock", &xbox::ex_initialize_read_write_lock_export},
        {0x007u, "ExAcquireReadWriteLockExclusive",
         &xbox::ex_acquire_read_write_lock_exclusive_export},
        {0x008u, "ExAcquireReadWriteLockShared", &xbox::ex_acquire_read_write_lock_shared_export},
        {0x2DDu, "ExTryToAcquireReadWriteLockExclusive",
         &xbox::ex_try_to_acquire_read_write_lock_exclusive_export},
        {0x2DEu, "ExTryToAcquireReadWriteLockShared",
         &xbox::ex_try_to_acquire_read_write_lock_shared_export},
        {0x016u, "ExReleaseReadWriteLock", &xbox::ex_release_read_write_lock_export},
        {0x07Fu, "KePulseEvent", &xbox::ke_pulse_event_export},
        {0x0E6u, "NtQueryEvent", &xbox::nt_query_event_export},
        {0x0A9u, "KeSuspendThread", &xbox::ke_suspend_thread_export},
        {0x0FCu, "NtSuspendThread", &xbox::nt_suspend_thread_export},
        {0x09Cu, "KeSetDisableBoostThread", &xbox::ke_set_disable_boost_thread_export},
        {0x020u, "FscGetCacheElementCount", &xbox::fsc_get_cache_element_count_export},
        {0x021u, "FscSetCacheElementCount", &xbox::fsc_set_cache_element_count_export},
        {0x00Au, "ExAllocatePoolWithTag", &xbox::ex_allocate_pool_with_tag_export},
        {0x00Bu, "ExAllocatePoolTypeWithTag", &xbox::ex_allocate_pool_type_with_tag_export},
        {0x00Fu, "ExFreePool", &xbox::ex_free_pool_export},
        {0x013u, "ExQueryPoolBlockSize", &xbox::ex_query_pool_block_size_export},
        {0x0EEu, "NtQueryVirtualMemory", &xbox::nt_query_virtual_memory_export},
        {0x0E1u, "NtProtectVirtualMemory", &xbox::nt_protect_virtual_memory_export},
        {0x0B9u, "MmAllocatePhysicalMemory", &xbox::mm_allocate_physical_memory_export},
        {0x0C2u, "MmMapIoSpace", &xbox::mm_map_io_space_export},
        {0x0BFu, "MmIsAddressValid", &xbox::mm_is_address_valid_export},
        {0x0BBu, "MmCreateKernelStack", &xbox::mm_create_kernel_stack_export},
        {0x0BCu, "MmDeleteKernelStack", &xbox::mm_delete_kernel_stack_export},
        {0x2A4u, "KeGetImagePageTableEntry", &xbox::ke_get_image_page_table_entry_export},
        {0x28Au, "NtAllocateEncryptedMemory", &xbox::nt_allocate_encrypted_memory_export},
        {0x28Bu, "NtFreeEncryptedMemory", &xbox::nt_free_encrypted_memory_export},
        {0x0BAu, "MmAllocatePhysicalMemoryEx", &xbox::mm_allocate_physical_memory_ex_export},
        {0x0BDu, "MmFreePhysicalMemory", &xbox::mm_free_physical_memory_export},
        {0x0BEu, "MmGetPhysicalAddress", &xbox::mm_get_physical_address_export},
        {0x0C4u, "MmQueryAddressProtect", &xbox::mm_query_address_protect_export},
        {0x0C5u, "MmQueryAllocationSize", &xbox::mm_query_allocation_size_export},
        {0x0C6u, "MmQueryStatistics", &xbox::mm_query_statistics_export, true,
         "returns real total/available/used/image/virtual page counts, but leaves "
         "pool/stack/heap/page-table/cache subdivisions zero because Memory V2 "
         "does not attribute allocations to those hardware-specific buckets"},
        {0x0C7u, "MmSetAddressProtect", &xbox::mm_set_address_protect_export},
        {0x110u, "ObReferenceObjectByHandle", &xbox::ob_reference_object_by_handle_export},
        {0x105u, "ObDereferenceObject", &xbox::ob_dereference_object_export},
        {0x0DAu, "NtDuplicateObject", &xbox::nt_duplicate_object_export},
        {0x0CEu, "NtClearEvent", &xbox::nt_clear_event_export},
        {0x0F6u, "NtSetEvent", &xbox::nt_set_event_export},
        {0x0E2u, "NtPulseEvent", &xbox::nt_pulse_event_export},
        {0x019u, "ExTerminateThread", &xbox::ex_terminate_thread_export},
        {0x0F5u, "NtResumeThread", &xbox::nt_resume_thread_export},
        {0x099u, "KeSetBasePriorityThread", &xbox::ke_set_base_priority_thread_export},
        {0x081u, "KeQueryBasePriorityThread", &xbox::ke_query_base_priority_thread_export},
        {0x097u, "KeSetAffinityThread", &xbox::ke_set_affinity_thread_export},
        {0x092u, "KeResumeThread", &xbox::ke_resume_thread_export},
        {0x066u, "KeGetCurrentProcessType", &xbox::ke_get_current_process_type_export},
        {0x09Au, "KeSetCurrentProcessType", &xbox::ke_set_current_process_type_export},
        {0x125u, "RtlEnterCriticalSection", &xbox::rtl_enter_critical_section_export},
        {0x12Eu, "RtlInitializeCriticalSection", &xbox::rtl_initialize_critical_section_export},
        {0x12Fu, "RtlInitializeCriticalSectionAndSpinCount",
         &xbox::rtl_initialize_critical_section_and_spin_count_export},
        {0x130u, "RtlLeaveCriticalSection", &xbox::rtl_leave_critical_section_export},
        {0x141u, "RtlTryEnterCriticalSection", &xbox::rtl_try_enter_critical_section_export},
        {0x152u, "KeTlsAlloc", &xbox::ke_tls_alloc_export},
        {0x153u, "KeTlsFree", &xbox::ke_tls_free_export},
        {0x154u, "KeTlsGetValue", &xbox::ke_tls_get_value_export},
        {0x155u, "KeTlsSetValue", &xbox::ke_tls_set_value_export},
        // Vd* Xenos GPU control-plane exports - hand the guest's GPU command
        // ring buffer, front buffer and vblank interrupt callback over to
        // KernelProcess's Gpu*State (see xboxkrnl_video_exports.cpp/
        // process.hpp), the write side of the contract a separate,
        // already-landed XenonSession GPU pump thread workstream reads from.
        {0x1B1u, "VdCallGraphicsNotificationRoutines",
         &xbox::vd_call_graphics_notification_routines_export},
        {0x1B6u, "VdEnableRingBufferRPtrWriteBack",
         &xbox::vd_enable_ring_buffer_rptr_write_back_export},
        {0x1BAu, "VdGetCurrentDisplayInformation",
         &xbox::vd_get_current_display_information_export},
        {0x1BDu, "VdGetSystemCommandBuffer", &xbox::vd_get_system_command_buffer_export},
        {0x1C2u, "VdInitializeEngines", &xbox::vd_initialize_engines_export},
        {0x1C3u, "VdInitializeRingBuffer", &xbox::vd_initialize_ring_buffer_export},
        {0x1C6u, "VdIsHSIOTrainingSucceeded", &xbox::vd_is_hsio_training_succeeded_export},
        {0x1CAu, "VdQueryVideoMode", &xbox::vd_query_video_mode_export},
        {0x1D5u, "VdSetGraphicsInterruptCallback",
         &xbox::vd_set_graphics_interrupt_callback_export},
        {0x1D9u, "VdSetSystemCommandBufferGpuIdentifierAddress",
         &xbox::vd_set_system_command_buffer_gpu_identifier_address_export},
        {0x1DCu, "VdShutdownEngines", &xbox::vd_shutdown_engines_export},
        {0x25Bu, "VdSwap", &xbox::vd_swap_export},
        {0x0CFu, "NtClose", &xbox::nt_close_export},
        {0x0FBu, "NtSignalAndWaitForSingleObjectEx",
         &xbox::nt_signal_and_wait_for_single_object_ex_export},
        {0x1B4u, "VdEnableDisableClockGating", &xbox::vd_enable_disable_clock_gating_export},
        {0x1B9u, "VdGetCurrentDisplayGamma", &xbox::vd_get_current_display_gamma_export},
        {0x1C9u, "VdQueryVideoFlags", &xbox::vd_query_video_flags_export},
        {0x1D3u, "VdSetDisplayMode", &xbox::vd_set_display_mode_export},
        {0x1C5u, "VdInitializeScalerCommandBuffer",
         &xbox::vd_initialize_scaler_command_buffer_export},
        {0x1C7u, "VdPersistDisplay", &xbox::vd_persist_display_export},
        {0x269u, "VdRetrainEDRAM", &xbox::vd_retrain_edram_export},
        {0x26Au, "VdRetrainEDRAMWorker", &xbox::vd_retrain_edram_worker_export},
        // Low-level block-device IOCTL exports - see
        // xboxkrnl_device_io_exports.cpp for the real behavior each
        // implements; none need KernelProcess state, but match this table's
        // shared SyncHandler signature.
        {0xD9u, "NtDeviceIoControlFile", &xbox::nt_device_io_control_file_export, true,
         "only the two real IOCTLs AC6's XMountUtilityDrive cache-mounting "
         "path issues (DISK_GET_DRIVE_GEOMETRY, DISK_GET_PARTITION_INFO) are "
         "handled; any other IOCTL reports InvalidParameter"},
        {0x3Bu, "IoDismountVolume", &xbox::io_dismount_volume_export, true,
         "no dynamic per-volume mount-table entry exists to actually "
         "dismount - matches rexglue-sdk's identical precedent"},
        {0x3Cu, "IoDismountVolumeByFileHandle", &xbox::io_dismount_volume_export, true,
         "no dynamic per-volume mount-table entry exists to actually "
         "dismount - matches rexglue-sdk's identical precedent"},
        {0x259u, "StfsCreateDevice", &xbox::stfs_device_export, true,
         "the low-level STFS NT device path is not backed by a real device "
         "object - modern content access goes through XamContent*/"
         "ContentManager instead"},
        {0x25Au, "StfsControlDevice", &xbox::stfs_device_export, true,
         "the low-level STFS NT device path is not backed by a real device "
         "object - modern content access goes through XamContent*/"
         "ContentManager instead"},
    };
    for (const auto& binding : kSyncBindings) {
      core::ExportDescriptor descriptor{};
      descriptor.library = "xboxkrnl.exe";
      descriptor.name = binding.name;
      descriptor.ordinal = binding.ordinal;
      descriptor.requirement = ExportRequirement::Required;
      descriptor.partial = binding.partial;
      if (binding.partial_note) descriptor.partial_note = binding.partial_note;
      descriptor.handler = [this, fn = binding.handler](ExportCallContext& ctx) -> bool {
        if (!kernel_process_) return false;
        return fn(*kernel_process_, ctx);
      };
      if (!export_registry_.register_export(std::move(descriptor))) {
        set_error(std::string("Failed to register xboxkrnl sync export: ") + binding.name);
        return false;
      }
    }
  }

  // Register XexCheckExecutablePrivilege (ordinal 0x194 / 404 - AC6's boot
  // path calls this immediately after its first RtlEnterCriticalSection/
  // RtlLeaveCriticalSection pair). loaded_xex_ does not exist yet at this
  // point in a fresh session either - same lazy-dereference-at-call-time
  // pattern as kernel_process_ above.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "XexCheckExecutablePrivilege";
    descriptor.ordinal = 0x194u;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (!loaded_xex_) return false;
      return xbox::xex_check_executable_privilege_export(loaded_xex_->image, ctx);
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xboxkrnl XexCheckExecutablePrivilege export");
      return false;
    }
  }

  // Register XexGetModuleHandle (0x195) / XexGetProcedureAddress (0x197). They
  // resolve through module_registry_, which is (re)created above with this
  // registry, so a lookup only ever sees the exports registered here.
  {
    core::ExportDescriptor handle_descriptor{};
    handle_descriptor.library = "xboxkrnl.exe";
    handle_descriptor.name = "XexGetModuleHandle";
    handle_descriptor.ordinal = 0x195u;
    handle_descriptor.requirement = ExportRequirement::Required;
    handle_descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (!module_registry_) return false;
      return xbox::xex_get_module_handle_export(*module_registry_, ctx);
    };
    if (!export_registry_.register_export(std::move(handle_descriptor))) {
      set_error("Failed to register xboxkrnl XexGetModuleHandle export");
      return false;
    }
    core::ExportDescriptor proc_descriptor{};
    proc_descriptor.library = "xboxkrnl.exe";
    proc_descriptor.name = "XexGetProcedureAddress";
    proc_descriptor.ordinal = 0x197u;
    proc_descriptor.requirement = ExportRequirement::Required;
    proc_descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (!module_registry_) return false;
      return xbox::xex_get_procedure_address_export(*module_registry_, ctx);
    };
    if (!export_registry_.register_export(std::move(proc_descriptor))) {
      set_error("Failed to register xboxkrnl XexGetProcedureAddress export");
      return false;
    }
  }

  // Register HalReturnToFirmware (ordinal 0x28 / 40). Real hardware: void
  // HalReturnToFirmware(FIRMWARE_REENTRY routine) - routine must be 1
  // (HalRebootRoutine); control never returns to the caller (the console
  // reboots/halts). rexglue-sdk's own reference implementation
  // (HalReturnToFirmware_entry) simply calls the host's exit(0) - too abrupt
  // for Xenon, which owns a status.json/log.txt writer contract
  // (docs/runtime/RUNTIME_HOST.md) a hard process exit would skip entirely.
  // Xenon instead requests the same cooperative stop a launcher's
  // stop.signal produces (sets stop_requested_ - see "Stopping a session" in
  // RUNTIME_HOST.md), so the guest execution thread unwinds through its own
  // normal shutdown path and status.json still reaches a real terminal
  // state, rather than the process vanishing mid-write.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "HalReturnToFirmware";
    descriptor.ordinal = 0x28u;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (config_.enable_logging) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] Guest requested HalReturnToFirmware(routine="
                  << ctx.cpu.gpr[3] << ") - requesting cooperative session stop" << std::endl;
      }
      stop_requested_.store(true, std::memory_order_relaxed);
      return true;
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xboxkrnl HalReturnToFirmware export");
      return false;
    }
  }

  // Register ObCreateSymbolicLink/ObDeleteSymbolicLink (ordinals 0x103/259,
  // 0x104/260). Real hardware: NTSTATUS ObCreateSymbolicLink(PANSI_STRING
  // path, PANSI_STRING target) registers a path alias (e.g. a title mapping
  // its own logical device name to a real Xbox path) in the kernel object
  // namespace; ObDeleteSymbolicLink(PANSI_STRING path) removes one. Routed
  // to filesystem_'s real symbolic-link table
  // (filesystem::VirtualFileSystem::register_symbolic_link/
  // unregister_symbolic_link - the same mechanism the launcher/content
  // system uses), not a fabricated no-op - a later NtCreateFile-style guest
  // path lookup through that alias resolves for real. filesystem_ does not
  // exist yet at this point in a fresh session (same lazy-dereference-at-
  // call-time pattern as kernel_process_/loaded_xex_ above).
  {
    // ANSI_STRING layout (matches RtlInitAnsiString in xboxkrnl_rtl_exports.cpp):
    // +0x0 Length (u16), +0x2 MaximumLength (u16), +0x4 Buffer (u32 guest ptr).
    auto read_ansi_string = [](cpu::MemoryPort& memory, cpu::GuestAddress ptr) -> std::string {
      if (ptr == 0u) return {};
      const auto length = memory.read16_be(ptr + 0u);
      const auto buffer = memory.read32_be(ptr + 4u);
      if (buffer == 0u || length == 0u) return {};
      std::string result;
      result.reserve(length);
      for (std::uint16_t i = 0; i < length; ++i) {
        result.push_back(static_cast<char>(memory.read8(buffer + i)));
      }
      return result;
    };

    core::ExportDescriptor create_descriptor{};
    create_descriptor.library = "xboxkrnl.exe";
    create_descriptor.name = "ObCreateSymbolicLink";
    create_descriptor.ordinal = 0x103u;
    create_descriptor.requirement = ExportRequirement::Required;
    create_descriptor.handler = [this, read_ansi_string](ExportCallContext& ctx) -> bool {
      if (!filesystem_) {
        ctx.cpu.gpr[3] = kernel::xbox::status::Unsuccessful;
        return true;
      }
      auto path = read_ansi_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]));
      const auto target =
          read_ansi_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]));
      constexpr std::string_view kNtObjectPrefix = "\\??\\";
      if (path.rfind(kNtObjectPrefix, 0) == 0) path = path.substr(kNtObjectPrefix.size());

      const auto error = filesystem_->register_symbolic_link(path, target);
      ctx.cpu.gpr[3] = (error == filesystem::FsError::None)
                            ? kernel::xbox::status::Success
                            : kernel::xbox::status::Unsuccessful;
      return true;
    };
    if (!export_registry_.register_export(std::move(create_descriptor))) {
      set_error("Failed to register xboxkrnl ObCreateSymbolicLink export");
      return false;
    }

    core::ExportDescriptor delete_descriptor{};
    delete_descriptor.library = "xboxkrnl.exe";
    delete_descriptor.name = "ObDeleteSymbolicLink";
    delete_descriptor.ordinal = 0x104u;
    delete_descriptor.requirement = ExportRequirement::Required;
    delete_descriptor.handler = [this, read_ansi_string](ExportCallContext& ctx) -> bool {
      if (!filesystem_) {
        ctx.cpu.gpr[3] = kernel::xbox::status::Unsuccessful;
        return true;
      }
      auto path = read_ansi_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]));
      constexpr std::string_view kNtObjectPrefix = "\\??\\";
      if (path.rfind(kNtObjectPrefix, 0) == 0) path = path.substr(kNtObjectPrefix.size());

      const auto error = filesystem_->unregister_symbolic_link(path);
      ctx.cpu.gpr[3] = (error == filesystem::FsError::None)
                            ? kernel::xbox::status::Success
                            : kernel::xbox::status::Unsuccessful;
      return true;
    };
    if (!export_registry_.register_export(std::move(delete_descriptor))) {
      set_error("Failed to register xboxkrnl ObDeleteSymbolicLink export");
      return false;
    }
  }

  // Register ExCreateThread (Phase 1/2 of the AC6 Runtime Readiness pass).
  // Ordinal 0x0D verified against the xenia-project/xenia xboxkrnl export
  // table (xboxkrnl_table.inc). A private XenonSession method rather than a
  // free function like the sync exports above: spawning a real
  // guest-executing thread needs compiled_registry_binder_/dynamic_fallback_/
  // memory_/the XEX's TLS template, not just kernel_process_.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "ExCreateThread";
    descriptor.ordinal = 0x0Du;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      return export_ex_create_thread(ctx);
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xboxkrnl ExCreateThread export");
      return false;
    }
  }

  // Register XAM exports
  if (xam_ && !xam_->register_exports(export_registry_, *this)) {
    set_error("Failed to register XAM exports");
    return false;
  }

  // Register XamTaskSchedule (xam.xex ordinal 0x01AF). Like ExCreateThread
  // above, spawning a real guest-executing thread needs
  // compiled_registry_binder_/dynamic_fallback_/memory_/the XEX's TLS
  // template - private XenonSession state a free xam/ export file cannot
  // reach - so this is registered inline here, not in xam_session.cpp.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xam";
    descriptor.name = "XamTaskSchedule";
    descriptor.ordinal = xam::ordinal::XamTaskSchedule;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      return export_xam_task_schedule(ctx);
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xam XamTaskSchedule export");
      return false;
    }
  }

#if defined(XENON_HAS_AUDIO)
  if (config_.enable_audio && audio_ &&
      !audio::register_xbox_audio_exports(export_registry_, *audio_)) {
    set_error("Failed to register Xbox audio exports");
    return false;
  }
#endif

  // Guest XamInput* calls go through the same canonical export_registry_ as
  // XAM/Audio above (see input_bridge_'s comment in session.hpp) rather than
  // a separate dispatcher, so they reach the InputSystem owned by this
  // session regardless of which module/ordinal table a title imports them
  // from.
  if (input_bridge_) {
    for (const auto& desc : input::xam::guest::exports()) {
      core::ExportDescriptor export_desc{};
      export_desc.library = "xam";
      export_desc.name = std::string(desc.name);
      export_desc.ordinal = desc.ordinal;
      export_desc.requirement = ExportRequirement::Required;
      export_desc.handler = [this, ordinal = desc.ordinal](ExportCallContext& ctx) {
        return input_bridge_->dispatch(ordinal, ctx.cpu, ctx.memory);
      };
      if (!export_registry_.register_export(std::move(export_desc))) {
        set_error("Failed to register XamInput export");
        return false;
      }
    }
  }

  // Bridge xboxkrnl file I/O (NtCreateFile, NtReadFile, ...) into the same
  // canonical export_registry_. xbox_imports_ already carries the real
  // ordinal/thunk table (register_xboxkrnl_io_imports(), init_kernel()); each
  // thunk operates on io_bridge_, which wraps this session's own kernel_io_ -
  // so this is the active session's real filesystem state, not a global.
  if (io_bridge_) {
    for (const auto& desc : xbox_imports_.enumerate("xboxkrnl")) {
      core::ExportDescriptor export_desc{};
      export_desc.library = desc.module;
      export_desc.name = desc.name;
      export_desc.ordinal = desc.ordinal;
      export_desc.requirement = ExportRequirement::Required;
      export_desc.handler = [this, ordinal = desc.ordinal](ExportCallContext& ctx) {
        xbox::ImportCallContext import_ctx{ctx.cpu, *memory_, *io_bridge_};
        return xbox_imports_.invoke("xboxkrnl", ordinal, import_ctx);
      };
      if (!export_registry_.register_export(std::move(export_desc))) {
        set_error("Failed to register xboxkrnl I/O export");
        return false;
      }
    }
  }

  if (!init_kernel_variable_exports()) {
    set_error("Failed to initialize xboxkrnl variable exports");
    return false;
  }

  return true;
}

bool XenonSession::init_kernel_variable_exports() {
  if (!memory_) return false;

  // One compact guest page backs the kernel variables that retail titles are
  // allowed to import directly. Keeping the addresses in ExportRegistry makes
  // variable imports a first-class system-module contract rather than a
  // title-specific patch table.
  memory::GuestAddress page{};
  if (!memory_->allocate(memory::kBasePageSize, memory::kBasePageSize,
                         memory::kReadWrite, /*top_down=*/true, page)) {
    return false;
  }

  struct VariableSpec {
    std::uint32_t ordinal;
    const char* name;
    std::uint32_t offset;
  };
  // Xbox 360 xboxkrnl variable ordinals. These are platform ABI, not
  // title-specific data.
  constexpr VariableSpec kVariables[] = {
      {0x001Bu, "ExThreadObjectType", 0x000u},
      {0x0059u, "KeDebugMonitorData", 0x010u},
      {0x00ADu, "KeTimeStampBundle", 0x020u},
      {0x0158u, "XboxKrnlVersion", 0x040u},
      {0x0193u, "XexExecutableModuleHandle", 0x050u},
      {0x01AEu, "ExLoadedCommandLine", 0x060u},
      {0x01BEu, "VdGlobalDevice", 0x0A0u},
      {0x01C0u, "VdGpuClockInMHz", 0x0B0u},
      {0x01C1u, "VdHSIOCalibrationLock", 0x0C0u},
      {0x0266u, "KeCertMonitorData", 0x0E0u},
  };

  for (const auto& variable : kVariables) {
    VariableExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = variable.name;
    descriptor.ordinal = variable.ordinal;
    descriptor.guest_address = page + variable.offset;
    if (!export_registry_.register_variable(std::move(descriptor))) return false;
  }

  try {
    // ExThreadObjectType is an exported pointer variable. Give it a stable,
    // non-null guest object-type descriptor rather than leaving imported code
    // to dereference address zero. The descriptor is intentionally minimal;
    // Xenon's handle layer owns the actual host-side object type semantics.
    constexpr auto kThreadObjectTypeDescriptor = 0x200u;
    memory_->write32_be(page + 0x000u, page + kThreadObjectTypeDescriptor);

    // KeDebugMonitorData / KeCertMonitorData / VdGlobalDevice are valid null
    // values when the corresponding optional service/device is absent. The
    // allocation itself is nevertheless real, so importing code sees the
    // address of the exported variable, not a raw ordinal placeholder.
    memory_->write32_be(page + 0x010u, 0u);
    memory_->write32_be(page + 0x0A0u, 0u);
    memory_->write32_be(page + 0x0E0u, 0u);

    // KeTimeStampBundle is 24 bytes. It starts zeroed; time services can
    // refresh it later without changing the exported address.
    for (std::uint32_t offset = 0; offset < 24u; offset += 4u) {
      memory_->write32_be(page + 0x020u + offset, 0u);
    }

    // Retail-compatible kernel version storage. This follows the established
    // Xbox runtime convention used by recomp/emulation projects: major 2 and
    // permissive high build/revision fields for compatibility checks.
    memory_->write16_be(page + 0x040u, 2u);
    memory_->write16_be(page + 0x042u, 0xFFFFu);
    memory_->write16_be(page + 0x044u, 0xFFFFu);
    memory_->write8(page + 0x046u, 0x80u);
    memory_->write8(page + 0x047u, 0x00u);

    // XexExecutableModuleHandle is a pointer variable. Back it with a small
    // stable module record now; refresh_dynamic_kernel_variables() fills the
    // XEX-header pointer once the effective image has been loaded.
    constexpr auto kExecutableModuleRecord = 0x100u;
    memory_->write32_be(page + 0x050u, page + kExecutableModuleRecord);

    static constexpr char kCommandLine[] = "\"default.xex\"";
    std::vector<std::byte> command_line(sizeof(kCommandLine));
    for (std::size_t i = 0; i < sizeof(kCommandLine); ++i) {
      command_line[i] = static_cast<std::byte>(kCommandLine[i]);
    }
    memory_->write_bytes(page + 0x060u, command_line);

    // Xenos nominal GPU clock is 500 MHz.
    memory_->write32_be(page + 0x0B0u, 500u);

    // VdHSIOCalibrationLock is an RTL critical section (28 bytes). Initialize
    // the fields used by the Xbox runtime: synchronization-event type, spin
    // count / 256, signal state 0, lock count -1, recursion 0, owner 0.
    memory_->write8(page + 0x0C0u, 1u);
    memory_->write8(page + 0x0C1u, static_cast<std::uint8_t>((10000u + 255u) >> 8u));
    memory_->write32_be(page + 0x0C4u, 0u);
    memory_->write32_be(page + 0x0D0u, 0xFFFFFFFFu);
    memory_->write32_be(page + 0x0D4u, 0u);
    memory_->write32_be(page + 0x0D8u, 0u);
  } catch (const memory::MemoryFault&) {
    return false;
  }

  return true;
}

bool XenonSession::bind_xex_variable_imports() {
  if (!loaded_xex_ || !memory_) return false;

  for (const auto& import : loaded_xex_->image.imports) {
    if (!import.is_variable()) continue;

    std::optional<cpu::GuestAddress> variable;
    if (!import.symbol.empty()) {
      variable = export_registry_.resolve_variable(import.module, import.symbol);
    }
    if (!variable) {
      variable = export_registry_.resolve_variable(import.module, import.ordinal);
    }
    if (!variable) continue;  // Kept as an unresolved compatibility diagnostic.

    const auto mapping = memory_->query(import.guest_thunk);
    if (!mapping || mapping->state != memory::PageState::Committed || mapping->page_size == 0u) {
      set_error("Variable import slot is not mapped/committed: " + import.module +
                " ordinal " + std::to_string(import.ordinal));
      return false;
    }

    const auto page_size = mapping->page_size;
    const auto page_base = import.guest_thunk - (import.guest_thunk % page_size);
    const auto original_protect = mapping->current_protect;
    const auto writable_protect = original_protect | memory::Protect::Write;
    if (writable_protect != original_protect &&
        !memory_->protect(page_base, page_size, writable_protect)) {
      set_error("Failed to make variable import page writable: " + import.module +
                " ordinal " + std::to_string(import.ordinal));
      return false;
    }

    bool wrote = false;
    try {
      memory_->write32_be(import.guest_thunk, *variable);
      wrote = true;
    } catch (const memory::MemoryFault&) {
      wrote = false;
    }

    if (writable_protect != original_protect) {
      if (!memory_->protect(page_base, page_size, original_protect)) {
        set_error("Failed to restore variable import page protection: " + import.module +
                  " ordinal " + std::to_string(import.ordinal));
        return false;
      }
    }
    if (!wrote) {
      set_error("Failed to bind variable import: " + import.module + " ordinal " +
                std::to_string(import.ordinal));
      return false;
    }
  }
  return true;
}

bool XenonSession::refresh_dynamic_kernel_variables() {
  if (!memory_ || !loaded_xex_) return false;
  const auto module_handle_storage =
      export_registry_.resolve_variable("xboxkrnl.exe", 0x0193u);
  if (!module_handle_storage) return true;

  try {
    const auto module_record = memory_->read32_be(*module_handle_storage);
    if (module_record == 0u) return false;

    // Preserve a guest copy of the effective XEX header and expose its address
    // at the loader-record field used by Xbox code that queries XEX optional
    // headers. This is deliberately a minimal loader record; KernelModule is
    // still the canonical host-side module object.
    if (!loaded_xex_->image.header_bytes.empty()) {
      memory::GuestAddress header_copy{};
      const auto header_size = static_cast<std::uint32_t>(loaded_xex_->image.header_bytes.size());
      if (!memory_->allocate(header_size, 16u, memory::kReadWrite,
                             /*top_down=*/true, header_copy)) {
        return false;
      }
      memory_->write_bytes(header_copy, loaded_xex_->image.header_bytes);
      memory_->write32_be(module_record + 0x58u, header_copy);
    }
    if (module_registry_) {
      // The title's own module: reachable through XexGetModuleHandle(NULL) and by
      // its file name, and its exports through XexGetProcedureAddress.
      module_registry_->set_executable(module_record, loaded_xex_->image, {"default.xex"});
    }
  } catch (const memory::MemoryFault&) {
    return false;
  }
  return true;
}

SessionResult XenonSession::load_game(std::span<const std::byte> xex_bytes,
                                     std::string_view game_id,
                                     std::span<const std::byte> title_update_bytes) {
  if (!is_initialized()) {
    return SessionResult::failure("Session not initialized");
  }

  if (state() != SessionState::Ready) {
    return SessionResult::failure("Session not in ready state");
  }

  set_state(SessionState::LoadingGame, "Loading game...");
  game_id_ = std::string(game_id);

  // Parse the immutable base image first - always, even when a title update
  // is selected, since apply_title_update() validates the update against it
  // (title/media identity, base-signature digest, source version) and needs
  // its header/effective-image bytes to do so. xex_bytes itself is never
  // modified.
  xbox::XexImage base_image{};
  std::string error;
  if (!xbox::parse_xex_image(xex_bytes, base_image, &error)) {
    set_error("Failed to parse base XEX: " + error);
    return SessionResult::failure(last_error_);
  }

  // When a title update was selected (see mount_content_graph()/Content
  // Services), apply it through XEX Loader V2's canonical XEXP patcher and
  // make the resulting *effective* image - not the base image - what
  // actually gets mapped and executed. A malformed/incompatible update is a
  // hard, explicit launch failure here: never a silent fallback to the base
  // XEX (see docs/runtime/RUNTIME_SESSION.md's title-update integration section).
  const bool has_title_update = !title_update_bytes.empty();
  xbox::XexImage patched_image{};
  const xbox::XexImage* effective_image = &base_image;
  if (has_title_update) {
    if (!xbox::apply_title_update(base_image, title_update_bytes, patched_image, &error)) {
      set_error("Failed to apply title update: " + error);
      return SessionResult::failure(last_error_);
    }
    effective_image = &patched_image;
  }

  // Map the effective image into memory.
  xbox::LoadedXex loaded{};
  if (!xbox::map_xex_image(*memory_, *effective_image, loaded, memory::kXex64KBase, &error)) {
    set_error("Failed to map effective XEX image: " + error);
    return SessionResult::failure(last_error_);
  }

  loaded_xex_ = std::move(loaded);
  effective_identity_ =
      xbox::compute_effective_identity(base_image, has_title_update ? &patched_image : nullptr);
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Effective executable: title_id=0x" << std::hex
              << effective_identity_->title_id << " media_id=0x" << effective_identity_->media_id
              << std::dec << " title_update_applied=" << (has_title_update ? "yes" : "no")
              << " hash=" << xbox::format_effective_image_hash(effective_identity_->effective_image_hash)
              << std::endl;
  }

  // Bind true native variable imports to guest-backed system-module
  // variables before any guest code or callbacks can observe the IAT slots.
  if (!bind_xex_variable_imports()) {
    return SessionResult::failure(last_error_.empty()
                                      ? "Failed to bind XEX variable imports"
                                      : last_error_);
  }

  // Resolve imports
  if (!resolve_xex_imports()) {
    return SessionResult::failure("Failed to resolve XEX imports");
  }

  // Bind compiled code (if any)
  if (!bind_compiled_code()) {
    return SessionResult::failure("Failed to bind compiled code");
  }

  // Create guest process/main thread
  if (!create_guest_process()) {
    return SessionResult::failure("Failed to create guest process");
  }
  if (!refresh_dynamic_kernel_variables()) {
    set_error("Failed to initialize dynamic xboxkrnl variable exports");
    return SessionResult::failure(last_error_);
  }

  set_state(SessionState::Ready, "Game loaded successfully");
  reach_boot_checkpoint(BootCheckpoint::XexLoaded);
  return SessionResult::ok("Game loaded", SessionState::Ready);
}

SessionResult XenonSession::mount_content_graph(
    std::uint32_t title_id,
    const std::filesystem::path& base_content_path,
    const std::filesystem::path& title_update_path,
    const std::filesystem::path& dlc_path,
    std::uint64_t profile_xuid) {
  
  if (!filesystem_) {
    return SessionResult::failure("Filesystem not initialized");
  }
  
  if (!xam_) {
    return SessionResult::failure("XAM not initialized");
  }
  
  // Initialize content services if not already done
  auto& content_manager = xam_->content();
  if (!content_manager.save_manager()) {
    // The launcher resolves savePath for the active profile/game and forwards
    // it through SessionConfig. Direct/headless callers that do not provide
    // one fall back to a private temp root rather than guessing a platform
    // Documents directory from temp_directory_path().
    const auto save_dir = config_.save_root_path.empty()
                              ? (std::filesystem::temp_directory_path() / "Xenon" / "Saves")
                              : config_.save_root_path;
    content_manager.initialize_content_services(save_dir);
  }
  
  // Build content graph
  content_graph_ = content_manager.build_content_graph(
      title_id,
      base_content_path,
      title_update_path,
      dlc_path,
      profile_xuid
  );
  
  if (!content_graph_) {
    return SessionResult::failure("Failed to build content graph");
  }
  
  // Mount content graph to VFS
  if (!content_manager.mount_content_graph(*filesystem_, *content_graph_)) {
    return SessionResult::failure("Failed to mount content graph");
  }
  
  set_state(SessionState::Ready, "Content graph mounted successfully");
  return SessionResult::ok("Content mounted", SessionState::Ready);
}

SessionResult XenonSession::mount_content(std::string_view host_path,
                                         std::string_view guest_mount_point) {
  if (!filesystem_) {
    return SessionResult::failure("Filesystem not initialized");
  }

  // Simple legacy content mounting - just mount a host path
  // For production use, prefer mount_content_graph()
  
  return SessionResult::ok("Use mount_content_graph() for full content support");
}

SessionResult XenonSession::start() {
  if (!loaded_xex_) {
    return SessionResult::failure("No game loaded");
  }

  const auto current = state();
  if (current != SessionState::Ready && current != SessionState::Paused) {
    return SessionResult::failure("Cannot start from current state");
  }
  // A prior run's thread object stays joinable until joined even after the
  // OS thread has finished; state() already gates genuine concurrent runs
  // (it only reaches Ready/Paused again once run_execution() has returned),
  // so this reclaims that thread object rather than signaling "still busy".
  if (main_thread_) {
    static_cast<void>(main_thread_->join());
  }
  if (!native_extension_bound_) {
    return SessionResult::failure(
        "No compiled game code is available to execute: " +
        (native_extension_error_.empty()
             ? std::string("this game's module supplied no native extension")
             : native_extension_error_));
  }
  if (!kernel_process_) {
    return SessionResult::failure("No guest process available (create_guest_process() did not run)");
  }

  stop_requested_.store(false);

  // A kernel::KernelThread cannot restart after terminating (see
  // create_guest_process()'s comment), so each start() gets a fresh thread
  // object bound to the same process/module/TLS state created once at
  // load_game() time.
  kernel::ThreadCreationParams thread_params{};
  thread_params.stack_size = stack_size_;
  thread_params.name = "MainThread";
  main_thread_ = kernel_process_->thread_manager().create_thread(
      [this]() -> std::uint32_t { return run_execution(); }, thread_params);
  if (!main_thread_) {
    set_error("Failed to create main KernelThread");
    return SessionResult::failure(last_error_);
  }
  write_guest_thread_id(*memory_, main_thread_tls_, main_thread_->thread_id());
  main_thread_->set_guest_kthread_address(main_thread_tls_.kthread_address);
  kernel_process_->set_main_thread(main_thread_);
  if (FILE* _d = std::fopen("thread_identity_diag.log", "a")) {
    std::fprintf(_d, "main_thread_ assigned thread_id=%u\n", main_thread_->thread_id());
    std::fclose(_d);
  }
  // Take the watch baseline before any guest code runs.
  start_memory_watch_poll();
  if (!main_thread_->start()) {
    set_error("Failed to start main KernelThread");
    return SessionResult::failure(last_error_);
  }

  set_state(SessionState::Running, "Starting game execution...");
  return SessionResult::ok("Game started", SessionState::Running);
}

SessionResult XenonSession::pause() {
  if (state() != SessionState::Running) {
    return SessionResult::failure("Cannot pause - not running");
  }

  // There is no interpreter/yield point to suspend already-running native
  // compiled code at, so "pause" only affects state reporting today. A real
  // pause needs a cooperative checkpoint in the generated code (e.g. at
  // frame boundaries), which is native-extension work, not session wiring.
  set_state(SessionState::Paused, "Game paused (execution continues; pause is state-only)");
  return SessionResult::ok("Game paused", SessionState::Paused);
}

SessionResult XenonSession::resume() {
  if (state() != SessionState::Paused) {
    return SessionResult::failure("Cannot resume - not paused");
  }

  set_state(SessionState::Running, "Resuming game...");
  return SessionResult::ok("Game resumed", SessionState::Running);
}

SessionResult XenonSession::stop() {
  const auto current = state();
  if (current != SessionState::Running && current != SessionState::Paused) {
    return SessionResult::failure("Cannot stop - not running or paused");
  }

  set_state(SessionState::Stopping, "Stop requested...");
  stop_requested_.store(true);

  if (!execution_active_.load()) {
    // The execution thread never actually got into guest code (e.g. it
    // failed immediately), so it is safe to join synchronously here.
    if (main_thread_) static_cast<void>(main_thread_->join());
    set_state(SessionState::Stopped, "Game stopped");
    return SessionResult::ok("Game stopped", SessionState::Stopped);
  }

  // A stop that finds the guest still executing usually means it is stalled
  // (blocked in a kernel wait or spinning). Say what each thread last asked the
  // kernel for, so the stall can be diagnosed from the log alone.
  if (config_.enable_logging && export_trace_.enabled()) {
    const auto recent = export_trace_.recent_global(16u);
    std::scoped_lock console_log_lock(console_log_mutex());
    {
      std::scoped_lock lock(in_flight_exports_mutex_);
      const auto now = std::chrono::steady_clock::now();
      std::cout << "[XenonSession] Threads currently blocked inside a kernel call: "
                << in_flight_exports_.size() << std::endl;
      for (const auto& [thread_id, call] : in_flight_exports_) {
        const auto waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - call.since).count();
        std::cout << "[XenonSession]   t" << thread_id << " in ";
        if (call.descriptor) {
          std::cout << call.descriptor->library << '!' << call.descriptor->name;
        } else {
          std::cout << "ordinal " << call.ordinal;
        }
        std::cout << " for " << waited_ms << "ms lr=0x" << std::hex << call.lr << " r3=0x"
                  << call.arguments[0] << " r4=0x" << call.arguments[1] << " r5=0x"
                  << call.arguments[2] << " r6=0x" << call.arguments[3] << std::dec
                  << std::endl;
        // A thread stuck acquiring a spin lock: the lock word holds the owner's id.
        if (call.descriptor && call.descriptor->name.find("SpinLock") != std::string::npos &&
            memory_) {
          try {
            std::cout << "[XenonSession]     lock word at 0x" << std::hex << call.arguments[0]
                      << " = 0x"
                      << memory_->read32_be(static_cast<cpu::GuestAddress>(call.arguments[0]))
                      << " (owner thread id)" << std::dec << std::endl;
          } catch (const memory::MemoryFault&) {
          }
          // How did this thread get here? Its own recent kernel calls show whether
          // it already took (and failed to release) the lock.
          for (const auto& trace : export_trace_.recent_for_thread(thread_id, 12u)) {
            std::cout << "[XenonSession]     earlier: " << trace.library_view() << '!'
                      << (trace.name_view().empty() ? std::string("?")
                                                    : std::string(trace.name_view()))
                      << " lr=0x" << std::hex << trace.lr << " r3=0x" << trace.arguments[0]
                      << " r4=0x" << trace.arguments[1] << " -> 0x" << trace.result_r3
                      << std::dec << std::endl;
          }
        }
      }
      const auto interrupt = kernel_process_ ? kernel_process_->gpu_interrupt_callback()
                                             : kernel::KernelProcess::GpuInterruptCallbackState{};
      std::cout << "[XenonSession] GPU vsync interrupt: callback=0x" << std::hex
                << interrupt.callback_address << " context=0x" << interrupt.context << std::dec
                << " delivered=" << gpu_interrupts_delivered_.load()
                << " failed=" << gpu_interrupts_failed_.load() << std::endl;
      if (graphics_system_ && kernel_process_) {
        // Racy diagnostic read of counters the pump thread updates.
        const auto& gpu_stats = graphics_system_->command_processor().statistics();
        const auto ring = kernel_process_->gpu_ring_buffer();
        const auto front = kernel_process_->gpu_front_buffer();
        std::cout << "[XenonSession] GPU: packets=" << gpu_stats.packets
                  << " draws=" << gpu_stats.draws << " events=" << gpu_stats.event_packets
                  << " interrupts=" << gpu_stats.interrupt_packets
                  << " wait_stalls=" << gpu_stats.wait_stalls
                  << " ring(read=" << ring.read_index << " write=" << ring.write_index
                  << " cap=" << ring.capacity_dwords << ")"
                  << " front_buffer=0x" << std::hex << front.base_address << std::dec << " "
                  << front.width << "x" << front.height << std::endl;
        const auto& stall = graphics_system_->command_processor().last_wait_stall();
        if (stall.valid) {
          std::cout << "[XenonSession] GPU parked on WAIT_REG_MEM: "
                    << (stall.memory ? "memory 0x" : "register 0x") << std::hex << stall.address
                    << " wait_info=0x" << stall.wait_info << " reference=0x" << stall.reference
                    << " mask=0x" << stall.mask << " last value=0x" << stall.last_value
                    << std::dec << std::endl;
        }
        // The ring words at the stalled position show what the GPU is waiting behind.
        if (ring.configured() && memory_) {
          std::cout << "[XenonSession] GPU ring @read:";
          for (std::uint32_t i = 0; i < 24u; ++i) {
            std::array<std::byte, 4> word{};
            const auto index = (ring.read_index + i) % ring.capacity_dwords;
            std::uint32_t value = 0;
            if (memory_->copy_physical_range(ring.base_address + index * 4u, word)) {
              value = (std::uint32_t(word[0]) << 24) | (std::uint32_t(word[1]) << 16) |
                      (std::uint32_t(word[2]) << 8) | std::uint32_t(word[3]);
            }
            std::cout << ' ' << std::hex << value << std::dec;
          }
          std::cout << std::endl;
          // If the stalled ring packet is an INDIRECT_BUFFER, show the words around
          // the WAIT_REG_MEM inside it (the fence the GPU is parked on).
          std::array<std::byte, 12> head{};
          if (memory_->copy_physical_range(ring.base_address + ring.read_index * 4u, head)) {
            const auto be = [&](std::size_t i) {
              return (std::uint32_t(head[i]) << 24) | (std::uint32_t(head[i + 1]) << 16) |
                     (std::uint32_t(head[i + 2]) << 8) | std::uint32_t(head[i + 3]);
            };
            if ((be(0) & 0xFFFF0000u) == 0xC0010000u && ((be(0) >> 8) & 0xFFu) == 0x3Fu) {
              const std::uint32_t ib_address = be(4) & 0x1FFFFFFFu;
              const std::uint32_t ib_length = std::min<std::uint32_t>(be(8) & 0xFFFFFu, 8192u);
              std::vector<std::uint32_t> ib(ib_length);
              std::vector<std::byte> raw(ib_length * 4u);
              if (memory_->copy_physical_range(ib_address, raw)) {
                for (std::uint32_t i = 0; i < ib_length; ++i) {
                  ib[i] = (std::uint32_t(raw[i * 4]) << 24) | (std::uint32_t(raw[i * 4 + 1]) << 16) |
                          (std::uint32_t(raw[i * 4 + 2]) << 8) | std::uint32_t(raw[i * 4 + 3]);
                }
                for (std::uint32_t i = 0; i < ib_length; ++i) {
                  if ((ib[i] & 0xFFFFFFFEu) == 0xC0043C00u && i + 3u < ib_length && ib[i + 2u] == stall.address && ib[i + 3u] == stall.reference) {
                    std::cout << "[XenonSession] GPU IB 0x" << std::hex << ib_address
                              << " len=" << std::dec << ib_length << " wait at dword " << i
                              << ", context:" << std::hex;
                    for (std::uint32_t j = (i > 40u ? i - 40u : 0u);
                         j < std::min<std::uint32_t>(ib_length, i + 24u); ++j) {
                      std::cout << (j == i ? " [" : " ") << ib[j];
                    }
                    std::cout << std::dec << std::endl;
                    break;
                  }
                }
              }
            }
          }
        }
      }
      std::cout << "[XenonSession] Guest threads running outside a kernel call (cia is the last "
                   "recorded control-flow point):" << std::endl;
      for (const auto& [thread_id, guest_state] : guest_thread_states_) {
        if (in_flight_exports_.contains(thread_id)) continue;
        std::cout << "[XenonSession]   t" << thread_id << " cia=0x" << std::hex
                  << guest_state->cia << " nia=0x" << guest_state->nia << " lr=0x"
                  << guest_state->lr << " ctr=0x" << guest_state->ctr << " r1=0x"
                  << guest_state->gpr[1] << " r3=0x" << guest_state->gpr[3] << " r4=0x"
                  << guest_state->gpr[4] << std::dec << std::endl;
        if (config_.verbose_logging) {
          // A stopped thread that is spinning entirely in guest code often has
          // no recent export to identify what it is waiting for. Preserve the
          // complete architectural register set and bounded snapshots of the
          // conventional nonvolatile object-pointer registers. This is
          // diagnostic-only, read-only, and title agnostic; it deliberately
          // runs only for verbose stop reports so normal logs stay compact.
          for (std::size_t base = 0; base < guest_state->gpr.size(); base += 4u) {
            std::cout << "[XenonSession]     gpr:";
            for (std::size_t index = base;
                 index < (std::min)(base + 4u, guest_state->gpr.size()); ++index) {
              std::cout << " r" << std::dec << index << "=0x" << std::hex
                        << guest_state->gpr[index];
            }
            std::cout << std::dec << std::endl;
          }

          if (memory_) {
            std::set<cpu::GuestAddress> dumped;
            for (const std::size_t index : {31u, 30u, 29u}) {
              const auto address = static_cast<cpu::GuestAddress>(guest_state->gpr[index]);
              const auto start = address & ~cpu::GuestAddress{0xFu};
              if (address == 0u || !dumped.insert(start).second) continue;
              const auto mapping = memory_->query(address);
              if (!mapping || mapping->state != memory::PageState::Committed ||
                  !memory::has(mapping->current_protect, memory::Protect::Read)) {
                continue;
              }
              std::cout << "[XenonSession]     r" << index << " pointee=0x" << std::hex
                        << address << " allocation=0x" << mapping->allocation_base << "+0x"
                        << mapping->allocation_size << " snapshot:" << std::dec << std::endl;
              for (std::uint32_t offset = 0u; offset < 0x600u; offset += 16u) {
                std::cout << "[XenonSession]       0x" << std::hex << (start + offset) << ':';
                bool readable = true;
                try {
                  for (std::uint32_t byte = 0u; byte < 16u; ++byte) {
                    std::cout << ' ' << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned>(memory_->read8(start + offset + byte));
                  }
                } catch (const memory::MemoryFault&) {
                  readable = false;
                }
                std::cout << std::setfill(' ') << std::dec;
                if (!readable) std::cout << " <unreadable>";
                std::cout << std::endl;
                if (!readable) break;
              }
            }
          }
        }
        // The ring is shared and busy, so a spinning thread's own history can be
        // long gone; whatever is still in it is the best evidence of how it got here.
        for (const auto& trace : export_trace_.recent_for_thread(thread_id, 60u)) {
          std::cout << "[XenonSession]     earlier: " << trace.library_view() << '!'
                    << (trace.name_view().empty() ? std::string("?") : std::string(trace.name_view()))
                    << " ord=" << trace.ordinal << " lr=0x" << std::hex << trace.lr << " r3=0x"
                    << trace.arguments[0] << " r4=0x" << trace.arguments[1] << " -> 0x"
                    << trace.result_r3 << std::dec << std::endl;
        }
      }
    }
    std::cout << "[XenonSession] Stop requested while the guest was running; last "
              << recent.size() << " kernel calls (oldest first):" << std::endl;
    for (const auto& trace : recent) {
      std::cout << "[XenonSession]   t" << trace.thread_id << ' ' << trace.library_view() << '!';
      if (!trace.name_view().empty()) {
        std::cout << trace.name_view();
      } else {
        std::cout << trace.ordinal;
      }
      std::cout << " lr=0x" << std::hex << trace.lr << " r3=0x" << trace.arguments[0]
                << " r4=0x" << trace.arguments[1] << " r5=0x" << trace.arguments[2]
                << " r6=0x" << trace.arguments[3] << " -> 0x" << trace.result_r3 << std::dec
                << std::endl;
    }
  }

  if (config_.enable_logging && memory_watch_.active()) {
    // One final boundary sample so a change that landed after the last poll shows
    // up, then the whole retained history (oldest first) and the current words.
    WatchObserver final_sample{};
    final_sample.phase = WatchPhase::Poll;
    final_sample.note = "stop-report";
    sample_memory_watch(final_sample);
    const auto history = memory_watch_.history();
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Memory watch: " << memory_watch_.addresses().size()
              << " word(s), " << memory_watch_.total_changes() << " change(s), "
              << history.size() << " retained record(s):" << std::endl;
    for (const auto& record : history) {
      std::cout << "[XenonSession]   " << GuestMemoryWatch::format(record) << std::endl;
    }
    for (const auto address : memory_watch_.addresses()) {
      const auto value = memory_watch_.last_value(address);
      std::cout << "[XenonSession]   0x" << std::hex << std::uppercase << address << " = ";
      if (value) {
        std::cout << "0x" << *value;
      } else {
        std::cout << "<unreadable>";
      }
      std::cout << std::dec << std::endl;
    }
  }

  // Already-running native compiled code cannot be preempted from here. The
  // caller (runtime host) owns the actual hard-stop policy: wait for
  // stop_requested()-aware code to exit cooperatively, then terminate the
  // process if it does not. Report "Stopping" rather than blocking.
  return SessionResult::ok(
      "Stop requested; waiting for guest execution to end cooperatively",
      SessionState::Stopping);
}

bool XenonSession::resolve_xex_imports() {
  if (!loaded_xex_) {
    return false;
  }

  // XEX-native function imports contain both a type-0 address record and a
  // type-1 callable thunk. Only the callable thunk participates in function
  // export resolution. Standalone type-0 records are genuine variable
  // imports and resolve through the guest-backed variable-export registry.
  unresolved_imports_.clear();
  for (const auto& import : loaded_xex_->image.imports) {
    if (import.is_function_address()) continue;

    bool resolved = false;
    if (import.is_variable()) {
      resolved = !import.symbol.empty()
                     ? export_registry_.resolve_variable(import.module, import.symbol).has_value()
                     : export_registry_.contains_variable(import.module, import.ordinal);
    } else if (import.callable()) {
      resolved = !import.symbol.empty()
                     ? export_registry_.contains(import.module, import.symbol)
                     : export_registry_.contains(import.module, import.ordinal);
    }
    if (resolved) continue;

    const bool already_reported = std::any_of(
        unresolved_imports_.begin(), unresolved_imports_.end(),
        [&](const UnresolvedImport& existing) {
          return existing.library == import.module && existing.symbol == import.symbol &&
                 existing.ordinal == import.ordinal;
        });
    if (already_reported) continue;

    unresolved_imports_.push_back(
        UnresolvedImport{import.module, import.symbol, import.ordinal});
    if (config_.enable_export_diagnostics) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Unresolved "
                << (import.is_variable() ? "variable import: " : "import: ")
                << import.module << " '" << import.symbol << "' ordinal "
                << import.ordinal << std::endl;
    }
  }
  return true;
}

namespace {

std::string format_xex_version(const xbox::XexVersion& version) {
  std::ostringstream out;
  out << static_cast<unsigned>(version.major()) << '.'
      << static_cast<unsigned>(version.minor()) << '.' << version.build() << '.'
      << static_cast<unsigned>(version.qfe());
  return out.str();
}

}  // namespace

JsonValue XenonSession::capability_report() const {
  RunFingerprint fingerprint{};
  if (effective_identity_) {
    fingerprint.effective_xex_sha1 =
        xbox::format_effective_image_hash(effective_identity_->effective_image_hash);
    fingerprint.tu_identity =
        effective_identity_->title_update_applied
            ? (format_xex_version(effective_identity_->base_version) + "+" +
               format_xex_version(effective_identity_->effective_version))
            : "none";
  } else {
    fingerprint.tu_identity = "none";
  }
  fingerprint.gpu_backend = config_.graphics_backend;
  fingerprint.host_os = host_os_identifier();
  fingerprint.host_cpu_arch = host_cpu_arch_identifier();
  fingerprint.diagnostic_mode = config_.enable_export_diagnostics ? "verbose" : "default";

  // Copy rather than mutate capability_report_builder_ in place: producing a
  // report is logically const (it does not change what any subsystem has
  // published), even though attaching the fingerprint uses the same
  // set_section() call a subsystem would use to publish its own section.
  CapabilityReportBuilder report = capability_report_builder_;
  report.set_run_fingerprint(fingerprint);

  // Part 14 of the AC6 Runtime Readiness pass ("Runtime Fallback
  // Accounting"), plus the reviewer's fallback_unique_pc_count/
  // fallback_hot_pc_top_n addition: a title can "run" while secretly
  // executing a large share of its guest code through the Gen 7 dynamic
  // fallback safety net rather than AOT-compiled code. Computed fresh here
  // (pull, not push) from the live atomics/sets each subsystem already
  // maintains for its own purposes - no subsystem needs to proactively call
  // set_section() on every fallback event, and nothing here is paid for
  // unless a caller actually requests a report.
  {
    JsonValue fallback = JsonValue::make_object();
    const std::uint64_t aot_blocks = code_cache_ ? code_cache_->aot_lookup_hits() : 0u;
    const std::uint64_t fallback_blocks =
        dynamic_fallback_ ? dynamic_fallback_->executed_blocks() : 0u;
    const std::uint64_t fallback_instructions =
        dynamic_fallback_ ? dynamic_fallback_->executed_instructions() : 0u;
    const std::uint64_t unsupported_instructions =
        dynamic_fallback_ ? dynamic_fallback_->unsupported_instructions() : 0u;
    const std::uint64_t source_invalidations =
        dynamic_fallback_ ? dynamic_fallback_->source_invalidations() : 0u;
    fallback.set("aotBlocksExecuted", static_cast<std::int64_t>(aot_blocks));
    fallback.set("fallbackBlocksExecuted", static_cast<std::int64_t>(fallback_blocks));
    fallback.set("fallbackInstructionsExecuted",
                 static_cast<std::int64_t>(fallback_instructions));
    fallback.set("unsupportedPpcInstructions",
                 static_cast<std::int64_t>(unsupported_instructions));
    fallback.set("fallbackSourceInvalidations",
                 static_cast<std::int64_t>(source_invalidations));
    fallback.set("unsupportedSprReads",
                 static_cast<std::int64_t>(
                     unsupported_spr_reads_.load(std::memory_order_relaxed)));
    fallback.set("unsupportedSprWrites",
                 static_cast<std::int64_t>(
                     unsupported_spr_writes_.load(std::memory_order_relaxed)));

    std::size_t new_indirect_targets_discovered = 0u;
    {
      std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
      new_indirect_targets_discovered = adaptive_observation_seen_.size();
    }
    fallback.set("newIndirectTargetsDiscovered",
                 static_cast<std::int64_t>(new_indirect_targets_discovered));

    if (dynamic_fallback_) {
      fallback.set("fallbackUniquePcCount",
                   static_cast<std::int64_t>(dynamic_fallback_->fallback_unique_pc_count()));
      JsonValue hot_pcs = JsonValue::make_array();
      constexpr std::size_t kHotPcTopN = 16u;
      for (const auto& [pc, hits] : dynamic_fallback_->fallback_hot_pcs(kHotPcTopN)) {
        JsonValue entry = JsonValue::make_object();
        entry.set("pc", static_cast<std::int64_t>(pc));
        entry.set("hits", static_cast<std::int64_t>(hits));
        hot_pcs.append(std::move(entry));
      }
      fallback.set("fallbackHotPcTopN", std::move(hot_pcs));
    } else {
      fallback.set("fallbackUniquePcCount", static_cast<std::int64_t>(0));
      fallback.set("fallbackHotPcTopN", JsonValue::make_array());
    }

    report.set_section("fallback", std::move(fallback));
  }

  // Part 7 of the AC6 Runtime Readiness pass ("GPU capability / silent
  // fallback audit"): a "gpu" section built from the live backend counters
  // - see GpuUnsupportedCounters's doc comment on why these must reflect
  // reality rather than being tuned to read as zero. Omitted entirely (not
  // reported as zero) when no GPU backend exists yet, so a caller can tell
  // "not measured" apart from "measured and clean".
  if (gpu_) {
    JsonValue gpu_section = JsonValue::make_object();
    const auto unsupported = gpu_->unsupported_counters();
    gpu_section.set("unknownPackets", static_cast<std::int64_t>(unsupported.unknown_packets));
    gpu_section.set("unknownRegisters", static_cast<std::int64_t>(unsupported.unknown_registers));
    gpu_section.set("unsupportedFetchFormats",
                    static_cast<std::int64_t>(unsupported.unsupported_fetch_formats));
    gpu_section.set("unsupportedTextureFormats",
                    static_cast<std::int64_t>(unsupported.unsupported_texture_formats));
    gpu_section.set("unsupportedSamplerBehaviors",
                    static_cast<std::int64_t>(unsupported.unsupported_sampler_behaviors));
    gpu_section.set("unsupportedShaderInstructions",
                    static_cast<std::int64_t>(unsupported.unsupported_shader_instructions));
    gpu_section.set("unsupportedShaderFeatures",
                    static_cast<std::int64_t>(unsupported.unsupported_shader_features));
    gpu_section.set("unhandledResolveModes",
                    static_cast<std::int64_t>(unsupported.unhandled_resolve_modes));
    gpu_section.set("unhandledDepthStencilPaths",
                    static_cast<std::int64_t>(unsupported.unhandled_depth_stencil_paths));
    gpu_section.set("unexpectedOwnershipTransitions",
                    static_cast<std::int64_t>(unsupported.unexpected_ownership_transitions));
    gpu_section.set("failedResourceBarriers",
                    static_cast<std::int64_t>(unsupported.failed_resource_barriers));
    gpu_section.set("fallbackShaderUses",
                    static_cast<std::int64_t>(unsupported.fallback_shader_uses));
    gpu_section.set("unsupportedOperationsTotal", static_cast<std::int64_t>(unsupported.total()));

    const auto performance = gpu_->performance_counters();
    gpu_section.set("submissions", static_cast<std::int64_t>(performance.submissions));
    gpu_section.set("draws", static_cast<std::int64_t>(performance.draws));
    gpu_section.set("shaderCacheMisses", static_cast<std::int64_t>(performance.shader_cache_misses));
    gpu_section.set("resolveOperations", static_cast<std::int64_t>(performance.resolve_operations));
    gpu_section.set("textureCacheInvalidations",
                    static_cast<std::int64_t>(performance.texture_cache_invalidations));

    report.set_section("gpu", std::move(gpu_section));

    // Part 9 of the AC6 Runtime Readiness pass ("shader coverage report").
    // Shaders are discovered dynamically as the title streams
    // ir::ShaderLoad commands, so this is a live snapshot, not a
    // static "every shader known before boot" requirement.
    JsonValue shader_section = JsonValue::make_object();
    const auto coverage = gpu_->shader_coverage();
    shader_section.set("shadersDiscovered", static_cast<std::int64_t>(coverage.shaders_discovered));
    shader_section.set("shadersTranslated", static_cast<std::int64_t>(coverage.shaders_translated));
    shader_section.set("translationFailures",
                       static_cast<std::int64_t>(coverage.translation_failures));
    shader_section.set("cacheHits", static_cast<std::int64_t>(coverage.cache_hits));
    shader_section.set("cacheMisses", static_cast<std::int64_t>(coverage.cache_misses));
    report.set_section("shader", std::move(shader_section));
  }

  // Part 12 of the AC6 Runtime Readiness pass ("Title Update fidelity"):
  // "Base SHA1, TU identity, Effective SHA1, Effective version" as its own
  // section, distinct from runFingerprint's single effective_xex_sha1/
  // tu_identity (which intentionally only describes what actually ran).
  // Omitted (not zeroed) until a title is actually loaded.
  if (effective_identity_) {
    JsonValue title_update_section = JsonValue::make_object();
    title_update_section.set(
        "baseSha1", xbox::format_effective_image_hash(effective_identity_->base_image_hash));
    title_update_section.set(
        "effectiveSha1", xbox::format_effective_image_hash(effective_identity_->effective_image_hash));
    title_update_section.set("titleUpdateApplied", effective_identity_->title_update_applied);
    title_update_section.set(
        "tuIdentity",
        effective_identity_->title_update_applied
            ? (format_xex_version(effective_identity_->base_version) + "+" +
               format_xex_version(effective_identity_->effective_version))
            : std::string("none"));
    title_update_section.set("effectiveVersion",
                            format_xex_version(effective_identity_->effective_version));
    report.set_section("titleUpdate", std::move(title_update_section));
  }

  // Part 17 of the AC6 Runtime Readiness pass: the whole-XEX import
  // capability audit (Part 3/4's classify_import()/
  // compute_import_capability_verdict(), already real and tested via
  // tools/recomp_tools.cpp's standalone import-scanner and
  // tests/core/import_capability_report_tests.cpp) surfaced directly in
  // the live session report, so a caller does not need to separately run
  // the offline tool against the XEX file to get the same classification.
  // Omitted until a title is loaded, matching "titleUpdate"/"gpu"'s own
  // convention.
  if (loaded_xex_) {
    std::size_t implemented = 0u, safe_stub = 0u, partial = 0u, missing = 0u;
    JsonValue partial_notes = JsonValue::make_array();
    for (const auto& import : loaded_xex_->image.imports) {
      if (import.is_function_address()) continue;  // Not separately resolved - see resolve_xex_imports().

      const auto* descriptor = !import.symbol.empty()
                                    ? export_registry_.resolve(import.module, import.symbol)
                                    : export_registry_.resolve(import.module, import.ordinal);
      switch (classify_import(descriptor)) {
        case ImportClassification::Implemented: ++implemented; break;
        case ImportClassification::SafeStub: ++safe_stub; break;
        case ImportClassification::Partial: ++partial; break;
        case ImportClassification::Missing: ++missing; break;
      }
      if (descriptor != nullptr && !descriptor->partial_note.empty()) {
        JsonValue entry = JsonValue::make_object();
        entry.set("library", import.module);
        entry.set("name", descriptor->name);
        entry.set("note", descriptor->partial_note);
        partial_notes.append(std::move(entry));
      }
    }

    JsonValue imports_section = JsonValue::make_object();
    imports_section.set("implemented", static_cast<std::int64_t>(implemented));
    imports_section.set("safeStub", static_cast<std::int64_t>(safe_stub));
    imports_section.set("partial", static_cast<std::int64_t>(partial));
    imports_section.set("missing", static_cast<std::int64_t>(missing));
    imports_section.set(
        "verdict",
        std::string(to_string(
            compute_import_capability_verdict(implemented, safe_stub, partial, missing))));
    imports_section.set("partialNotes", std::move(partial_notes));
    report.set_section("imports", std::move(imports_section));
  }

  // Reviewer feedback addition 3 on the AC6 Runtime Readiness pass
  // ("RunFingerprint + kernel-object liveness accounting"): RunFingerprint
  // itself has existed since Phase 0 (see set_run_fingerprint() above); this
  // is the liveness half. Surfaces real, live counts from
  // kernel::ThreadManager/kernel::HandleTable - not a running total, a
  // point-in-time snapshot, so a leak (a session whose live count keeps
  // growing across many created-and-finished guest threads/objects instead
  // of returning to baseline) is actually observable. Omitted until a
  // kernel process exists, matching "gpu"/"shader"'s own convention.
  if (kernel_process_) {
    // Opportunistic reap before reading liveThreads below, so this figure
    // reflects real, current liveness rather than "threads ever created
    // minus threads reaped by unrelated activity elsewhere" - cleanup is
    // lazy (see ThreadManager::reap_finished_threads()'s doc comment), so a
    // session that hasn't triggered a reap via any other path recently
    // would otherwise report a stale, inflated count here.
    kernel_process_->thread_manager().reap_finished_threads();

    JsonValue kernel_objects_section = JsonValue::make_object();
    kernel_objects_section.set(
        "liveThreads",
        static_cast<std::int64_t>(kernel_process_->thread_manager().thread_count()));
    kernel_objects_section.set(
        "liveHandles",
        static_cast<std::int64_t>(kernel_process_->handle_table().size()));
    report.set_section("kernelObjects", std::move(kernel_objects_section));
  }

  // Part 15 of the AC6 Runtime Readiness pass ("boot phase checkpoints"):
  // always published (unlike "gpu"/"shader", this needs no subsystem to
  // exist) - the reached checkpoints in the order they actually happened,
  // so a stalled run makes the last real progress point obvious.
  {
    JsonValue boot_section = JsonValue::make_object();
    JsonValue checkpoints = JsonValue::make_array();
    for (const auto checkpoint : boot_checkpoints_.reached_in_order()) {
      checkpoints.append(JsonValue(std::string(to_string(checkpoint))));
    }
    boot_section.set("reached", std::move(checkpoints));
    report.set_section("boot", std::move(boot_section));
  }

  // Part 17 of the AC6 Runtime Readiness pass ("AC6 capability report"),
  // plus the reviewer's PASS/PASS_WITH_FALLBACK/FAIL addition: a single
  // top-level verdict synthesized from every section already published
  // above, rather than a separate subsystem of its own. This never invents
  // new telemetry - it only reads counters/state each subsystem already
  // maintains for its own diagnostic purposes, so the verdict can never
  // read cleaner than the sections it is built from.
  //
  // FAIL is reserved for signals that mean part of the run could not
  // execute at all (a recorded session failure, or a loaded title whose
  // native compiled code never got bound). Everything else that indicates
  // a *degraded but completed* run - unresolved imports that were never
  // actually called, guest code that ran through the Gen 7 dynamic
  // fallback instead of AOT-compiled code, unsupported GPU operations, or
  // shader translation failures - is PASS_WITH_FALLBACK, since the title
  // still produced output rather than crashing outright.
  {
    JsonValue verdict_section = JsonValue::make_object();
    std::vector<std::string> fail_reasons;
    std::vector<std::string> fallback_reasons;

    if (state() == SessionState::Failed) {
      const auto error = last_error();
      fail_reasons.push_back(error.empty() ? "session reported a failure with no message"
                                            : "session failed: " + error);
    }
    if (loaded_xex_ && !native_extension_bound_) {
      fail_reasons.push_back(
          native_extension_error_.empty()
              ? "native extension (compiled guest code) never bound"
              : "native extension not bound: " + native_extension_error_);
    }

    if (!unresolved_imports_.empty()) {
      fallback_reasons.push_back(std::to_string(unresolved_imports_.size()) +
                                  " unresolved import(s)");
    }
    if (dynamic_fallback_) {
      if (dynamic_fallback_->executed_blocks() > 0u) {
        fallback_reasons.push_back(
            std::to_string(dynamic_fallback_->executed_blocks()) +
            " guest code block(s) executed via the Gen 7 dynamic fallback instead of AOT");
      }
      if (dynamic_fallback_->unsupported_instructions() > 0u) {
        fallback_reasons.push_back(
            std::to_string(dynamic_fallback_->unsupported_instructions()) +
            " unsupported PPC instruction(s) encountered");
      }
    }
    {
      const auto spr_reads = unsupported_spr_reads_.load(std::memory_order_relaxed);
      const auto spr_writes = unsupported_spr_writes_.load(std::memory_order_relaxed);
      if (spr_reads > 0u) {
        fallback_reasons.push_back(std::to_string(spr_reads) +
                                    " unsupported SPR read(s) encountered");
      }
      if (spr_writes > 0u) {
        fallback_reasons.push_back(std::to_string(spr_writes) +
                                    " unsupported SPR write(s) encountered");
      }
    }
    if (const auto* imports_section = report.find_section("imports")) {
      const auto safe_stub = imports_section->get_number("safeStub");
      const auto partial = imports_section->get_number("partial");
      if (safe_stub > 0.0) {
        fallback_reasons.push_back(std::to_string(static_cast<std::int64_t>(safe_stub)) +
                                    " import(s) resolve to a safe stub");
      }
      if (partial > 0.0) {
        fallback_reasons.push_back(std::to_string(static_cast<std::int64_t>(partial)) +
                                    " import(s) resolve to a documented partial implementation");
      }
    }
    if (gpu_) {
      const auto unsupported = gpu_->unsupported_counters();
      if (unsupported.total() > 0u) {
        fallback_reasons.push_back(std::to_string(unsupported.total()) +
                                    " unsupported GPU operation(s)");
      }
      const auto coverage = gpu_->shader_coverage();
      if (coverage.translation_failures > 0u) {
        fallback_reasons.push_back(std::to_string(coverage.translation_failures) +
                                    " shader translation failure(s)");
      }
    }

    const char* verdict_state = !fail_reasons.empty()
                                     ? "FAIL"
                                     : (!fallback_reasons.empty() ? "PASS_WITH_FALLBACK" : "PASS");
    verdict_section.set("state", std::string(verdict_state));

    JsonValue fail_array = JsonValue::make_array();
    for (auto& reason : fail_reasons) fail_array.append(JsonValue(std::move(reason)));
    verdict_section.set("failReasons", std::move(fail_array));

    JsonValue fallback_array = JsonValue::make_array();
    for (auto& reason : fallback_reasons) fallback_array.append(JsonValue(std::move(reason)));
    verdict_section.set("fallbackReasons", std::move(fallback_array));

    report.set_section("verdict", std::move(verdict_section));
  }

  return report.build();
}

bool XenonSession::bind_compiled_code() {
  if (!loaded_xex_) {
    return false;
  }

  load_native_extension();
  // A missing/failed native extension is not a load failure: the session
  // still loads (imports resolved, content mounted) so status/UI can report
  // exactly what is missing. start() refuses to run without a bound
  // registry rather than silently doing nothing.
  return true;
}

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

void XenonSession::load_native_extension() {
  native_extension_bound_ = false;
  native_extension_error_.clear();
  compiled_registry_binder_ = nullptr;
  unload_native_extension();

  if (config_.native_extension_path.empty()) {
    native_extension_error_ = "no native extension configured for this game";
    return;
  }

  std::string load_error;
  native_extension_handle_ = load_native_library(config_.native_extension_path, &load_error);
  if (!native_extension_handle_) {
    native_extension_error_ = load_error.empty()
                                   ? "failed to load native extension library"
                                   : load_error;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Native extension load failed: " << native_extension_error_
                << std::endl;
    }
    return;
  }

  auto* symbol = resolve_native_symbol(native_extension_handle_, kBindCompiledRegistrySymbol);
  if (!symbol) {
    native_extension_error_ =
        std::string("native extension does not export '") + kBindCompiledRegistrySymbol + "'";
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Native extension load failed: " << native_extension_error_
                << std::endl;
    }
    unload_native_extension();
    return;
  }

  // Module-compatibility validation: if this module declares which
  // effective-executable revision(s) it was compiled for, the currently
  // loaded effective image (base, or base+title-update - see load_game())
  // must be one of them. A module that declares nothing is not checked
  // (back-compat with modules that predate this contract); a module that
  // does declare revisions and does not include the running one is rejected
  // outright rather than silently run against code it was never generated
  // from (see docs/runtime/RUNTIME_HOST.md's "Effective executable identity"
  // section).
  if (effective_identity_) {
    if (auto* revisions_symbol =
            resolve_native_symbol(native_extension_handle_, kSupportedExecutableRevisionsSymbol)) {
      auto* revisions_fn = reinterpret_cast<XenonSupportedExecutableRevisionsFn>(revisions_symbol);
      const char* declared_raw = revisions_fn();
      const std::string declared = declared_raw ? declared_raw : "";
      if (!declared.empty()) {
        const auto effective_hash_hex =
            ascii_lower(xbox::format_effective_image_hash(effective_identity_->effective_image_hash));
        if (!declared_revisions_include(declared, effective_hash_hex)) {
          native_extension_error_ =
              "native extension does not declare compatibility with the effective executable "
              "revision (hash " + effective_hash_hex + "); module declares: " + declared;
          if (config_.enable_logging) {
            std::scoped_lock console_log_lock(console_log_mutex());
            std::cout << "[XenonSession] Native extension rejected: " << native_extension_error_
                      << std::endl;
          }
          unload_native_extension();
          return;
        }
      }
    }
  }

  auto* bind_fn = reinterpret_cast<XenonBindCompiledRegistryFn>(symbol);
  compiled_registry_binder_ = [bind_fn](cpu::ExecutionContext& context) { bind_fn(context); };
  native_extension_bound_ = true;
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Native extension bound: " << config_.native_extension_path
              << std::endl;
  }
}

void XenonSession::unload_native_extension() noexcept {
  if (native_extension_handle_) {
    unload_native_library(native_extension_handle_);
    native_extension_handle_ = nullptr;
  }
}

XenonSession::GuestDispatchOutcome XenonSession::dispatch_guest_thread(
    cpu::CpuState& state, cpu::GuestAddress entry,
    const std::shared_ptr<kernel::KernelThread>& thread,
    bool trace_dispatches) {
  GuestDispatchOutcome outcome{};

  // Publish this thread's register file for the stall report in stop(). Reads
  // there are deliberately racy and diagnostic-only.
  const auto tracked_thread_id = thread ? thread->thread_id() : 0u;
  if (tracked_thread_id != 0u) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    guest_thread_states_[tracked_thread_id] = &state;
  }
  struct StateRegistration {
    XenonSession* session;
    std::uint32_t thread_id;
    ~StateRegistration() {
      if (thread_id == 0u) return;
      std::scoped_lock lock(session->in_flight_exports_mutex_);
      session->guest_thread_states_.erase(thread_id);
    }
  } state_registration{this, tracked_thread_id};

  cpu::ExecutionContext context(state, *memory_, *this);
  if (dynamic_fallback_) dynamic_fallback_->bind(context);
  if (!config_.adaptive_observation_path.empty() ||
      !config_.adaptive_observation_mirror_path.empty()) {
    context.compiled_lookup_observer = this;
    context.compiled_lookup_miss = &XenonSession::record_compiled_lookup_miss;
  }
  if (compiled_registry_binder_) {
    compiled_registry_binder_(context);
  }

  if (!context.compiled_lookup && !context.dynamic_fallback) {
    outcome.crashed = true;
    outcome.crash_message = "No compiled game code or dynamic fallback is available to execute";
    outcome.crash_exit_code = 0xFFFFFFFFu;
    return outcome;
  }

  auto* entry_fn = context.lookup_compiled(entry, cpu::CompiledLookupKind::Call);
  {
    if (FILE* _d = std::fopen("dispatch_lookup_diag.log", "a")) {
      std::fprintf(_d, "dispatch_guest_thread lookup: thread_id=%u entry=0x%08llX entry_fn=%p\n",
                   tracked_thread_id, (unsigned long long)entry, (void*)entry_fn);
      std::fclose(_d);
    }
  }

  cpu::ExecutionResult result{};
  bool crashed = false;
  std::string crash_message;
  std::uint32_t crash_exit_code = 0xC0000005u;  // NTSTATUS-style default (access violation).
  bool thread_terminated_mid_dispatch = false;
  const auto fail_execution = [&](std::string message, std::uint32_t exit_code) {
    crashed = true;
    crash_message = std::move(message);
    crash_exit_code = exit_code;
  };
  try {
    if (entry_fn) {
      if (FILE* _d = std::fopen("dispatch_lookup_diag.log", "a")) {
        std::fprintf(_d, "dispatch_guest_thread: thread_id=%u ABOUT TO CALL entry_fn=%p for entry=0x%08llX\n",
                     tracked_thread_id, (void*)entry_fn, (unsigned long long)entry);
        std::fclose(_d);
      }
      result = entry_fn(context);
      if (FILE* _d = std::fopen("dispatch_lookup_diag.log", "a")) {
        std::fprintf(_d, "dispatch_guest_thread: thread_id=%u entry_fn RETURNED reason=%s next=0x%08llX\n",
                     tracked_thread_id, std::string(cpu::flow_reason_name(result.reason)).c_str(),
                     (unsigned long long)result.next_address);
        std::fclose(_d);
      }
    } else {
      const auto fallback =
          context.try_dynamic_fallback(entry, cpu::CompiledLookupKind::Call);
      if (!fallback.handled) {
        std::ostringstream address;
        address << std::hex << std::uppercase << entry;
        fail_execution("The compiled registry has no entry for 0x" +
                           address.str() +
                           " and the target is not fallback-executable",
                       0xC000001Du);
      } else {
        result = fallback.result;
      }
    }
    if (!crashed && trace_dispatches && config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest entry returned: reason="
                << cpu::flow_reason_name(result.reason)
                << " next=0x" << std::hex << std::uppercase << result.next_address
                << " detail=0x" << result.detail
                << " cia=0x" << state.cia
                << " nia=0x" << state.nia
                << " lr=0x" << state.lr
                << " ctr=0x" << state.ctr
                << " r1=0x" << state.gpr[1]
                << " r3=0x" << state.gpr[3] << std::dec << std::endl;
    }

    constexpr std::uint32_t kMaxTopLevelDispatches = 1'000'000u;
    std::uint32_t dispatch_count = 0u;
    for (;
         !crashed && !stop_requested_.load() &&
         !(thread && thread->is_terminated()) &&
         dispatch_count < kMaxTopLevelDispatches;) {
      // Preemptive safepoint: parks if `thread` was suspend()ed by another
      // host thread (cheap no-op otherwise), then re-checks termination in
      // case terminate() raced with the suspend/resume - a thread must not
      // dispatch one more block after being terminated while parked here.
      if (thread) {
        thread->wait_while_suspended();
        if (thread->is_terminated()) {
          thread_terminated_mid_dispatch = true;
          break;
        }
      }
      if (memory_watch_.active()) {
        WatchObserver observer{};
        observer.phase = WatchPhase::DispatchBoundary;
        observer.thread_id = tracked_thread_id;
        observer.cia = static_cast<cpu::GuestAddress>(state.cia);
        observer.nia = static_cast<cpu::GuestAddress>(state.nia);
        observer.lr = state.lr;
        observer.describe = [this, tracked_thread_id] {
          return describe_recent_exports(tracked_thread_id, 6u);
        };
        sample_memory_watch(observer);
      }
      switch (result.reason) {
        case cpu::FlowReason::Return:
          // A Return reaching this OUTER dispatch loop (as opposed to being
          // absorbed by a nested C++ call's own "if next != expected, return
          // rr" propagation - see the giant per-function switch tables
          // codegen emits) can mean two different things. The top-level
          // guest entry genuinely finishing is the next_address==0/lr==0
          // sentinel pattern (see run_execution()'s caller, which sets the
          // initial synthetic LR to 0 specifically to detect this) - that
          // case is handled below, unchanged. But a nonzero next_address
          // means a tail branch (bctr/bclr with LK=0, not bl) crossed a
          // compiled-function boundary into a shared helper - most commonly
          // a register-restore helper (see runtime_helpers.hpp's
          // restore_gpr_lr_v2 and friends) - whose own Return is really
          // "continue at my caller's real return address", exactly like
          // Branch/Fallthrough below, just arriving as FlowReason::Return
          // because the helper had no idea it was reached via a tail branch
          // from a *different* enclosing function rather than entered as
          // this thread's own top-level function. Treating every such
          // Return as terminal previously made any tail-branch-into-helper
          // pattern that crosses a compiled-function boundary look like a
          // broken/early return, even though the guest call chain was
          // genuinely still live and simply needs to keep dispatching at
          // result.next_address.
          if (result.next_address == 0u && state.lr == 0u) break;
          [[fallthrough]];
        case cpu::FlowReason::Branch:
        case cpu::FlowReason::Fallthrough: {
          if (result.next_address == 0u) {
            fail_execution("Guest execution returned " +
                               std::string(cpu::flow_reason_name(result.reason)) +
                               " with a zero next address",
                           0xC000001Du);
            break;
          }
          auto* next_fn =
              context.lookup_compiled(result.next_address,
                                      cpu::CompiledLookupKind::Branch);
          if (!next_fn) {
            const auto fallback = context.try_dynamic_fallback(
                result.next_address, cpu::CompiledLookupKind::Branch);
            if (!fallback.handled) {
              std::ostringstream diagnostic;
              diagnostic << "Guest " << cpu::flow_reason_name(result.reason)
                         << " target 0x" << std::hex << std::uppercase
                         << result.next_address
                         << " is not compiled or fallback-executable";
              fail_execution(diagnostic.str(), 0xC000001Du);
              break;
            }
            ++dispatch_count;
            result = fallback.result;
          } else {
            ++dispatch_count;
            result = next_fn(context);
          }
          if (trace_dispatches && config_.enable_logging) {
            std::scoped_lock console_log_lock(console_log_mutex());
            std::cout << "[XenonSession] Guest dispatch returned: reason="
                      << cpu::flow_reason_name(result.reason)
                      << " next=0x" << std::hex << std::uppercase
                      << result.next_address << " detail=0x" << result.detail
                      << " cia=0x" << state.cia
                      << " nia=0x" << state.nia
                      << " lr=0x" << state.lr
                      << " ctr=0x" << state.ctr
                      << " r1=0x" << state.gpr[1]
                      << " r3=0x" << state.gpr[3];
            if (result.next_address == 0x82379AC4u) {
              cpu::MemoryAccessContext::PhysicalResolution _probe{};
              const auto obj = static_cast<std::uint32_t>(state.gpr[27]);
              std::cout << " r27(obj)=0x" << obj;
              if (context.memory_access.resolve_physical_ram(
                      static_cast<cpu::GuestAddress>(obj) + 8u, 4, false, 4, _probe)) {
                std::cout << " obj+8=0x"
                          << context.memory_access.read32_be(
                                 static_cast<cpu::GuestAddress>(obj) + 8u);
              } else {
                std::cout << " obj+8=<unresolvable>";
              }
              const auto gbase = static_cast<std::uint32_t>(state.gpr[29]);
              std::cout << " r29(gbase)=0x" << gbase;
              if (context.memory_access.resolve_physical_ram(
                      static_cast<cpu::GuestAddress>(gbase) + 0x3B38u, 4, false, 4, _probe)) {
                std::cout << " gbase+3B38=0x"
                          << context.memory_access.read32_be(
                                 static_cast<cpu::GuestAddress>(gbase) + 0x3B38u);
              } else {
                std::cout << " gbase+3B38=<unresolvable>";
              }
            }
            std::cout << std::dec << std::endl;
          }
          continue;
        }
        case cpu::FlowReason::Halt:
          break;
        case cpu::FlowReason::Trap:
          break;
        case cpu::FlowReason::Syscall:
          fail_execution("Unhandled guest syscall escaped RuntimeServices::syscall "
                             "(level " + std::to_string(result.detail) + ")",
                         0xC000001Du);
          break;
        case cpu::FlowReason::LongJump:
          {
            std::ostringstream diagnostic;
            diagnostic << "Guest LongJump escaped the owning compiled function "
                       << "(target 0x" << std::hex << std::uppercase
                       << result.next_address << ')';
            fail_execution(diagnostic.str(), 0xC000001Du);
          }
          break;
      }
      break;
    }
    // Distinguishes "the loop genuinely exhausted its dispatch budget" from
    // "a terminal case (Halt/Trap/a real Return-with-null-sentinel/...)
    // explicitly broke out of the switch" by checking the real counter
    // rather than inferring it from result.reason - Return can now reach
    // this point via either path (see the Return case above), so
    // result.reason alone can no longer tell them apart the way it could
    // when only Branch/Fallthrough ever re-entered the loop.
    if (!crashed && !stop_requested_.load() && !(thread && thread->is_terminated()) &&
        dispatch_count >= kMaxTopLevelDispatches) {
      // A guest that spins without ever leaving compiled/fallback code is almost
      // always retrying a kernel call that keeps failing, so name the last calls.
      std::ostringstream spin;
      spin << "Guest execution exceeded the top-level dispatch limit [cia=0x" << std::hex
           << std::uppercase << state.cia << " lr=0x" << state.lr << std::dec
           << " last_exports=[";
      const auto recent =
          export_trace_.recent_for_thread(thread ? thread->thread_id() : 0u, 12u);
      for (std::size_t i = 0; i < recent.size(); ++i) {
        const auto& trace = recent[i];
        if (i != 0u) spin << ' ';
        spin << trace.library_view() << '!';
        if (!trace.name_view().empty()) {
          spin << trace.name_view();
        } else {
          spin << trace.ordinal;
        }
        spin << "(ord=" << trace.ordinal << ",lr=0x" << std::hex << trace.lr << ",r3in=0x"
             << trace.arguments[0] << ",r4=0x" << trace.arguments[1] << ",r5=0x"
             << trace.arguments[2] << ",r6=0x" << trace.arguments[3] << ",ret=0x"
             << trace.result_r3 << std::dec << ")";
      }
      spin << "]]";
      fail_execution(spin.str(), 0xC000001Du);
    }
    if (!crashed && thread && thread->is_terminated()) {
      thread_terminated_mid_dispatch = true;
    }
  } catch (const memory::MemoryFault& fault) {
    // Real connection to the guest exception path (not a new subsystem):
    // Memory V2 already throws this on a genuine guest memory fault; route
    // it through kernel::ExceptionDispatcher, scoped to whichever thread
    // called dispatch_guest_thread() (the caller already registered itself
    // as current_thread() before invoking this). Also retain the guest
    // CPU/memory state that caused the first fault - "fault at 0" alone
    // cannot distinguish a null data dereference from an indirect call, bad
    // ABI state, or bad mapping.
    crashed = true;
    const auto& info = fault.info();
    const auto record = kernel::ExceptionDispatcher::fault_to_exception(info);
    crash_exit_code = static_cast<std::uint32_t>(record.code);
    // Phase 5 of the AC6 Runtime Readiness pass: scope this dispatch to the
    // actual faulting thread (0 if none, e.g. a test harness invoking
    // dispatch_guest_thread() directly) so a handler registered for one
    // guest thread is never consulted for a different thread's fault.
    static_cast<void>(exception_dispatcher_.dispatch_exception(
        record, thread ? thread->thread_id() : 0u));

    const auto access_name = [](memory::AccessKind access) {
      switch (access) {
        case memory::AccessKind::Read: return "read";
        case memory::AccessKind::Write: return "write";
        case memory::AccessKind::Execute: return "execute";
      }
      return "unknown";
    };
    const auto reason_name = [](memory::FaultReason reason) {
      switch (reason) {
        case memory::FaultReason::Unmapped: return "unmapped";
        case memory::FaultReason::Uncommitted: return "uncommitted";
        case memory::FaultReason::Protection: return "protection";
        case memory::FaultReason::OutOfRange: return "out-of-range";
        case memory::FaultReason::MmioWidth: return "mmio-width";
      }
      return "unknown";
    };
    const auto page_state_name = [](memory::PageState page_state) {
      switch (page_state) {
        case memory::PageState::Free: return "free";
        case memory::PageState::Reserved: return "reserved";
        case memory::PageState::Committed: return "committed";
      }
      return "unknown";
    };
    const auto protect_string = [](memory::Protect protect) {
      std::string protect_str;
      protect_str += memory::has(protect, memory::Protect::Read) ? 'R' : '-';
      protect_str += memory::has(protect, memory::Protect::Write) ? 'W' : '-';
      protect_str += memory::has(protect, memory::Protect::Execute) ? 'X' : '-';
      if (memory::has(protect, memory::Protect::NoCache)) protect_str += "|NC";
      if (memory::has(protect, memory::Protect::WriteCombine)) protect_str += "|WC";
      return protect_str;
    };

    std::ostringstream diagnostic;
    diagnostic << "Guest memory fault: " << fault.what()
               << " [cia=0x" << std::hex << std::uppercase << state.cia
               << " nia=0x" << state.nia
               << " lr=0x" << state.lr
               << " ctr=0x" << state.ctr
               << " request=0x" << info.request_address
               << " fault=0x" << info.fault_address
               << std::dec << " width=" << info.width
               << " access=" << access_name(info.access)
               << " reason=" << reason_name(info.reason)
               << " page_state=" << page_state_name(info.page_state)
               << " mapped=" << (info.mapped ? "yes" : "no")
               << " committed=" << (info.committed ? "yes" : "no")
               << " current_protect=" << protect_string(info.current_protect)
               << " allocation_protect=" << protect_string(info.allocation_protect)
               << " page_size=0x" << std::hex << info.page_size
               << " r1=0x" << state.gpr[1]
               << " r2=0x" << state.gpr[2]
               << " r3=0x" << state.gpr[3]
               << " r4=0x" << state.gpr[4]
               << " r5=0x" << state.gpr[5]
               << " r6=0x" << state.gpr[6]
               << " r7=0x" << state.gpr[7]
               << " r8=0x" << state.gpr[8]
               << " r9=0x" << state.gpr[9]
               << " r10=0x" << state.gpr[10]
               << " r13=0x" << state.gpr[13] << ']';
    // The XEX loader already parsed the title's runtime-function directory.
    // Report coverage from that source of truth; coverage alone is not
    // evidence that a language/exception handler exists.
    const auto append_function_metadata = [&](std::string_view label,
                                              cpu::GuestAddress address) {
      diagnostic << ' ' << label << "_function=";
      if (!loaded_xex_) {
        diagnostic << "no-module";
        return;
      }
      const auto& functions = loaded_xex_->image.function_metadata;
      const auto it = std::find_if(functions.begin(), functions.end(),
                                   [address](const xbox::XexFunctionMetadata& fn) {
                                     return address >= fn.begin && address < fn.end;
                                   });
      if (it == functions.end()) {
        diagnostic << "none";
        return;
      }
      diagnostic << "{module="
                 << (loaded_xex_->image.original_pe_name.empty()
                         ? "title"
                         : loaded_xex_->image.original_pe_name)
                 << ",begin=0x" << std::hex << it->begin
                 << ",end=0x" << it->end
                 << ",unwind=0x" << it->unwind_data
                 << std::dec << ",valid=" << (it->valid ? "yes" : "no") << '}';
    };
    append_function_metadata("cia", state.cia);
    append_function_metadata("lr", static_cast<cpu::GuestAddress>(state.lr));
    // Diagnostic-only guest stack walk (read-only, never changes execution):
    // standard PPC back-chain convention - [r1] = caller's saved r1, [r1+8] =
    // caller's saved LR (the return address into this frame). Best-effort:
    // a corrupt/absent frame chain stops the walk early rather than faulting
    // again while already handling a fault.
    {
      diagnostic << " stack=[";
      auto frame = static_cast<memory::GuestAddress>(state.gpr[1]);
      bool first = true;
      for (int depth = 0; depth < 16 && frame != 0u; ++depth) {
        std::uint32_t saved_lr = 0u;
        std::uint32_t next_frame = 0u;
        try {
          saved_lr = memory_->read32_be(frame + 8u);
          next_frame = memory_->read32_be(frame);
        } catch (const memory::MemoryFault&) {
          break;
        }
        if (!first) diagnostic << ',';
        first = false;
        diagnostic << "0x" << std::hex << saved_lr;
        append_function_metadata("stack", saved_lr);
        if (next_frame <= frame) break;  // frame chain must strictly ascend
        frame = static_cast<memory::GuestAddress>(next_frame);
      }
      diagnostic << ']' << std::dec;
    }
    const auto append_export_records = [&](std::string_view label,
                                           const std::vector<ExportTraceRecord>& records) {
      diagnostic << ' ' << label << "=[";
      bool first = true;
      for (const auto& trace : records) {
        if (!first) diagnostic << ';';
        first = false;
        diagnostic << '#' << trace.sequence << " tid=" << trace.thread_id << ' '
                   << trace.library_view() << '!';
        if (!trace.name_view().empty()) {
          diagnostic << trace.name_view();
        } else {
          diagnostic << trace.ordinal;
        }
        diagnostic << "(ord=" << trace.ordinal << ",cia=0x" << std::hex
                   << trace.call_address << ",lr=0x" << trace.lr << ",ctr=0x"
                   << trace.ctr << ",args=";
        for (const auto argument : trace.arguments) diagnostic << "0x" << argument << ',';
        diagnostic << "r3=0x" << trace.result_r3 << std::dec
                   << ",found=" << trace.handler_found
                   << ",handled=" << trace.handled
                   << ",success=" << trace.success << ')';
      }
      diagnostic << ']';
    };
    if (export_trace_.enabled()) {
      append_export_records(
          "export_trace_thread",
          export_trace_.recent_for_thread(thread ? thread->thread_id() : 0u, 64u));
      append_export_records("export_trace_global", export_trace_.recent_global(64u));
    }
    crash_message = diagnostic.str();
  } catch (const std::exception& ex) {
    crashed = true;
    crash_message = std::string("Unhandled exception during guest execution: ") + ex.what();
  } catch (...) {
    crashed = true;
    crash_message = "Unhandled unknown exception during guest execution";
  }

  outcome.crashed = crashed;
  outcome.crash_message = std::move(crash_message);
  outcome.crash_exit_code = crash_exit_code;
  outcome.final_result = result;
  outcome.stop_requested = stop_requested_.load();
  outcome.thread_terminated = thread_terminated_mid_dispatch;

  // Exception dispatch for a genuine guest Trap is generic fault
  // interpretation (like the MemoryFault catch above), not session-lifecycle
  // bookkeeping, so it happens here rather than in each caller - every
  // guest thread that traps gets this, not just the main thread.
  if (!crashed && result.reason == cpu::FlowReason::Trap) {
    static_cast<void>(exception_dispatcher_.dispatch_exception(
        kernel::ExceptionRecord{kernel::ExceptionCode::IllegalInstruction, 0,
                                static_cast<std::uint32_t>(state.cia), {}},
        thread ? thread->thread_id() : 0u));
  }

  return outcome;
}

std::uint32_t XenonSession::run_execution() {
  // Establishes this host thread as the active guest thread for anything
  // that resolves "current thread" via kernel::ThreadManager (thread_local),
  // so kernel/exception context below - and any future kernel export that
  // asks "who am I" - is scoped to the real KernelThread, not inferred.
  if (kernel_process_ && main_thread_) {
    kernel_process_->thread_manager().set_current_thread(main_thread_);
  }
  reach_boot_checkpoint(BootCheckpoint::EntryStarted);

  execution_active_.store(true);
  const auto outcome =
      dispatch_guest_thread(*main_cpu_state_, loaded_xex_->image.entry_point, main_thread_);
  execution_active_.store(false);

  // See run_created_guest_thread()'s matching cleanup: main_thread_ is
  // tracked by ThreadManager the same way a created thread is, and must
  // stop being counted as live once its dispatch has genuinely ended,
  // regardless of which outcome branch below is taken. This runs AS
  // main_thread_'s own entry_() (see start(), which spawns it exactly like
  // a created guest thread), so directly calling
  // remove_thread(main_thread_->thread_id()) here would be the same
  // self-destruction hazard described at run_created_guest_thread()'s
  // reap_finished_threads() call below - it could drop the map's last
  // *other* reference and destroy main_thread_ while thread_main() is
  // still executing on top of it. reap_finished_threads() is safe here
  // because it can never remove the calling thread itself; main_thread_
  // gets cleaned up by the next opportunistic reap elsewhere (another
  // thread finishing, a new create_thread(), a capability_report() call),
  // or unconditionally by ThreadManager::shutdown() at teardown.
  if (kernel_process_) {
    kernel_process_->thread_manager().reap_finished_threads();
  }

  if (outcome.crashed) {
    set_error(outcome.crash_message);
    return outcome.crash_exit_code;
  }
  if (outcome.thread_terminated) {
    // terminate() was called on main_thread_ (possibly mid-dispatch, via the
    // preemptive safepoint) - its own committed exit code is authoritative,
    // not any in-flight register state.
    set_state(SessionState::Stopped, "Game execution ended (thread terminated)");
    return main_thread_ ? main_thread_->exit_code() : 0;
  }
  if (outcome.stop_requested) {
    set_state(SessionState::Stopped, "Game execution ended (stop requested)");
    return 0;
  }
  if (outcome.final_result.reason == cpu::FlowReason::Trap) {
    set_error("Game execution trapped (code " + std::to_string(outcome.final_result.detail) + ")");
    return outcome.final_result.detail;
  }
  set_state(SessionState::Stopped, "Game execution completed");
  return 0;
}

std::uint32_t XenonSession::run_created_guest_thread(
    std::shared_ptr<kernel::KernelThread> thread, cpu::GuestAddress start_address,
    std::uint64_t start_context, memory::GuestAddress stack_base,
    std::uint32_t stack_size, GuestThreadTlsContext tls) {
  {
    static std::atomic<int> _thread_start_diag_count{0};
    const int _n = _thread_start_diag_count.fetch_add(1) + 1;
    if (_n <= 100) {
      if (FILE* _d = std::fopen("thread_start_diag.log", "a")) {
#ifdef _WIN32
        const unsigned long _os_tid = GetCurrentThreadId();
#else
        const unsigned long _os_tid = 0;
#endif
        std::fprintf(_d, "thread start #%d: id=%u start_address=0x%08llX start_context=0x%08llX os_tid=%lu\n",
                     _n, thread ? thread->thread_id() : 0u,
                     (unsigned long long)start_address, (unsigned long long)start_context, _os_tid);
        std::fclose(_d);
      }
    }
  }
  if (kernel_process_ && thread) {
    kernel_process_->thread_manager().set_current_thread(thread);
  }
  {
    std::ostringstream tid_oss;
    tid_oss << std::this_thread::get_id();
    auto readback = kernel_process_ ? kernel_process_->thread_manager().current_thread() : nullptr;
    if (FILE* _d = std::fopen("set_current_thread_diag.log", "a")) {
      std::fprintf(_d,
                   "run_created_guest_thread: set thread_id=%u os_thread=%s readback_id=%u "
                   "readback_ptr=%p set_ptr=%p\n",
                   thread ? thread->thread_id() : 0u, tid_oss.str().c_str(),
                   readback ? readback->thread_id() : 0xFFFFFFFFu, (void*)readback.get(),
                   (void*)thread.get());
      std::fclose(_d);
    }
  }

  cpu::CpuState state{};
  state.cia = start_address;
  // PowerPC stacks grow downward; r1 starts at the top of the allocation,
  // less a small back-chain reserve, matching create_guest_process()'s main
  // thread stack setup and start_audio_guest_thread()'s callback stack.
  state.gpr[1] = static_cast<std::uint64_t>(stack_base) + stack_size - 64u;
  state.gpr[3] = start_context;
  state.gpr[13] = tls.kpcr_address;

  const auto outcome = dispatch_guest_thread(state, start_address, thread);
  {
    if (FILE* _d = std::fopen("dispatch_outcome_diag.log", "a")) {
      std::fprintf(_d,
                   "dispatch_guest_thread returned: thread_id=%u start_address=0x%08llX "
                   "crashed=%d crash_message=\"%s\" thread_terminated=%d stop_requested=%d "
                   "final_reason=%s final_next=0x%08llX final_detail=0x%X\n",
                   thread ? thread->thread_id() : 0u, (unsigned long long)start_address,
                   outcome.crashed ? 1 : 0, outcome.crash_message.c_str(),
                   outcome.thread_terminated ? 1 : 0, outcome.stop_requested ? 1 : 0,
                   std::string(cpu::flow_reason_name(outcome.final_result.reason)).c_str(),
                   (unsigned long long)outcome.final_result.next_address,
                   outcome.final_result.detail);
      std::fclose(_d);
    }
  }

  std::uint32_t exit_code = 0;
  if (outcome.crashed) {
    exit_code = outcome.crash_exit_code;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Created guest thread "
                << (thread ? thread->thread_id() : 0u)
                << " crashed: " << outcome.crash_message << std::endl;
    }
  } else if (outcome.thread_terminated) {
    // terminate() was called on this thread (possibly mid-dispatch, via the
    // preemptive safepoint) - its own committed exit code is authoritative,
    // not any in-flight register state.
    exit_code = thread ? thread->exit_code() : 0;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Created guest thread "
                << (thread ? thread->thread_id() : 0u)
                << " terminated mid-dispatch, exit_code=" << exit_code << std::endl;
    }
  } else if (outcome.stop_requested) {
    // Session-wide stop was requested mid-dispatch; exit_code stays 0,
    // matching run_execution()'s own "stop requested" -> clean 0 convention.
  } else if (outcome.final_result.reason == cpu::FlowReason::Trap) {
    // A Trap is a distinct, non-"crashed" outcome in GuestDispatchOutcome
    // (see dispatch_guest_thread()) - it still must not be reported as a
    // successful exit code, exactly matching how run_execution() treats a
    // main-thread Trap (returns final_result.detail, not gpr[3]).
    exit_code = outcome.final_result.detail;
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Created guest thread "
                << (thread ? thread->thread_id() : 0u)
                << " trapped (code " << exit_code << ')' << std::endl;
    }
  } else {
    // Natural return: PPC ABI convention - r3 holds the function's return
    // value, matching real ExCreateThread/ExTerminateThread semantics (a
    // thread function returning is an implicit ExTerminateThread(returnValue)).
    exit_code = static_cast<std::uint32_t>(state.gpr[3]);
  }

  // Release this thread's own guest stack/TLS now that nothing will execute
  // on them again. Safe to do from within the thread's own host-side
  // ThreadEntry body: this code runs on host_thread_'s native OS stack,
  // entirely separate from the guest-memory stack/TLS being freed here.
  if (memory_) {
    release_guest_thread_tls_context(*memory_, tls);
    static_cast<void>(memory_->release(stack_base));
  }

  // This thread id will never execute guest code again (KernelThread cannot
  // restart once terminated, and ids are never reused - see thread.cpp's
  // monotonic g_next_thread_id). A per-thread exception handler chain
  // registered for it must not linger forever in exception_dispatcher_,
  // and must never be silently inherited by a different, later thread.
  exception_dispatcher_.clear_thread_handlers(thread ? thread->thread_id() : 0u);

  // NOT calling ThreadManager::remove_thread(thread->thread_id()) here,
  // deliberately - a real, confirmed use-after-free bug used to live at
  // this exact spot. This function runs AS entry_(), called by
  // KernelThread::thread_main() (see thread.cpp) as `result = entry_();`;
  // thread_main() then keeps running AFTER entry_() returns, touching
  // mutex_/state_/completion_promise_ on `this`. This function's own
  // `thread` parameter is a BY-VALUE std::shared_ptr<KernelThread> copy -
  // the guest HandleTable's reference is the only other one once removed
  // from the map (a game may already have closed the handle before the
  // thread naturally finishes). Removing the map's reference here could
  // therefore drop the very last reference and run ~KernelThread()
  // synchronously, right before `thread` (this function's own copy) is
  // destroyed as this function returns - destroying the KernelThread
  // object while thread_main() is still executing ON it, one call frame
  // up. thread_main()'s subsequent member accesses then hit freed memory:
  // this was reachable in practice (any short-lived guest thread whose
  // handle closes before it returns - not merely a hypothetical "handle
  // closed early" edge case) and manifested as intermittent heap
  // corruption/access violations/hangs depending on heap layout - never a
  // clean, reliable crash, which is why this took so long to isolate.
  //
  // ThreadManager::reap_finished_threads() is the fix: it can never remove
  // the calling thread itself (see KernelThread::is_current_host_thread()),
  // only OTHER threads that have already finished, so it is safe to call
  // from right here, on this thread's own stack, one statement before this
  // function - and thread_main() above it - returns.
  if (kernel_process_) {
    kernel_process_->thread_manager().reap_finished_threads();
  }

  return exit_code;
}

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

  if (start_address == 0x821EEDE0u) {
    if (FILE* _d = std::fopen("ex_create_thread_caller_diag.log", "a")) {
      std::fprintf(_d,
                   "ExCreateThread(821EEDE0): caller_lr=0x%08llX start_context=0x%08llX "
                   "creation_flags=0x%08X\n",
                   (unsigned long long)context.cpu.lr, (unsigned long long)start_context,
                   creation_flags);
      std::fclose(_d);
    }
  }

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
    if (FILE* _d = std::fopen("thread_create_flags_diag.log", "a")) {
      std::fprintf(_d, "ExCreateThread: thread_id=%u start_address=0x%08llX creation_flags=0x%08X create_suspended=%d\n",
                   thread->thread_id(), (unsigned long long)start_address, _diag_creation_flags,
                   params.create_suspended ? 1 : 0);
      std::fclose(_d);
    }
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

#if defined(XENON_HAS_AUDIO)
bool XenonSession::start_audio_guest_thread() {
  if (!audio_ || !kernel_process_ || !memory_ || !loaded_xex_) return false;
  if (!audio_callback_stack_base_ || !audio_callback_stack_size_) {
    set_error("Audio callback stack was not allocated");
    return false;
  }

  // A dedicated KPCR + static-TLS block for the audio callback thread. Real
  // Xbox 360 render-driver callbacks execute on their own kernel thread with
  // their own TLS instance, never the game's main thread's - reusing the same
  // image.tls template that seeded main_thread_tls_ but allocating an
  // independent block guarantees the two threads observe distinct TLS state
  // (see tests/core/session_tests.cpp's TLS-independence test).
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, loaded_xex_->image.tls,
                                      audio_callback_stack_base_,
                                      audio_callback_stack_size_,
                                      audio_thread_tls_, &tls_error)) {
    set_error("Failed to set up audio callback thread TLS: " + tls_error);
    return false;
  }

  kernel::ThreadCreationParams thread_params{};
  thread_params.stack_size = audio_callback_stack_size_;
  thread_params.name = "AudioCallbackThread";
  audio_thread_ = kernel_process_->thread_manager().create_thread(
      [this]() -> std::uint32_t { return run_audio_callback_thread(); },
      thread_params);
  if (!audio_thread_) {
    set_error("Failed to create audio callback KernelThread");
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
    audio_thread_tls_ = {};
    return false;
  }
  write_guest_thread_id(*memory_, audio_thread_tls_, audio_thread_->thread_id());
  audio_thread_->set_guest_kthread_address(audio_thread_tls_.kthread_address);

  // Mark the pump active and wire the invoker before starting the thread, so
  // there is no window where the thread is running but has nothing to do (or
  // races begin_guest_callback_pump() against the thread's own startup).
  audio_->begin_guest_callback_pump();
  audio_->set_guest_callback_invoker(
      [this](cpu::GuestAddress callback, cpu::GuestAddress argument) {
        return invoke_audio_callback(callback, argument);
      });

  if (!audio_thread_->start()) {
    set_error("Failed to start audio callback KernelThread");
    audio_->stop_guest_callback_pump();
    audio_thread_.reset();
    release_guest_thread_tls_context(*memory_, audio_thread_tls_);
    audio_thread_tls_ = {};
    return false;
  }
  return true;
}

std::uint32_t XenonSession::run_audio_callback_thread() {
  // Registers this host thread as audio_thread_ for anything that resolves
  // "current thread" via kernel::ThreadManager (thread_local) - the same
  // mechanism run_execution() uses for main_thread_ - so a guest xboxkrnl
  // export invoked from inside a guest audio callback sees the audio
  // callback's own thread identity, not the main thread's or none at all.
  if (kernel_process_ && audio_thread_) {
    kernel_process_->thread_manager().set_current_thread(audio_thread_);
  }
  if (audio_) {
    audio_->run_guest_callback_pump_body();
  }
  return 0;
}

bool XenonSession::invoke_audio_callback(cpu::GuestAddress callback,
                                         cpu::GuestAddress argument) {
  if (!callback || !memory_ || !compiled_registry_binder_ || !kernel_process_ ||
      !audio_callback_stack_base_ || !audio_callback_stack_size_) {
    return false;
  }

  cpu::CpuState state{};
  state.cia = callback;
  state.gpr[1] = static_cast<std::uint64_t>(audio_callback_stack_base_) +
                 audio_callback_stack_size_ - 64u;
  state.gpr[3] = argument;
  // The real fix this pass makes: gpr[13] points at this thread's OWN KPCR
  // (allocated once in start_audio_guest_thread(), reused across every call -
  // TLS is per-thread state that outlives a single callback invocation, not
  // per-call scratch), instead of an isolated bare CpuState with no thread
  // identity at all. Compiled guest code that accesses __declspec(thread)
  // TLS or calls a real xboxkrnl export now does so through the same
  // r13-relative KPCR mechanism the main thread uses (see
  // guest_thread_context.hpp) and against the audio callback's own
  // ThreadManager::current_thread(), set once in run_audio_callback_thread().
  state.gpr[13] = audio_thread_tls_.kpcr_address;

  {
    static std::atomic<int> _n{0};
    const int _en = _n.fetch_add(1) + 1;
    if (_en <= 20 || (_en % 500) == 0) {
      if (FILE* _d = std::fopen("audio_callback_diag.log", "a")) {
        std::fprintf(_d, "invoke_audio_callback #%d ENTER: callback=0x%08X argument=0x%08X\n",
                     _en, (unsigned)callback, (unsigned)argument);
        std::fclose(_d);
      }
    }
    const bool _ok = run_guest_callback(state, callback, audio_thread_);
    if (_en <= 20 || (_en % 500) == 0) {
      if (FILE* _d = std::fopen("audio_callback_diag.log", "a")) {
        std::fprintf(_d, "invoke_audio_callback #%d RETURNED: ok=%d\n", _en, (int)_ok);
        std::fclose(_d);
      }
    }
    return _ok;
  }
}
#endif

bool XenonSession::start_gpu_pump_thread() {
  if (!gpu_ || !graphics_system_ || !kernel_process_ || !memory_ || !loaded_xex_) {
    return false;
  }
  if (!gpu_pump_callback_stack_base_ || !gpu_pump_callback_stack_size_) {
    set_error("GPU pump callback stack was not allocated");
    return false;
  }

  // A dedicated KPCR + static-TLS block for the GPU pump thread, independent
  // of main_thread_tls_/audio_thread_tls_ - real Xbox 360 vsync interrupt
  // dispatch runs on its own kernel context, never the game's main thread's.
  std::string tls_error;
  if (!setup_guest_thread_tls_context(*memory_, loaded_xex_->image.tls,
                                      gpu_pump_callback_stack_base_,
                                      gpu_pump_callback_stack_size_,
                                      gpu_pump_thread_tls_, &tls_error)) {
    set_error("Failed to set up GPU pump thread TLS: " + tls_error);
    return false;
  }

  kernel::ThreadCreationParams thread_params{};
  thread_params.stack_size = gpu_pump_callback_stack_size_;
  thread_params.name = "GpuPumpThread";
  gpu_pump_thread_ = kernel_process_->thread_manager().create_thread(
      [this]() -> std::uint32_t { return run_gpu_pump_thread(); }, thread_params);
  if (!gpu_pump_thread_) {
    set_error("Failed to create GPU pump KernelThread");
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
    gpu_pump_thread_tls_ = {};
    return false;
  }
  write_guest_thread_id(*memory_, gpu_pump_thread_tls_, gpu_pump_thread_->thread_id());
  gpu_pump_thread_->set_guest_kthread_address(gpu_pump_thread_tls_.kthread_address);
  if (FILE* _d = std::fopen("thread_identity_diag.log", "a")) {
    std::fprintf(_d, "gpu_pump_thread_ assigned thread_id=%u\n", gpu_pump_thread_->thread_id());
    std::fclose(_d);
  }
  if (main_thread_) {
    if (FILE* _d = std::fopen("thread_identity_diag.log", "a")) {
      std::fprintf(_d, "main_thread_ thread_id=%u\n", main_thread_->thread_id());
      std::fclose(_d);
    }
  }
  if (audio_thread_) {
    if (FILE* _d = std::fopen("thread_identity_diag.log", "a")) {
      std::fprintf(_d, "audio_thread_ thread_id=%u\n", audio_thread_->thread_id());
      std::fclose(_d);
    }
  }

  // Mark the pump active before starting the thread, so there is no window
  // where the thread is running but gpu_pump_running_ has not been observed
  // true yet.
  gpu_pump_running_.store(true);

  if (!gpu_pump_thread_->start()) {
    set_error("Failed to start GPU pump KernelThread");
    gpu_pump_running_.store(false);
    gpu_pump_thread_.reset();
    release_guest_thread_tls_context(*memory_, gpu_pump_thread_tls_);
    gpu_pump_thread_tls_ = {};
    return false;
  }
  return true;
}

void XenonSession::stop_gpu_pump_thread() noexcept {
  gpu_pump_running_.store(false);
  if (gpu_pump_thread_) {
    static_cast<void>(gpu_pump_thread_->join());
    gpu_pump_thread_.reset();
  }
}

std::uint32_t XenonSession::run_gpu_pump_thread() {
  // Registers this host thread as gpu_pump_thread_ for anything that
  // resolves "current thread" via kernel::ThreadManager (thread_local) - the
  // same mechanism run_execution()/run_audio_callback_thread() use - so a
  // guest xboxkrnl export invoked from inside the vsync interrupt callback
  // sees the pump thread's own identity.
  if (kernel_process_ && gpu_pump_thread_) {
    kernel_process_->thread_manager().set_current_thread(gpu_pump_thread_);
  }

  using Clock = std::chrono::steady_clock;
  // Short poll interval so ring-buffer drain latency stays low; a separate
  // accumulator (next_vsync) triggers vsync-rate work (present + interrupt)
  // at ~60Hz regardless of how often this tighter loop actually wakes.
  constexpr auto kPollInterval = std::chrono::milliseconds(3);
  constexpr auto kVsyncInterval = std::chrono::nanoseconds(16'666'667);  // ~60Hz
  auto next_vsync = Clock::now() + kVsyncInterval;

  while (gpu_pump_running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(kPollInterval);
    if (!gpu_pump_running_.load(std::memory_order_relaxed)) break;
    if (!graphics_system_ || !gpu_ || !kernel_process_) continue;

    const auto _tick_phase_start = Clock::now();
    const auto ring = kernel_process_->gpu_ring_buffer();
    if (ring.configured() && ring.write_index != ring.read_index) {
      try {
        const auto new_read_index = graphics_system_->submit_ring(
            ring.base_address, ring.capacity_dwords, ring.read_index,
            ring.write_index);
        kernel_process_->set_gpu_ring_buffer_read_index(new_read_index);
        // Real Xenos read-pointer write-back hardware semantics
        // (VdEnableRingBufferRPtrWriteBack): mirror the freshly-consumed
        // read index into guest memory so the guest can poll its own copy.
        // VdEnableRingBufferRPtrWriteBack hands the kernel a PHYSICAL address (as
        // does VdInitializeRingBuffer), so write it as physical memory: treating
        // it as a guest virtual address faulted on the free page every drain and
        // the title never saw the read pointer advance.
        if (ring.rptr_writeback_address != 0 && memory_) {
          const std::array<std::byte, 4> big_endian{
              static_cast<std::byte>(new_read_index >> 24), static_cast<std::byte>(new_read_index >> 16),
              static_cast<std::byte>(new_read_index >> 8), static_cast<std::byte>(new_read_index)};
          if (!memory_->write_physical(ring.rptr_writeback_address, big_endian) &&
              config_.enable_logging) {
            std::scoped_lock console_log_lock(console_log_mutex());
            std::cout << "[XenonSession] GPU pump: read-pointer writeback to physical 0x" << std::hex
                      << ring.rptr_writeback_address << std::dec << " has no RAM backing" << std::endl;
          }
        }
      } catch (const std::exception& ex) {
        // Malformed/guest-corrupted ring content must not kill the pump
        // thread - drop this drain attempt (read_index stays where it was;
        // the next tick will retry from there) rather than crash or silently
        // pretend nothing happened.
        if (config_.enable_logging) {
          std::scoped_lock console_log_lock(console_log_mutex());
          std::cout << "[XenonSession] GPU pump: ring buffer decode failed: "
                    << ex.what() << std::endl;
        }
      }
    }

    const auto _after_submit_ring = Clock::now();

    // Drain whatever graphics IR is now pending every tick, even when
    // submit_ring() above produced nothing new this time - IR decoded by a
    // previous tick's partial submission can still be waiting to be played
    // into the backend (see GraphicsSystem::execute_ir()).
    try {
      graphics_system_->execute_ir(*gpu_);
    } catch (const std::exception& ex) {
      if (config_.enable_logging) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] GPU pump: IR execution failed: " << ex.what()
                  << std::endl;
      }
    }

    {
      const auto _after_execute_ir = Clock::now();
      const auto _submit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   _after_submit_ring - _tick_phase_start)
                                   .count();
      const auto _ir_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               _after_execute_ir - _after_submit_ring)
                               .count();
      if (_submit_ms >= 20 || _ir_ms >= 20) {
        static std::atomic<int> _tick_phase_diag_count{0};
        if (_tick_phase_diag_count.fetch_add(1) < 40) {
          if (FILE* _d = std::fopen("tick_phase_diag.log", "a")) {
            std::fprintf(_d, "slow tick phase: submit_ring=%lldms execute_ir=%lldms\n",
                         (long long)_submit_ms, (long long)_ir_ms);
            std::fclose(_d);
          }
        }
      }
    }

    const auto now = Clock::now();
    if (now < next_vsync) continue;
    // Catch up rather than let a slow tick cause a burst of back-to-back
    // vsyncs once it finally returns.
    do {
      next_vsync += kVsyncInterval;
    } while (next_vsync <= now);

    const auto front_buffer = kernel_process_->gpu_front_buffer();
    if (front_buffer.base_address != 0) {
      gpu::TextureDescriptor texture{};
      texture.base_address = front_buffer.base_address;
      texture.width = front_buffer.width;
      texture.height = front_buffer.height;
      texture.pitch = front_buffer.pitch;
      texture.format = front_buffer.format;
      gpu::PresentationFrame frame{};
      frame.texture = texture;
      frame.visible_width = front_buffer.width;
      frame.visible_height = front_buffer.height;
      const auto _present_start = Clock::now();
      static_cast<void>(graphics_system_->present(*gpu_, frame));
      const auto _present_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - _present_start)
                                    .count();
      if (_present_ms >= 20) {
        static std::atomic<int> _present_diag_count{0};
        if (_present_diag_count.fetch_add(1) < 40) {
          if (FILE* _d = std::fopen("tick_phase_diag.log", "a")) {
            std::fprintf(_d, "slow present: %lldms\n", (long long)_present_ms);
            std::fclose(_d);
          }
        }
      }
    }

    const auto interrupt = kernel_process_->gpu_interrupt_callback();
    {
      static std::atomic<int> _vsync_diag_count{0};
      const int _n = _vsync_diag_count.fetch_add(1) + 1;
      if (_n <= 5 || (_n % 300) == 0) {
        if (FILE* _d = std::fopen("vsync_tick_diag.log", "a")) {
          std::fprintf(_d, "vsync tick #%d: interrupt_callback_address=0x%08X front_buffer_base=0x%08X\n",
                       _n, interrupt.callback_address, kernel_process_->gpu_front_buffer().base_address);
          std::fclose(_d);
        }
      }
    }

    // Event-dispatcher probe (0x823AD848 family): a global pointer at
    // 0x82916E4C leads to a per-subsystem struct whose +0x12C field looked,
    // from static reading, like a pending-event bitmask and +0x130 like a
    // tick counter guarding which handler branch runs. Sampled here (host
    // side, every vsync) rather than inside the generated shard itself,
    // since that shard's custom incremental-build cache was not picking up
    // edits reliably. Logged on change only, uncapped, to catch a rare
    // transition without flooding when the value is static.
    {
      static std::atomic<std::uint32_t> _last_bitmask{0xFFFFFFFFu};
      static std::atomic<std::uint32_t> _last_counter{0xFFFFFFFFu};
      try {
        const auto ctx_ptr = memory_->read32_be(static_cast<cpu::GuestAddress>(0x82916E4Cu));
        if (ctx_ptr != 0u) {
          const auto bitmask = memory_->read32_be(static_cast<cpu::GuestAddress>(ctx_ptr + 0x12Cu));
          const auto counter = memory_->read32_be(static_cast<cpu::GuestAddress>(ctx_ptr + 0x130u));
          if (bitmask != _last_bitmask.load() || counter != _last_counter.load()) {
            _last_bitmask.store(bitmask);
            _last_counter.store(counter);
            if (FILE* _d = std::fopen("event_dispatch_823AD848_diag.log", "a")) {
              std::fprintf(_d, "823AD848 probe CHANGED: ctx_ptr=0x%08X bitmask=0x%08X counter=0x%08X\n",
                           ctx_ptr, bitmask, counter);
              std::fclose(_d);
            }
          }
        }
      } catch (const std::exception&) {
        // Guest memory not mapped yet (early boot) - ignore, next tick retries.
      }
    }

    // One-time read of the guest C-string at 0x82067EC8, the label argument
    // the subsystem-registration call (xenon_fn_821E4AD0 -> 0x821DD028)
    // passes alongside the object that owns the two stuck handshake events -
    // to identify semantically what this subsystem actually is.
    {
      static std::atomic<bool> _label_dumped{false};
      if (!_label_dumped.load()) {
        try {
          std::string label;
          for (std::uint32_t i = 0; i < 64u; ++i) {
            const auto c = memory_->read8(static_cast<cpu::GuestAddress>(0x82067EC8u + i));
            if (c == 0) break;
            label.push_back(static_cast<char>(c));
          }
          if (!label.empty()) {
            _label_dumped.store(true);
            if (FILE* _d = std::fopen("subsystem_label_diag.log", "a")) {
              std::fprintf(_d, "label at 0x82067EC8: \"%s\"\n", label.c_str());
              std::fclose(_d);
            }
          }
        } catch (const std::exception&) {
        }
      }
    }

    // Raw guest-memory probe on the two xenon_fn_821EEDE0 render-setup
    // threads' own per-thread handshake events (X_DISPATCH_HEADER SignalState
    // field, header+0x4) at 0x62D74 and 0x62DC4. Both threads are confirmed
    // (live debugger) permanently blocked in KeWaitForSingleObject on these
    // exact headers, and KeSetEvent is confirmed (full-run log) to never be
    // called for either. This probe tests whether the GUEST's own memory at
    // the SignalState offset ever flips to nonzero anyway (i.e. the game
    // signals via a direct memory write, bypassing the KeSetEvent export
    // entirely - a legitimate real-hardware "fast path" pattern) while
    // Xenon's host-side KernelEvent, which only reads this field once at
    // first resolution, never finds out. Logged on change only.
    {
      static std::atomic<std::uint32_t> _last_sig1{0xFFFFFFFFu};
      static std::atomic<std::uint32_t> _last_sig2{0xFFFFFFFFu};
      try {
        const auto sig1 = memory_->read32_be(static_cast<cpu::GuestAddress>(0x00062D78u));
        const auto sig2 = memory_->read32_be(static_cast<cpu::GuestAddress>(0x00062DC8u));
        if (sig1 != _last_sig1.load() || sig2 != _last_sig2.load()) {
          _last_sig1.store(sig1);
          _last_sig2.store(sig2);
          if (FILE* _d = std::fopen("handshake_event_raw_memory_diag.log", "a")) {
            std::fprintf(_d, "raw SignalState CHANGED: addr62D74+4(0x62D78)=0x%08X addr62DC4+4(0x62DC8)=0x%08X\n",
                         sig1, sig2);
            std::fclose(_d);
          }
        }
      } catch (const std::exception&) {
        // Guest memory not mapped yet (early boot) - ignore, next tick retries.
      }
    }

    // Probe the vsync ISR's own per-tick dispatch-target chain: the guest ISR
    // (0x821E63F0, via its real per-tick callee 0x821EFBE0) resolves
    // obj = read32(ctx + 0x2A94) and then conditionally writes into
    // obj + 0x4 (the X_DISPATCH_HEADER SignalState offset) once an internal
    // counter wraps. ctx here is the exact same guest address as
    // interrupt.context. Logged on change only.
    {
      static std::atomic<std::uint32_t> _last_cur{0xFFFFFFFFu};
      static std::atomic<std::uint32_t> _last_target{0xFFFFFFFFu};
      static std::atomic<std::uint32_t> _last_tickcount{0xFFFFFFFFu};
      static std::atomic<std::uint32_t> _last_gate1{0xFFFFFFFFu};
      static std::atomic<std::uint32_t> _last_gate2{0xFFFFFFFFu};
      try {
        if (interrupt.context != 0) {
          // ctx+0x412C = "current processed" counter, ctx+0x4130 = "target/pending"
          // counter inside the guest ISR's real per-tick handler (0x821EFBE0). When
          // equal, the ISR skips its entire processing chain (including the write at
          // obj+0x4) and does nothing but bump ctx+0x4094 (a plain call counter).
          // ctx+0x4130 is only ever incremented by the producer at xenon_fn_821EFCE0,
          // which itself early-outs unless ctx+0x409C != ctx+0x4094 (gate1) and
          // ctx+0x40A4 != 1 (gate2).
          const auto cur = memory_->read32_be(static_cast<cpu::GuestAddress>(interrupt.context + 0x412Cu));
          const auto target = memory_->read32_be(static_cast<cpu::GuestAddress>(interrupt.context + 0x4130u));
          const auto tickcount = memory_->read32_be(static_cast<cpu::GuestAddress>(interrupt.context + 0x4094u));
          const auto gate1 = memory_->read32_be(static_cast<cpu::GuestAddress>(interrupt.context + 0x409Cu));
          const auto gate2 = memory_->read32_be(static_cast<cpu::GuestAddress>(interrupt.context + 0x40A4u));
          if (cur != _last_cur.load() || target != _last_target.load() || tickcount != _last_tickcount.load() ||
              gate1 != _last_gate1.load() || gate2 != _last_gate2.load()) {
            _last_cur.store(cur);
            _last_target.store(target);
            _last_tickcount.store(tickcount);
            _last_gate1.store(gate1);
            _last_gate2.store(gate2);
            if (FILE* _d = std::fopen("vsync_isr_obj_chain_diag.log", "a")) {
              std::fprintf(_d,
                           "vsync ISR counters CHANGED: ctx=0x%08X cur[+0x412C]=0x%08X target[+0x4130]=0x%08X tickcount[+0x4094]=0x%08X gate1[+0x409C]=0x%08X gate2[+0x40A4]=0x%08X\n",
                           (unsigned)interrupt.context, cur, target, tickcount, gate1, gate2);
              std::fclose(_d);
            }
          }
        }
      } catch (const std::exception&) {
        // Guest memory not mapped yet (early boot) - ignore, next tick retries.
      }
    }

    // Periodic dump (every ~30s) of EVERY known thread's recent kernel-call
    // history, to get a holistic picture of what the whole system is doing
    // over time - not just the two known-stuck render-setup threads - in
    // case some other thread (asset/content loader, async I/O) stalls
    // partway through and is the real upstream blocker.
    {
      static std::atomic<int> _all_threads_dump_count{0};
      static const auto _all_threads_trace_start = Clock::now();
      const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
                                 Clock::now() - _all_threads_trace_start)
                                 .count();
      const int expected_dumps = static_cast<int>(elapsed_s / 30) + 1;
      if (elapsed_s >= 8 && _all_threads_dump_count.load() < expected_dumps) {
        _all_threads_dump_count.fetch_add(1);
        if (FILE* _d = std::fopen("all_threads_export_trace_diag.log", "a")) {
          std::fprintf(_d, "=== snapshot at t=%llds ===\n", (long long)elapsed_s);
          for (std::uint32_t tid = 0; tid <= 30u; ++tid) {
            const auto recent = export_trace_.recent_for_thread(tid, 8u);
            if (recent.empty()) continue;
            std::fprintf(_d, "--- thread_id=%u (last %zu calls) ---\n", tid, recent.size());
            for (const auto& trace : recent) {
              std::fprintf(_d, "  %s!%s lr=0x%08llX r3=0x%08llX r4=0x%08llX -> 0x%08llX\n",
                           std::string(trace.library_view()).c_str(),
                           trace.name_view().empty() ? "?" : std::string(trace.name_view()).c_str(),
                           (unsigned long long)trace.lr, (unsigned long long)trace.arguments[0],
                           (unsigned long long)trace.arguments[1], (unsigned long long)trace.result_r3);
            }
          }
          std::fclose(_d);
        }
      }
    }
    if (interrupt.callback_address != 0) {
      {
        static std::atomic<int> _vsync_cb_diag_count{0};
        const int _n = _vsync_cb_diag_count.fetch_add(1) + 1;
        if (_n <= 10) {
          if (FILE* _d = std::fopen("vsync_cb_diag.log", "a")) {
            std::fprintf(_d, "vsync callback dispatch #%d: BEFORE invoke_gpu_interrupt_callback(0x%08X)\n",
                         _n, interrupt.callback_address);
            std::fclose(_d);
          }
        }
      }
      const auto _vsync_cb_start = Clock::now();
      const bool _vsync_cb_ok = invoke_gpu_interrupt_callback(interrupt.callback_address, interrupt.context);
      const auto _vsync_cb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - _vsync_cb_start)
                                     .count();
      {
        static std::atomic<int> _vsync_cb_after_diag_count{0};
        const int _n = _vsync_cb_after_diag_count.fetch_add(1) + 1;
        if (_n <= 10 || _vsync_cb_ms >= 20) {
          if (FILE* _d = std::fopen("vsync_cb_diag.log", "a")) {
            std::fprintf(_d, "vsync callback dispatch #%d: AFTER invoke_gpu_interrupt_callback ok=%d elapsed=%lldms\n",
                         _n, _vsync_cb_ok ? 1 : 0, (long long)_vsync_cb_ms);
            std::fclose(_d);
          }
        }
      }
      if (_vsync_cb_ok) {
        gpu_interrupts_delivered_.fetch_add(1u, std::memory_order_relaxed);
      } else if (gpu_interrupts_failed_.fetch_add(1u, std::memory_order_relaxed) == 0u &&
                 config_.enable_logging) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] GPU vsync interrupt callback 0x" << std::hex
                  << interrupt.callback_address << std::dec
                  << " failed to run (first failure; later ones are only counted)" << std::endl;
      }
    }
  }
  return 0;
}

bool XenonSession::run_guest_callback(cpu::CpuState& state, cpu::GuestAddress entry,
                                    const std::shared_ptr<kernel::KernelThread>& thread) {
  // Callbacks fire tens of times a second; per-dispatch tracing would drown the log.
  const auto outcome = dispatch_guest_thread(state, entry, thread, /*trace_dispatches=*/false);
  if (outcome.crashed) {
    if (config_.enable_logging) {
      std::scoped_lock console_log_lock(console_log_mutex());
      std::cout << "[XenonSession] Guest callback 0x" << std::hex << entry << std::dec
                << " crashed: " << outcome.crash_message << std::endl;
    }
    return false;
  }
  return outcome.final_result.reason != cpu::FlowReason::Trap;
}

bool XenonSession::invoke_gpu_interrupt_callback(cpu::GuestAddress callback,
                                                 cpu::GuestAddress context,
                                                 std::uint32_t source,
                                                 std::uint32_t cpu) {
  if (!callback || !memory_ || !compiled_registry_binder_ || !kernel_process_ ||
      !gpu_pump_callback_stack_base_ || !gpu_pump_callback_stack_size_) {
    return false;
  }

  cpu::CpuState state{};
  state.cia = callback;
  state.gpr[1] = static_cast<std::uint64_t>(gpu_pump_callback_stack_base_) +
                 gpu_pump_callback_stack_size_ - 64u;
  // Real VdSetGraphicsInterruptCallback ABI, verified against xenia-project/
  // xenia's xboxkrnl_video.cc (callback registration comment: "r3 = bool
  // 0/1 - 0 is normal interrupt, 1 is some acquire/lock") and
  // gpu::GraphicsSystem::MarkVblank(), which dispatches the real vsync case
  // with DispatchInterruptCallback(0, ...) - Xenon's pump thread only ever
  // fires the normal vsync interrupt, so r3 is always 0 here. r4 is the
  // guest-supplied context pointer from the registration call
  // (VdSetGraphicsInterruptCallback's own second argument).
  state.gpr[3] = source;
  state.gpr[4] = context;
  state.gpr[13] = gpu_pump_thread_tls_.kpcr_address;
  // The title reads its current hardware-thread number from r13+0x10C (its interrupt
  // handler clears this CPU's bit in the GPU fence word). Xenia runs the callback with
  // the thread's active CPU set to the interrupt's target, defaulting to CPU 2.
  try {
    memory_->write8(gpu_pump_thread_tls_.kpcr_address + GuestKpcrLayout::kCurrentCpuOffset,
                    static_cast<std::uint8_t>(cpu));
  } catch (const memory::MemoryFault&) {
    return false;
  }

  return run_guest_callback(state, callback, gpu_pump_thread_);
}

// RuntimeServices implementation

cpu::ExecutionResult XenonSession::call(cpu::GuestAddress target,
                                       cpu::CpuState& state,
                                       cpu::MemoryPort& memory) {
  if (!code_cache_) {
    return {cpu::FlowReason::Trap, state.cia, 0};
  }

  // Look up compiled code
  auto result = code_cache_->execute(target, state, memory, *this);
  if (result) {
    return *result;
  }

  // This is the real "recompiled PPC -> import -> ExportRegistry" boundary.
  // Native XEX function imports have two records: a type-0 address record and
  // a type-1 callable thunk. Only type-1 is a callable import. Standalone
  // type-0 records are imported variables and are bound to guest-backed
  // system variables during load_game().
  if (loaded_xex_) {
    for (const auto& import : loaded_xex_->image.imports) {
      if (!import.callable() || import.guest_thunk != target) continue;
      const bool handled = external_call(import.module, import.ordinal, state, memory);
      if (handled) {
        // Fallthrough (non-terminal) matches normal call/return semantics:
        // execution continues at the instruction after the `bl`.
        return {cpu::FlowReason::Fallthrough, state.cia, 0u};
      }
      // A recognized import call site whose specific export this build does
      // not implement is still a genuine, unrecoverable call failure from
      // the guest program's point of view. Returning a non-terminal result
      // here would be silently swallowed by Op::Call/CallIndirect's
      // `if(rr.terminal()) return rr;` check, letting execution fall through
      // to the next instruction with a stale, unspecified r3 as though the
      // call had quietly succeeded - exactly the "silent ignore"/"return
      // zero and continue" pattern the completion contract forbids. Trap is
      // terminal, so it propagates all the way out to run_execution(),
      // which already turns a terminal Trap into a dispatched guest
      // exception instead of pretending nothing happened.
      if (config_.enable_export_diagnostics) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] Unresolved import call: " << import.module << "!"
                  << (import.symbol.empty() ? std::to_string(import.ordinal) : import.symbol)
                  << " at 0x" << std::hex << target << std::dec << std::endl;
      }
      return {cpu::FlowReason::Trap, target,
              static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
    }
  }

  // A thunk minted by XexGetProcedureAddress for a system export: dispatch it
  // through the ExportRegistry exactly like a XEX import thunk.
  if (module_registry_) {
    if (const auto thunk = module_registry_->thunk_target(target)) {
      if (external_call(thunk->library, thunk->ordinal, state, memory)) {
        return {cpu::FlowReason::Fallthrough, state.cia, 0u};
      }
      if (config_.enable_export_diagnostics) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] Unresolved dynamic import call: " << thunk->library << "!"
                  << thunk->ordinal << " at 0x" << std::hex << target << std::dec << std::endl;
      }
      return {cpu::FlowReason::Trap, target,
              static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
    }
  }

  // The target is neither a locally compiled function (already checked by
  // Op::Call/CallIndirect's context.lookup_compiled() before ever reaching
  // here - see backend_cpp_aot.cpp) nor a recognized XEX import call site.
  // A `bl`/`bctrl` always expects SOME code to run and produce a real
  // result, so - same reasoning as the unresolved-import case above - this
  // must be a terminal, diagnosable failure rather than a silently
  // swallowed non-terminal Branch.
  if (config_.enable_export_diagnostics) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Unresolved call target: 0x" << std::hex << target
              << " (neither compiled guest code nor a known import)" << std::dec << std::endl;
  }
  return {cpu::FlowReason::Trap, target,
          static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
}

bool XenonSession::is_recognized_import_thunk(cpu::GuestAddress target) {
  if (module_registry_ && module_registry_->is_thunk(target)) return true;
  if (!loaded_xex_) return false;
  for (const auto& import : loaded_xex_->image.imports) {
    if (import.callable() && import.guest_thunk == target) return true;
  }
  return false;
}

cpu::ExecutionResult XenonSession::syscall(std::uint32_t level,
                                          cpu::CpuState& state,
                                          cpu::MemoryPort& memory) {
  // Handle Xbox syscalls
  // For now, just return
  return {cpu::FlowReason::Syscall, state.cia + 4, level};
}

cpu::ExecutionResult XenonSession::trap(std::uint32_t trap_code,
                                       cpu::CpuState& state,
                                       cpu::MemoryPort& memory) {
  // Handle traps
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Trap: code=" << trap_code
              << " at 0x" << std::hex << state.cia << std::dec << std::endl;
  }
  return {cpu::FlowReason::Trap, state.cia, trap_code};
}

std::uint64_t XenonSession::read_spr(std::uint32_t spr,
                                    const cpu::CpuState& state) {
  // Reached only for SPRs outside mfspr/mftb's own xer/lr/ctr/vrsave/pvr/
  // time-base fast paths (dynamic_fallback.cpp) - i.e. a real, if rare, PPC
  // SPR this runtime does not model per-register semantics for. 0 is the
  // safest neutral default (matching many real-hardware unimplemented/
  // reserved SPRs' own behavior), but the access itself must not be
  // silent: counted here and surfaced in capability_report()'s "fallback"
  // section, so a title that actually depends on one shows up as a real,
  // diagnosable gap rather than a silently-wrong constant zero.
  unsupported_spr_reads_.fetch_add(1u, std::memory_order_relaxed);
  logging::Logger::instance().log_if_enabled(
      logging::Level::Debug, "cpu", [spr] {
        return "unsupported SPR read: spr=" + std::to_string(spr);
      });
  return 0;
}

void XenonSession::write_spr(std::uint32_t spr, std::uint64_t value,
                            cpu::CpuState& state) {
  // See read_spr() above - same "not silent" reasoning. The write itself
  // still has nowhere real to go (no per-SPR storage/semantics modeled),
  // but it is now an accounted, logged gap instead of a silent no-op.
  unsupported_spr_writes_.fetch_add(1u, std::memory_order_relaxed);
  logging::Logger::instance().log_if_enabled(
      logging::Level::Debug, "cpu", [spr, value] {
        return "unsupported SPR write: spr=" + std::to_string(spr) +
               " value=" + std::to_string(value);
      });
}

std::uint64_t XenonSession::read_time_base(const cpu::CpuState& state) {
  // The real Xbox 360 PPC time-base register runs at a fixed 50 MHz,
  // independent of CPU clock scaling - see TimeServices::
  // kGuestTimeBaseFrequencyHz's doc comment for how this was verified
  // against xenia-project/xenia rather than guessed. mftb/mftbu-reading
  // guest code (frame pacing, physics timestep, animation timing) needs
  // this to track real elapsed time at that exact rate, matching what
  // KeQueryPerformanceFrequency() (TimeServices::performance_frequency())
  // already reports - the two must stay in the same unit.
  return kernel::TimeServices::performance_counter();
}

// Part 15 of the AC6 Runtime Readiness pass ("boot phase checkpoints"):
// closes the gap between the 3 checkpoints wired at their own direct call
// sites (XexLoaded, EntryStarted, FirstGuestThread - none of which are
// export calls) and the remaining ones, which are all first-observed
// through a specific real guest export call. Ordinals are the same real,
// already-verified ones their own export registration files use (see the
// comment at each case) - never guessed.
void XenonSession::observe_boot_checkpoint_from_export_call(
    std::string_view module, std::uint32_t ordinal, const cpu::CpuState& state) {
  // Real XEX import tables spell library names "xboxkrnl.exe"/"xam.xex" -
  // ExportRegistry::normalize_library() does the same lowercase+strip
  // internally but is private, so this matches its exact behavior locally
  // rather than comparing against the wrong (suffixed/cased) string.
  std::string normalized_module(module);
  std::transform(normalized_module.begin(), normalized_module.end(),
                 normalized_module.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized_module.ends_with(".exe") || normalized_module.ends_with(".xex")) {
    normalized_module.resize(normalized_module.size() - 4);
  }
  if (normalized_module == "xboxkrnl") {
    switch (ordinal) {
      case 0x00D2u:  // NtCreateFile - src/xbox/exports/xboxkrnl_io_exports.cpp
      case 0x00DFu:  // NtOpenFile - src/xbox/exports/xboxkrnl_io_exports.cpp
        reach_boot_checkpoint(BootCheckpoint::FirstFileOpen);
        return;
      case 0x1F3u:  // XAudioRegisterRenderDriverClient - src/audio/exports.cpp
        reach_boot_checkpoint(BootCheckpoint::FirstAudioClient);
        return;
      default:
        return;
    }
  }
  if (normalized_module == "xam") {
    switch (ordinal) {
      case 0x0191u:  // XamInputGetState - include/xenon/xam/xam_exports.hpp
        reach_boot_checkpoint(BootCheckpoint::FirstInputPoll);
        return;
      case 0x0210u:  // XamUserGetSigninState - include/xenon/xam/xam_exports.hpp
        // gpr[3] carries the real xam::SigninState the handler wrote
        // (NotSignedIn=0) - "ready" means an actual signed-in profile, not
        // merely that the guest asked whether one exists.
        if (state.gpr[3] != 0u) {
          reach_boot_checkpoint(BootCheckpoint::ProfileReady);
        }
        return;
      case 0x025Cu:  // XamContentCreateEnumerator - include/xenon/xam/xam_exports.hpp
        reach_boot_checkpoint(BootCheckpoint::SaveEnumeration);
        return;
      default:
        return;
    }
  }
}

void XenonSession::sample_memory_watch(const WatchObserver& observer) {
  if (!memory_watch_.active() || !memory_) return;
  auto* address_space = memory_.get();
  const auto added = memory_watch_.sample(
      [address_space](cpu::GuestAddress address) -> std::optional<std::uint32_t> {
        try {
          return address_space->read32_be(address);
        } catch (const memory::MemoryFault&) {
          return std::nullopt;
        }
      },
      observer);
  if (added.empty() || !config_.enable_logging) return;
  // Logged after sample() released the watch lock so console and watch locks are
  // never held together.
  std::scoped_lock console_log_lock(console_log_mutex());
  for (const auto& record : added) {
    std::cout << "[XenonSession] " << GuestMemoryWatch::format(record) << std::endl;
  }
}

std::string XenonSession::describe_running_guest_threads() const {
  std::ostringstream out;
  out << "running=[";
  {
    std::scoped_lock lock(in_flight_exports_mutex_);
    bool first = true;
    for (const auto& [thread_id, guest_state] : guest_thread_states_) {
      if (in_flight_exports_.contains(thread_id)) continue;
      if (!first) out << ' ';
      first = false;
      out << 't' << thread_id << "@0x" << std::hex << std::uppercase << guest_state->cia
          << std::dec;
    }
  }
  out << ']';
  return out.str();
}

std::string XenonSession::describe_recent_exports(std::uint32_t thread_id,
                                                  std::size_t limit) const {
  if (thread_id == 0u || !export_trace_.enabled()) return {};
  const auto recent = export_trace_.recent_for_thread(thread_id, limit);
  std::ostringstream out;
  out << "recent=[";
  for (std::size_t i = 0; i < recent.size(); ++i) {
    if (i != 0u) out << ' ';
    const auto& trace = recent[i];
    if (!trace.name_view().empty()) {
      out << trace.name_view();
    } else {
      out << trace.ordinal;
    }
    out << "(lr=0x" << std::hex << std::uppercase << trace.lr << ",in_r3=0x" << trace.arguments[0]
        << ",in_r4=0x" << trace.arguments[1] << ",out_r3=0x" << trace.result_r3 << std::dec
        << ')';
  }
  out << ']';
  return out.str();
}

void XenonSession::start_memory_watch_poll() {
  stop_memory_watch_poll();
  if (!memory_watch_.active()) return;
  WatchObserver baseline{};
  baseline.phase = WatchPhase::Poll;
  baseline.note = "baseline";
  sample_memory_watch(baseline);
  if (config_.memory_watch_poll_ms == 0u) return;

  memory_watch_poll_running_.store(true);
  const auto period = std::chrono::milliseconds(config_.memory_watch_poll_ms);
  memory_watch_thread_ = std::thread([this, period] {
    while (memory_watch_poll_running_.load(std::memory_order_relaxed)) {
      WatchObserver observer{};
      observer.phase = WatchPhase::Poll;
      observer.describe = [this] { return describe_running_guest_threads(); };
      sample_memory_watch(observer);
      std::this_thread::sleep_for(period);
    }
  });
}

void XenonSession::stop_memory_watch_poll() noexcept {
  memory_watch_poll_running_.store(false);
  if (memory_watch_thread_.joinable()) memory_watch_thread_.join();
}

bool XenonSession::external_call(std::string_view module,
                                 std::uint32_t ordinal,
                                 cpu::CpuState& state,
                                 cpu::MemoryPort& memory) {
  // Resolves to the real calling guest thread's id via
  // kernel::ThreadManager's thread_local current-thread slot, which every
  // guest-executing host thread registers itself into once at the start of
  // its run (run_execution() for the main thread, run_audio_callback_thread()
  // for the audio callback thread, run_created_guest_thread() for
  // ExCreateThread-spawned threads) - external_call() runs synchronously on
  // whichever host thread is currently executing the guest code that issued
  // this call, so this is always the correct thread, not an approximation.
  // Previously hardcoded to 0 regardless of caller, which made
  // ExportCallContext::thread_id meaningless in production for every export
  // that uses it (e.g. NtCreateMutant/NtWaitForSingleObjectEx's real thread
  // identity/ownership semantics) even though it worked correctly in
  // isolated tests that construct ExportCallContext directly.
  std::uint32_t calling_thread_id = 0;
  if (kernel_process_) {
    if (auto current = kernel_process_->thread_manager().current_thread()) {
      calling_thread_id = current->thread_id();
    }
  }
  if (calling_thread_id == 0u) {
    // Diagnostic: a guest-executing host thread with no registered
    // KernelThread breaks every thread_id-keyed export (mutant ownership,
    // waits). Log each such host thread's first few export calls.
    static std::mutex _anon_mutex;
    static std::map<std::thread::id, int> _anon_counts;
    int _count = 0;
    {
      std::scoped_lock _lock(_anon_mutex);
      _count = ++_anon_counts[std::this_thread::get_id()];
    }
    if (_count <= 5) {
      if (FILE* _d = std::fopen("anonymous_thread_export_diag.log", "a")) {
        std::fprintf(_d, "export %s ordinal=0x%X with thread_id=0: host_thread=%zu call #%d cia=0x%08X lr=0x%08X r13=0x%08X\n",
                     std::string(module).c_str(), ordinal,
                     std::hash<std::thread::id>{}(std::this_thread::get_id()), _count,
                     static_cast<unsigned>(state.cia), static_cast<unsigned>(state.lr),
                     static_cast<unsigned>(state.gpr[13]));
        std::fclose(_d);
      }
    }
  }

  // Try the export registry first
  ExportCallContext context{state, memory, state.cia, calling_thread_id, &stop_requested_};
  std::array<std::uint64_t, 8> arguments{};
  std::copy_n(state.gpr.begin() + 3, arguments.size(), arguments.begin());
  const auto call_address = context.call_address;
  const auto call_lr = state.lr;
  const auto call_ctr = state.ctr;
  const auto* descriptor = export_registry_.resolve(module, ordinal);
  const bool track_in_flight = export_trace_.enabled() && calling_thread_id != 0u;
  if (track_in_flight) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    in_flight_exports_[calling_thread_id] = InFlightExport{
        ordinal, descriptor, call_lr, {arguments[0], arguments[1], arguments[2], arguments[3]},
        std::chrono::steady_clock::now()};
  }
  const bool watch_active = memory_watch_.active();
  const auto watch_observer = [&](WatchPhase phase) {
    WatchObserver observer{};
    observer.phase = phase;
    observer.thread_id = calling_thread_id;
    observer.cia = static_cast<cpu::GuestAddress>(call_address);
    observer.nia = static_cast<cpu::GuestAddress>(state.nia);
    observer.lr = call_lr;
    observer.note = std::string(module) + '!' +
                    (descriptor ? descriptor->name : std::to_string(ordinal));
    observer.describe = [this, calling_thread_id] {
      return describe_recent_exports(calling_thread_id, 6u);
    };
    return observer;
  };
  if (watch_active) sample_memory_watch(watch_observer(WatchPhase::BeforeKernelCall));
  auto result = export_registry_.invoke(module, ordinal, context);
  if (watch_active) sample_memory_watch(watch_observer(WatchPhase::AfterKernelCall));
  if (track_in_flight) {
    std::scoped_lock lock(in_flight_exports_mutex_);
    in_flight_exports_.erase(calling_thread_id);
  }
  export_trace_.record(calling_thread_id, module,
                       descriptor ? std::string_view(descriptor->name) : std::string_view{},
                       ordinal, call_address, call_lr, call_ctr, arguments,
                       state.gpr[3], descriptor != nullptr, result.handled, result.success);

  {
    static std::mutex _seen_exports_mutex;
    static std::set<std::string> _seen_exports;
    const std::string _name = descriptor ? std::string(descriptor->name)
                                          : (std::string(module) + "!ordinal_" + std::to_string(ordinal));
    bool _is_new = false;
    {
      std::scoped_lock _lock(_seen_exports_mutex);
      _is_new = _seen_exports.insert(_name).second;
    }
    if (_is_new) {
      if (FILE* _d = std::fopen("unique_exports_seen_diag.log", "a")) {
        std::fprintf(_d, "%s\n", _name.c_str());
        std::fclose(_d);
      }
    }
  }

  if (result.handled) {
    observe_boot_checkpoint_from_export_call(module, ordinal, state);
    return true;
  }

  // Fall back to legacy registry (for backwards compatibility)
  if (legacy_call_registry_.dispatch(module, ordinal, state, memory)) {
    return true;
  }

  // Unknown export
  if (config_.enable_export_diagnostics) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Unknown export: " << module
              << " ordinal " << ordinal
              << " at 0x" << std::hex << state.cia << std::dec << std::endl;
  }

  return false;
}

}  // namespace xenon::core
