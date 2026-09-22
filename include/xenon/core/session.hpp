#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/guest_thread_context.hpp"
#include "xenon/cpu/executable_code_cache.hpp"
#include "xenon/cpu/external_calls.hpp"
#include "xenon/cpu/runtime.hpp"
#include "xenon/cpu/state.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/gpu/backend.hpp"
#include "xenon/input/system.hpp"
#include "xenon/input/xam_guest.hpp"
#include "xenon/kernel/exception.hpp"
#include "xenon/kernel/io_manager.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/xbox_io_guest.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/xam_session.hpp"
#include "xenon/xbox/imports.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::audio {
class AudioSystem;
}

namespace xenon::core {

// Configuration for a Xenon runtime session
struct SessionConfig {
  bool enable_graphics{false};
  std::string graphics_backend{"null"};
  bool enable_input{false};
  std::vector<std::string> input_drivers{};
  std::string input_preferred_device{"Automatic"};
  float input_deadzone{0.10f};
  bool input_rumble{true};
  bool input_background{false};
  std::filesystem::path input_profile_store_path{};
  std::array<std::vector<std::string>, input::kMaxUsers> input_user_sources{};
  bool enable_audio{false};
  float audio_master_volume{1.0f};
  // Consumed by XenonSession::set_focused(): when true, losing focus mutes
  // the AudioSystem via AudioSystem::set_muted(). There is no window/focus
  // event source wired into set_focused() yet (see docs/runtime/RUNTIME_HOST.md) -
  // presentation/window creation is required before anything calls it
  // automatically - but the behavior itself is real and callable/testable.
  bool audio_mute_unfocused{false};
  // Recorded for status/diagnostics only: AudioSystem's render-driver frame
  // pump is hard-tied to the Xbox render-driver's fixed 256-sample callback
  // contract (kRenderFrameSamples), so a host-side latency/buffer-size knob
  // cannot be layered on without decoupling that guest-visible timing first.
  // Intentionally metadata-only for this phase - not silently dropped, just
  // not yet actionable.
  std::string audio_latency_profile{};
  bool enable_network{false};
  memory::GuestTranslationMode memory_mode{memory::GuestTranslationMode::Auto};
  bool enable_dynamic_compilation{false};
  bool enable_logging{true};
  // Mirrors the launcher's "developer/verboseLogging" preference
  // (LaunchConfig::log_verbose). Consumed by the runtime host to decide how
  // much diagnostic detail to print beyond XenonSession's own always-on
  // lifecycle lines (see runtime_host/src/main.cpp).
  bool verbose_logging{false};
  bool enable_export_diagnostics{true};

  // Optional host root for this launch's save data. The launcher already
  // resolves profile/game-specific savePath; wiring it through SessionConfig
  // keeps SaveManager out of guessed platform folders. Empty means use a
  // conservative temporary fallback for direct/headless callers.
  std::filesystem::path save_root_path{};

  // Optional path to a game-specific native extension library ("module" in
  // launcher terms: symbols, patches, and the recomp-driver-generated
  // compiled-code registry for one title). See docs/runtime/RUNTIME_HOST.md for the
  // export contract. Left empty, the session has no compiled guest code to
  // run and start() will report that clearly rather than pretending to
  // execute anything.
  std::string native_extension_path{};
};

// Diagnostic summary of a single unresolved XEX import, kept for status
// reporting/support bundles. This is informational only: unresolved imports
// are resolved lazily at call time through ExportRegistry (see
// XenonSession::external_call), not patched at load time.
struct UnresolvedImport {
  std::string library{};
  std::string symbol{};
  std::uint32_t ordinal{};
};

// Lifecycle state of a session
enum class SessionState : std::uint8_t {
  Uninitialized,
  Initializing,
  Ready,
  LoadingGame,
  Running,
  Paused,
  Stopping,
  Stopped,
  Failed
};

// Result of a session operation
struct SessionResult {
  bool success{false};
  std::string message{};
  SessionState state{SessionState::Failed};

  [[nodiscard]] static SessionResult ok(std::string message = {},
                                       SessionState state = SessionState::Ready) {
    return {true, std::move(message), state};
  }

  [[nodiscard]] static SessionResult failure(std::string message,
                                            SessionState state = SessionState::Failed) {
    return {false, std::move(message), state};
  }
};

// The single Xenon runtime session
class XenonSession final : public cpu::RuntimeServices {
 public:
  XenonSession();
  ~XenonSession() override;

  XenonSession(const XenonSession&) = delete;
  XenonSession& operator=(const XenonSession&) = delete;

  // Lifecycle
  [[nodiscard]] SessionResult initialize(const SessionConfig& config);
  
