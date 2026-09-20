#include "xenon/core/session.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/exports.hpp"
#include "xenon/audio/system.hpp"
#endif

#include <iomanip>
#include <iostream>
#include <sstream>

#include "xenon/xam/content_graph.hpp"

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
// ExecutionContext. Documented in docs/RUNTIME_HOST.md; module authors
// (e.g. Project Gracemeria) implement this once around their generated
// registry.cpp's bind_compiled_registry(ExecutionContext&).
using XenonBindCompiledRegistryFn = void (*)(cpu::ExecutionContext&);
constexpr const char* kBindCompiledRegistrySymbol = "Xenon_BindCompiledRegistry";

}  // namespace

XenonSession::XenonSession() = default;
XenonSession::~XenonSession() {
  shutdown();
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
    std::cout << "[XenonSession] " << error << std::endl;
  }
}

SessionResult XenonSession::initialize(const SessionConfig& config) {
  if (is_initialized()) {
    return SessionResult::failure("Session already initialized");
  }

  set_state(SessionState::Initializing, "Initializing session...");
  config_ = config;

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
  stop_requested_.store(true);
  if (execution_thread_.joinable()) {
    execution_thread_.join();
  }

#if defined(XENON_HAS_AUDIO)
  // Stop native callbacks before unloading the title's compiled registry.
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
  unload_native_extension();

  // Shutdown in reverse order of initialization
  xam_.reset();

  if (input_) {
    input_->shutdown();
    input_.reset();
  }

  gpu_.reset();
  code_cache_.reset();
  kernel_io_.reset();
  filesystem_.reset();

  if (memory_) {
    memory_->reset();
    memory_.reset();
  }

  loaded_xex_.reset();
  main_cpu_state_.reset();
  export_registry_.clear();

  set_state(SessionState::Stopped, "Session shut down");
}

bool XenonSession::init_memory() {
  memory_ = std::make_unique<memory::AddressSpace>(config_.memory_mode);
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
  return true;
}

bool XenonSession::init_cpu() {
  code_cache_ = std::make_unique<cpu::ExecutableCodeCache>();
  main_cpu_state_ = std::make_unique<cpu::CpuState>();
  return true;
}

bool XenonSession::init_gpu() {
  // For now, always create a null backend
  // In the future, this will select based on config_.graphics_backend
  gpu_ = std::make_unique<gpu::NullBackend>();
  return true;
}

bool XenonSession::init_input() {
  input_ = std::make_unique<input::InputSystem>();
  
  // Add configured drivers
  // For now, this is a placeholder - actual driver instantiation
  // will be added when we have proper driver factories
  
  auto result = input_->setup();
  if (result != input::Result::Success) {
    set_error("Input system setup failed");
    return false;
  }
  
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
  if (!audio_->initialize(&error)) {
    set_error(error.empty() ? "Audio system initialization failed" : error);
    audio_.reset();
    return false;
  }

  // Render-driver callbacks execute guest code on the audio worker. Give
  // them a dedicated PPC stack and an independent CpuState so the audio
  // thread never races the main guest thread's register file.
  constexpr std::uint32_t kAudioCallbackStackSize = 128u * 1024u;
  if (!memory_->allocate(kAudioCallbackStackSize, 16, memory::kReadWrite,
                         /*top_down=*/true, audio_callback_stack_base_)) {
    set_error("Failed to allocate guest audio callback stack");
    audio_->shutdown();
    audio_.reset();
    return false;
  }
  audio_callback_stack_size_ = kAudioCallbackStackSize;
  audio_->set_guest_callback_invoker(
      [this](cpu::GuestAddress callback, cpu::GuestAddress argument) {
        return invoke_audio_callback(callback, argument);
      });
  return true;
#else
  set_error("Audio was requested, but this Xenon build has no production audio subsystem");
  return false;
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
  
  // Register XAM exports
  if (xam_ && !xam_->register_exports(export_registry_)) {
    set_error("Failed to register XAM exports");
    return false;
  }

#if defined(XENON_HAS_AUDIO)
  if (config_.enable_audio && audio_ &&
      !audio::register_xbox_audio_exports(export_registry_, *audio_)) {
    set_error("Failed to register Xbox audio exports");
    return false;
  }
#endif
  
  return true;
}

