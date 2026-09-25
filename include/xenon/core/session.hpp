#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "xenon/core/capability_report.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/core/guest_thread_context.hpp"
#include "xenon/cpu/dynamic_fallback.hpp"
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

  // Runtime input preferences written by the launcher.  Keep these in the
  // core session contract rather than runtime_host so direct embedders and
  // tests receive identical input behavior.
  std::string input_preferred_device{"Automatic"};
  double input_deadzone{0.10};
  bool input_rumble{true};
  bool input_background{false};
  int input_module_api_version{1};
  std::string input_profile_store_path{};
  // Per-user source selectors. runtime_host indexes this table by the
  // launch-config userIndex (0..kMaxUsers-1), and XenonSession resolves each
  // string against enumerated device identity/persistent key/display name/
  // driver name. A fixed-size table keeps the launcher/core contract
  // deterministic and prevents duplicate user records.
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
  // Gen 7 correctness/discovery safety net. AOT remains primary; this only
  // executes a bounded PPC fragment after a compiled lookup genuinely misses.
  bool enable_dynamic_fallback{true};
  bool enable_logging{true};
  // Mirrors the launcher's "developer/verboseLogging" preference
  // (LaunchConfig::log_verbose). Consumed by the runtime host to decide how
  // much diagnostic detail to print beyond XenonSession's own always-on
  // lifecycle lines (see runtime_host/src/main.cpp).
  bool verbose_logging{false};
  bool enable_export_diagnostics{true};

  // Optional newline-delimited adaptive-analysis trace. Every compiled-code
  // lookup miss records the exact target, current guest CIA and whether the
  // transfer was call-like or branch-like. recomp-driver can consume this on
  // a later pass via --observations; empty disables collection entirely.
  std::filesystem::path adaptive_observation_path{};
  // Optional second path used for per-session diagnostics/support bundles.
  // The stable path above is what feeds the next preparation pass; this mirror
  // keeps each runtime session self-contained for debugging.
  std::filesystem::path adaptive_observation_mirror_path{};

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
  // Builds the current capability/diagnostic report (Phase 0 of the AC6
  // Runtime Readiness pass): every section any subsystem has published to
  // capability_report_builder() so far, tagged with a run fingerprint
  // computed from this session's current identity/config. Safe to call at
  // any point in the session's lifetime, including before a title is loaded
  // (fingerprint fields default to empty strings rather than throwing).
  [[nodiscard]] JsonValue capability_report() const;
  // Direct access to the underlying builder so subsystems (import capability
  // audit, fallback accounting, GPU telemetry, ...) can publish their own
  // named sections without XenonSession needing to know about each one.
  [[nodiscard]] CapabilityReportBuilder& capability_report_builder() noexcept {
    return capability_report_builder_;
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
  friend struct SessionExecutionTestAccess;
  bool init_memory();
  bool init_filesystem();
  bool init_kernel();
  bool init_cpu();
  bool init_gpu();
  bool init_input();
  bool init_audio();
  bool init_xam();
  bool init_exports();
  bool init_kernel_variable_exports();
  bool bind_xex_variable_imports();
  bool refresh_dynamic_kernel_variables();
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

  // Outcome of dispatch_guest_thread() - enough information for a caller to
  // decide what THREAD-SPECIFIC session bookkeeping to apply (SessionState
  // transitions, execution_active_, last_error()) without dispatch_guest_thread()
  // itself needing to know which guest thread (main vs. a created one) is
  // running it.
  struct GuestDispatchOutcome {
    bool crashed{false};
    std::string crash_message{};
    std::uint32_t crash_exit_code{0xC0000005u};  // NTSTATUS-style default (access violation).
    cpu::ExecutionResult final_result{};
    bool stop_requested{false};
    // True when the loop exited because thread->is_terminated() was
    // observed at a safepoint (terminate() was called - possibly while this
    // thread was mid-dispatch, not just parked/blocked). Distinct from
    // stop_requested (session-wide): this is a per-thread terminate(). When
    // set, the caller should use thread->exit_code() (terminate()'s own
    // committed value), not any in-flight CpuState register, as the
    // authoritative exit code.
    bool thread_terminated{false};
  };
  // Shared AOT/fallback guest-code dispatch core: runs `entry` on `state`
  // (a CPU V2 ExecutionContext built around it) until the guest returns,
  // faults, traps, halts, stop_requested_ (session-wide) is set, or `thread`
  // (if non-null) is suspended/terminated. Used by BOTH run_execution() (the
  // main thread) and run_created_guest_thread() (ExCreateThread-spawned
  // threads) so the dispatch loop, fault handling, exception-dispatch logic,
  // and preemptive safepoint exist exactly once rather than being duplicated
  // per thread kind. Deliberately does NOT touch any caller/thread-specific
  // session state (execution_active_, SessionState transitions,
  // last_error()) - see GuestDispatchOutcome above.
  //
  // Preemptive safepoint (Phase 2 of the AC6 Runtime Readiness pass): at
  // every compiled-block boundary (Branch/Fallthrough dispatch), calls
  // thread->wait_while_suspended() (parks if suspended, cheap no-op
  // otherwise) and checks thread->is_terminated() (lock-free), so
  // suspend()/terminate() from another host thread are honored within
  // roughly one block's latency for any guest thread actually executing
  // code - not only one blocked in a wait/sleep of its own. `thread` may be
  // null (no safepoint checks performed) for callers that do not have a
  // KernelThread yet.
  [[nodiscard]] GuestDispatchOutcome dispatch_guest_thread(
      cpu::CpuState& state, cpu::GuestAddress entry,
      const std::shared_ptr<kernel::KernelThread>& thread);
  // Guest execution thread body: builds a CPU V2 ExecutionContext, binds the
  // native extension's compiled registry into it, looks up the entry point,
  // and invokes it via dispatch_guest_thread(), then applies main-thread-only
  // session bookkeeping to the outcome. Runs entirely on main_thread_'s own
  // host thread (it is the kernel::KernelThread's ThreadEntry - see
  // create_guest_process()). Returns the guest exit code.
  [[nodiscard]] std::uint32_t run_execution();
  // ExCreateThread's ThreadEntry body (Phase 1/2 of the AC6 Runtime
  // Readiness pass): registers `thread` as the current guest thread
  // identity, dispatches start_address(start_context) on a fresh
  // CpuState/stack/KPCR-TLS via dispatch_guest_thread(), and releases that
  // stack/TLS once the thread exits (naturally or via crash) - see
  // docs/kernel/THREADING_V2.md. The exit code is the PPC ABI return value
  // (gpr[3]) on a natural return, matching real ExCreateThread/
  // ExTerminateThread semantics (returning from the thread function is an
  // implicit ExTerminateThread(returnValue)).
  [[nodiscard]] std::uint32_t run_created_guest_thread(
      std::shared_ptr<kernel::KernelThread> thread, cpu::GuestAddress start_address,
      std::uint64_t start_context, memory::GuestAddress stack_base,
      std::uint32_t stack_size, GuestThreadTlsContext tls);
  // ExCreateThread export handler: allocates a guest stack and KPCR/TLS
  // block, creates the KernelThread via kernel_process_->thread_manager()
  // (honoring ThreadCreationParams::create_suspended from the guest's
  // CREATE_SUSPENDED flag), publishes its Handle through
  // kernel_process_->handle_table() (the same shared dispatcher-object table
  // NtWaitForSingleObjectEx et al. already use, so a thread handle is
  // waitable like any other kernel object), and starts it.
  [[nodiscard]] bool export_ex_create_thread(ExportCallContext& context);
  static void record_compiled_lookup_miss(void* observer,
                                          cpu::ExecutionContext& context,
                                          cpu::GuestAddress target,
                                          cpu::CompiledLookupKind kind);
  void record_dynamic_fallback_observation(
      const cpu::DynamicFallbackObservation& observation);
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
  // Runtime feedback can be emitted from the main guest thread, audio guest
  // thread, or future worker callbacks. Serialize JSONL append operations so
  // individual facts remain line-atomic and ingestible.
  mutable std::mutex adaptive_observation_mutex_{};
  // A missing compiled entry may sit on a hot indirect-call path. Record each
  // (target, site, transfer-kind) fact once per session rather than performing
  // append-only disk I/O on every execution of the same edge. Cross-session
  // repetition is still preserved by the JSONL ingest/aggregation layer.
  std::set<std::tuple<cpu::GuestAddress, cpu::GuestAddress, cpu::CompiledLookupKind>>
      adaptive_observation_seen_{};
  std::set<std::pair<cpu::GuestAddress, std::uint64_t>>
      dynamic_fallback_observation_seen_{};
  SessionState state_{SessionState::Uninitialized};
  std::string last_error_{};
  // shared_ptr: kernel::KernelMemory (owned by kernel_process_ via
  // KernelProcess's own shared_ptr<KernelMemory>) needs shared ownership of
  // the same AddressSpace XenonSession itself uses - not a second Memory V2
  // instance or mapping set, just a second reference to this one.
  std::shared_ptr<memory::AddressSpace> memory_{};
  std::unique_ptr<cpu::ExecutableCodeCache> code_cache_{};
  std::unique_ptr<cpu::DynamicFallbackExecutor> dynamic_fallback_{};
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
  CapabilityReportBuilder capability_report_builder_{};

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