  // Loads the game. `xex_bytes` is always the immutable base executable;
  // when `title_update_bytes` is non-empty, it is validated and applied via
  // xbox::apply_title_update() (XEX Loader V2's canonical XEXP patcher)
  // before anything is mapped into memory, and the resulting *effective*
  // (patched) image - not the base image - becomes what actually gets
  // mapped, analyzed for imports, and executed. An empty (default)
  // `title_update_bytes` preserves the base-only launch path unchanged. See
  // effective_identity() for the resulting executable's identity and
  // docs/runtime/RUNTIME_SESSION.md for the full title-update integration.
  [[nodiscard]] SessionResult load_game(std::span<const std::byte> xex_bytes,
                                       std::string_view game_id = {},
                                       std::span<const std::byte> title_update_bytes = {});
  
  // Build and mount content graph for a title
  [[nodiscard]] SessionResult mount_content_graph(
      std::uint32_t title_id,
      const std::filesystem::path& base_content_path,
      const std::filesystem::path& title_update_path,
      const std::filesystem::path& dlc_path,
      std::uint64_t profile_xuid);
  
  // Simple content mounting (legacy)
  [[nodiscard]] SessionResult mount_content(std::string_view host_path,
                                           std::string_view guest_mount_point);
  
  [[nodiscard]] SessionResult start();
  [[nodiscard]] SessionResult pause();
  [[nodiscard]] SessionResult resume();
  [[nodiscard]] SessionResult stop();
  void shutdown();

  // State. Execution runs on a dedicated thread once start() succeeds, so
  // these are safe to call from another thread (e.g. a runtime host's
  // status-reporting loop) while the game is running.
  [[nodiscard]] SessionState state() const noexcept;
  [[nodiscard]] bool is_initialized() const noexcept;
  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] std::string last_error() const;

  // True once the guest execution thread has actually started running
  // compiled code (distinct from SessionState::Running, which is set the
  // moment start() is accepted).
  [[nodiscard]] bool execution_active() const noexcept { return execution_active_.load(); }
  // True once the native extension's compiled-code registry has been bound
  // successfully; see load_native_extension(). Surfaced in status/UI as
  // "recompilation state".
  [[nodiscard]] bool native_extension_bound() const noexcept { return native_extension_bound_; }
  [[nodiscard]] const std::string& native_extension_error() const noexcept {
    return native_extension_error_;
  }
  [[nodiscard]] const std::vector<UnresolvedImport>& unresolved_imports() const noexcept {
    return unresolved_imports_;
  }
  // Ask a running game to stop. This does not forcibly preempt already
  // executing native compiled code (there is no interpreter/yield point to
  // preempt at) - it flips a flag a well-behaved native extension may poll,
  // and callers that need a hard stop should terminate the hosting process.
  [[nodiscard]] bool stop_requested() const noexcept { return stop_requested_.load(); }

  // Host focus signal (e.g. the presentation window gaining/losing OS
  // keyboard/input focus). Forwards to InputSystem::set_focused() always,
  // and additionally mutes AudioSystem when SessionConfig::audio_mute_unfocused
  // was requested. Safe to call before graphics/audio/input are enabled.
  void set_focused(bool focused);

  // Subsystem access
  [[nodiscard]] memory::AddressSpace* memory() noexcept { return memory_.get(); }
  [[nodiscard]] filesystem::VirtualFileSystem* filesystem() noexcept;
  [[nodiscard]] kernel::KernelIoManager* kernel_io() noexcept;
  [[nodiscard]] input::InputSystem* input() noexcept { return input_.get(); }
  [[nodiscard]] gpu::Backend* gpu() noexcept { return gpu_.get(); }
  [[nodiscard]] audio::AudioSystem* audio() noexcept {
#if defined(XENON_HAS_AUDIO)
    return audio_.get();
#else
    return nullptr;
#endif
  }
  [[nodiscard]] xam::XamSession* xam() noexcept { return xam_.get(); }
  [[nodiscard]] ExportRegistry* exports() noexcept { return &export_registry_; }
  [[nodiscard]] const xbox::LoadedXex* loaded_xex() const noexcept;
  // The content graph built by mount_content_graph(), if any - null before
  // that call succeeds. load_game() reads selected_title_update() from this
  // (via the runtime host, which owns the two calls' ordering) rather than
  // rediscovering a title update itself; Content Services remains the one
  // place a launch's update selection is decided.
  [[nodiscard]] const xam::ContentGraph* content_graph() const noexcept { return content_graph_.get(); }
  // Identity of the executable actually mapped/run by the most recent
  // load_game() call: null before load_game() succeeds. Distinguishes a
  // base-only launch from a title-update-patched one (see
  // xbox::XexEffectiveIdentity) - this is what module-compatibility
  // validation and status/diagnostics reporting compare against, never the
  // raw base XEX's own identity once a title update has been applied.
  [[nodiscard]] const std::optional<xbox::XexEffectiveIdentity>& effective_identity() const noexcept {
    return effective_identity_;
  }
  // The canonical owner/context for the running title once load_game()
  // succeeds: null before that. Normal execution is
  // XenonSession -> KernelProcess -> KernelThread -> CPU V2, not a bare
  // CpuState (see create_guest_process()/run_execution()).
  [[nodiscard]] kernel::KernelProcess* kernel_process() noexcept { return kernel_process_.get(); }
  [[nodiscard]] kernel::KernelThread* main_thread() noexcept { return main_thread_.get(); }