SessionResult XenonSession::load_game(std::span<const std::byte> xex_bytes,
                                     std::string_view game_id) {
  if (!is_initialized()) {
    return SessionResult::failure("Session not initialized");
  }

  if (state() != SessionState::Ready) {
    return SessionResult::failure("Session not in ready state");
  }

  set_state(SessionState::LoadingGame, "Loading game...");
  game_id_ = std::string(game_id);

  // Load XEX into memory
  xbox::LoadedXex loaded{};
  std::string error;
  if (!xbox::load_xex(*memory_, xex_bytes, loaded, memory::kXex64KBase, &error)) {
    set_error("Failed to load XEX: " + error);
    return SessionResult::failure(last_error_);
  }

  loaded_xex_ = std::move(loaded);

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

  set_state(SessionState::Ready, "Game loaded successfully");
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
    // Use default save directory
    auto documents = std::filesystem::temp_directory_path().parent_path() / "Documents";
    auto save_dir = documents / "Xenon" / "Saves";
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
  if (execution_thread_.joinable()) {
    execution_thread_.join();
  }
  if (!native_extension_bound_) {
    return SessionResult::failure(
        "No compiled game code is available to execute: " +
        (native_extension_error_.empty()
             ? std::string("this game's module supplied no native extension")
             : native_extension_error_));
  }

  stop_requested_.store(false);
  set_state(SessionState::Running, "Starting game execution...");
  execution_thread_ = std::thread([this]() { run_execution(); });

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
    if (execution_thread_.joinable()) execution_thread_.join();
    set_state(SessionState::Stopped, "Game stopped");
    return SessionResult::ok("Game stopped", SessionState::Stopped);
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

  // Static recompilation bakes direct guest-to-guest calls into the
  // generated native code, so nothing needs patching here. This pass is a
  // preflight diagnostic: it records which imports this build's export
  // registry does not (yet) know about, surfaced as compatibility warnings
  // in session/game status. Missing entries are resolved lazily and
  // per-call through external_call() rather than failing the whole load,
  // since most titles only exercise a fraction of their imports.
  unresolved_imports_.clear();
  for (const auto& import : loaded_xex_->image.imports) {
    const bool resolved = !import.symbol.empty()
                               ? export_registry_.contains(import.module, import.symbol)
                               : export_registry_.contains(import.module, import.ordinal);
    if (resolved) continue;
    unresolved_imports_.push_back(
        UnresolvedImport{import.module, import.symbol, import.ordinal});
    if (config_.enable_export_diagnostics) {
      std::cout << "[XenonSession] Unresolved import: " << import.module << " '"
                << import.symbol << "' ordinal " << import.ordinal << std::endl;
    }
  }
  return true;
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

  return true;
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
      std::cout << "[XenonSession] Native extension load failed: " << native_extension_error_
                << std::endl;
    }
    unload_native_extension();
    return;
  }

  auto* bind_fn = reinterpret_cast<XenonBindCompiledRegistryFn>(symbol);
  compiled_registry_binder_ = [bind_fn](cpu::ExecutionContext& context) { bind_fn(context); };
  native_extension_bound_ = true;
  if (config_.enable_logging) {
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

void XenonSession::run_execution() {
  execution_active_.store(true);
  const auto entry = loaded_xex_->image.entry_point;

  cpu::ExecutionContext context(*main_cpu_state_, *memory_, *this);
  if (compiled_registry_binder_) {
    compiled_registry_binder_(context);
  }

  if (!context.compiled_lookup) {
    execution_active_.store(false);
    set_error("No compiled game code is available to execute");
    return;
  }

  auto* entry_fn = context.lookup_compiled(entry, cpu::CompiledLookupKind::Call);
  if (!entry_fn) {
    std::ostringstream address;
    address << std::hex << std::uppercase << entry;
    execution_active_.store(false);
    set_error("The native extension's compiled registry has no entry for 0x" + address.str());
    return;
  }

  cpu::ExecutionResult result{};
  bool crashed = false;
  std::string crash_message;
  try {
    result = entry_fn(context);
  } catch (const std::exception& ex) {
    crashed = true;
    crash_message = std::string("Unhandled exception during guest execution: ") + ex.what();
  } catch (...) {
    crashed = true;
    crash_message = "Unhandled unknown exception during guest execution";
  }

  execution_active_.store(false);

  if (crashed) {
    set_error(crash_message);
    return;
  }
  if (stop_requested_.load()) {
    set_state(SessionState::Stopped, "Game execution ended (stop requested)");
    return;
  }
  if (result.reason == cpu::FlowReason::Trap) {
    set_error("Game execution trapped (code " + std::to_string(result.detail) + ")");
    return;
  }
  set_state(SessionState::Stopped, "Game execution completed");
}

#if defined(XENON_HAS_AUDIO)
bool XenonSession::invoke_audio_callback(cpu::GuestAddress callback,
                                         cpu::GuestAddress argument) {
  if (!callback || !memory_ || !compiled_registry_binder_ ||
      !audio_callback_stack_base_ || !audio_callback_stack_size_) {
    return false;
  }

  cpu::CpuState state{};
  state.cia = callback;
  state.gpr[1] = static_cast<std::uint64_t>(audio_callback_stack_base_) +
                 audio_callback_stack_size_ - 64u;
  state.gpr[3] = argument;

  cpu::ExecutionContext context(state, *memory_, *this);
  compiled_registry_binder_(context);
  if (!context.compiled_lookup) return false;
  auto* fn = context.lookup_compiled(callback, cpu::CompiledLookupKind::Call);
  if (!fn) return false;

  try {
    const auto result = fn(context);
    return result.reason != cpu::FlowReason::Trap;
  } catch (...) {
    return false;
  }
}
#endif

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

  // No compiled code found
  return {cpu::FlowReason::Branch, target, 0};
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
    std::cout << "[XenonSession] Trap: code=" << trap_code 
              << " at 0x" << std::hex << state.cia << std::dec << std::endl;
  }
  return {cpu::FlowReason::Trap, state.cia, trap_code};
}

std::uint64_t XenonSession::read_spr(std::uint32_t spr,
                                    const cpu::CpuState& state) {
  // Handle SPR reads
  // For now, just return 0
  return 0;
}

void XenonSession::write_spr(std::uint32_t spr, std::uint64_t value,
                            cpu::CpuState& state) {
  // Handle SPR writes
  // For now, do nothing
}

std::uint64_t XenonSession::read_time_base(const cpu::CpuState& state) {
  // Simple incrementing counter for now
  // In a real implementation, this would be based on host time
  return time_base_counter_++;
}

bool XenonSession::external_call(std::string_view module,
                                 std::uint32_t ordinal,
                                 cpu::CpuState& state,
                                 cpu::MemoryPort& memory) {
  // Try the export registry first
  ExportCallContext context{state, memory, state.cia, 0};
  auto result = export_registry_.invoke(module, ordinal, context);
  
  if (result.handled) {
    return true;
  }

  // Fall back to legacy registry (for backwards compatibility)
  if (legacy_call_registry_.dispatch(module, ordinal, state, memory)) {
    return true;
  }

  // Unknown export
  if (config_.enable_export_diagnostics) {
    std::cout << "[XenonSession] Unknown export: " << module 
              << " ordinal " << ordinal 
              << " at 0x" << std::hex << state.cia << std::dec << std::endl;
  }

  return false;
}

}  // namespace xenon::core