  // RuntimeServices implementation
  cpu::ExecutionResult call(cpu::GuestAddress target, cpu::CpuState& state,
                           cpu::MemoryPort& memory) override;
  cpu::ExecutionResult syscall(std::uint32_t level, cpu::CpuState& state,
                              cpu::MemoryPort& memory) override;
  cpu::ExecutionResult trap(std::uint32_t trap_code, cpu::CpuState& state,
                           cpu::MemoryPort& memory) override;
  std::uint64_t read_spr(std::uint32_t spr, const cpu::CpuState& state) override;
  void write_spr(std::uint32_t spr, std::uint64_t value, cpu::CpuState& state) override;
  std::uint64_t read_time_base(const cpu::CpuState& state) override;
  kernel::KernelProcess* current_process() noexcept override { return kernel_process_.get(); }
  bool external_call(std::string_view module, std::uint32_t ordinal,
                    cpu::CpuState& state, cpu::MemoryPort& memory) override;

 private:
  bool init_memory();
  bool init_filesystem();
  bool init_kernel();
  bool init_cpu();
  bool init_gpu();
  bool init_input();
  bool init_audio();
  bool init_xam();
  bool init_exports();
  bool resolve_xex_imports();
  bool bind_compiled_code();
  bool create_guest_process();
  // Releases whatever create_guest_process() had already allocated when a
  // later step in that same call fails (guest stack, kernel_process_ and the
  // kernel_memory_/module registration it owns). Without this, a failure
  // partway through create_guest_process() (e.g. TLS setup) used to leave
  // those resources permanently alive with nothing left referencing them
  // meaningfully - a real leak, not a hypothetical one. Safe to call whether
  // or not each resource was actually allocated yet.
  void release_partial_guest_process() noexcept;
  void set_state(SessionState new_state, std::string message = {});
  void set_error(std::string error);
  // Guest execution thread body: builds a CPU V2 ExecutionContext, binds the
  // native extension's compiled registry into it, looks up the entry point,
  // and invokes it. Runs entirely on main_thread_'s own host thread (it is
  // the kernel::KernelThread's ThreadEntry - see create_guest_process()).
  // Returns the guest exit code.
  [[nodiscard]] std::uint32_t run_execution();
#if defined(XENON_HAS_AUDIO)
  [[nodiscard]] bool invoke_audio_callback(cpu::GuestAddress callback,
                                           cpu::GuestAddress argument);
  // Creates and starts the audio callback's own kernel::KernelThread (with
  // its own KPCR/static-TLS block, distinct from main_thread_tls_) and wires
  // AudioSystem's guest-callback invoker to run through it. Called once from
  // create_guest_process(), after kernel_process_/loaded_xex_ exist, so guest
  // render-driver callbacks always execute with a legitimate
  // KernelProcess/KernelThread/KPCR/TLS identity instead of a bare CpuState -
  // see docs/audio/AUDIO_V1.md and docs/runtime/RUNTIME_SESSION.md for the execution model.
  [[nodiscard]] bool start_audio_guest_thread();
  // Entry point (kernel::ThreadEntry) for audio_thread_: registers itself as
  // the current KernelThread for this host thread, then blocks running
  // AudioSystem's guest-callback pump loop until stop_guest_callback_pump().
  [[nodiscard]] std::uint32_t run_audio_callback_thread();
#endif
  void load_native_extension();
  void unload_native_extension() noexcept;

  SessionConfig config_{};
  mutable std::mutex status_mutex_{};
  SessionState state_{SessionState::Uninitialized};
  std::string last_error_{};
  // shared_ptr: kernel::KernelMemory (owned by kernel_process_ via
  // KernelProcess's own shared_ptr<KernelMemory>) needs shared ownership of
  // the same AddressSpace XenonSession itself uses - not a second Memory V2
  // instance or mapping set, just a second reference to this one.
  std::shared_ptr<memory::AddressSpace> memory_{};
  std::unique_ptr<cpu::ExecutableCodeCache> code_cache_{};
  // shared_ptr: kernel::KernelIoManager takes shared ownership of the VFS.
  std::shared_ptr<filesystem::VirtualFileSystem> filesystem_{};
  std::unique_ptr<kernel::KernelIoManager> kernel_io_{};
  // Guest-memory marshalling for xboxkrnl file I/O (NtCreateFile et al.),
  // wrapping this same kernel_io_ - not a second filesystem/kernel-I/O
  // implementation. xbox_imports_ is the ordinal/thunk table
  // register_xboxkrnl_io_imports() already defines; init_exports() bridges
  // each of its entries into export_registry_ so guest calls reach it
  // through the one canonical registry instead of being unreachable (see
  // xbox::ImportRegistry's doc comment history - previously only exercised
  // by tests/xbox/xbox_import_tests.cpp, never constructed by any session).
  std::unique_ptr<kernel::xbox::GuestIoBridge> io_bridge_{};
  xbox::ImportRegistry xbox_imports_{};
  std::unique_ptr<input::InputSystem> input_{};
  // Bridges guest XamInput* calls to input_. Registered into the same
  // export_registry_ that XAM/Audio use (see init_exports()) rather than the
  // separate cpu::ExternalCallRegistry input::xam::guest historically
  // targeted, so guest input calls reach the active session through the one
  // canonical dispatch path. Declared after input_ so it is destroyed first.
  std::unique_ptr<input::xam::guest::GuestInputBridge> input_bridge_{};
  std::unique_ptr<gpu::Backend> gpu_{};
#if defined(XENON_HAS_AUDIO)
  std::unique_ptr<audio::AudioSystem> audio_{};
#endif
  std::unique_ptr<xam::XamSession> xam_{};
  ExportRegistry export_registry_{};
  cpu::ExternalCallRegistry legacy_call_registry_{};
  std::optional<xbox::LoadedXex> loaded_xex_{};
  std::optional<xbox::XexEffectiveIdentity> effective_identity_{};
  std::unique_ptr<xam::ContentGraph> content_graph_{};
  std::string game_id_{};
  std::unique_ptr<cpu::CpuState> main_cpu_state_{};
  std::uint64_t time_base_counter_{0};

  // Real guest process/thread model: XenonSession -> KernelProcess ->
  // KernelThread -> CPU V2, replacing the previous bare
  // "CpuState + stack + entry point" startup. kernel_memory_ wraps this same
  // memory_ (shared, not a second Memory V2 instance/mapping set).
  // kernel_process_ becomes the canonical owner/context for the running
  // title once create_guest_process() succeeds: it holds the loaded module's
  // identity (KernelModule: base/size/entry/TLS info) and the main
  // KernelThread. main_thread_ replaces what used to be a bare
  // execution_thread_ std::thread; its ThreadEntry runs run_execution().
  std::shared_ptr<kernel::KernelMemory> kernel_memory_{};
  std::shared_ptr<kernel::KernelProcess> kernel_process_{};
  std::shared_ptr<kernel::KernelThread> main_thread_{};
  // Guest-visible KPCR + compiler-emitted static TLS block for main_thread_,
  // pointed to by that thread's CpuState::gpr[13] - see
  // guest_thread_context.hpp for the field layout and why this lives in
  // xenon_core rather than xenon_kernel.
  GuestThreadTlsContext main_thread_tls_{};
  // Connects Memory V2 faults (thrown as memory::MemoryFault from guest
  // memory accesses during run_execution()) and CPU traps to a structured,
  // process/thread-scoped guest exception path instead of only a stringified
  // last_error(). See run_execution()'s catch clauses.
  kernel::ExceptionDispatcher exception_dispatcher_{};

  // Native extension (module) dynamic loading. See docs/runtime/RUNTIME_HOST.md for
  // the export contract a module's compiled-code library must provide.
  void* native_extension_handle_{};
  std::function<void(cpu::ExecutionContext&)> compiled_registry_binder_{};
  bool native_extension_bound_{false};
  std::string native_extension_error_{};
  std::vector<UnresolvedImport> unresolved_imports_{};

  // Guest stack allocated for the main thread's CpuState (r1) in
  // create_guest_process().
  memory::GuestAddress stack_base_{};
  std::uint32_t stack_size_{};
#if defined(XENON_HAS_AUDIO)
  memory::GuestAddress audio_callback_stack_base_{};
  std::uint32_t audio_callback_stack_size_{};
  // The audio callback's own KernelThread + KPCR/TLS context - a real guest
  // thread identity distinct from main_thread_/main_thread_tls_ (see
  // start_audio_guest_thread()). Created once in create_guest_process() and
  // joined/released in shutdown() before kernel_process_ is destroyed.
  std::shared_ptr<kernel::KernelThread> audio_thread_{};
  GuestThreadTlsContext audio_thread_tls_{};
#endif

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> execution_active_{false};
};

}  // namespace xenon::core
