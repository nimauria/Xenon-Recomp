#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/cpu/executable_code_cache.hpp"
#include "xenon/cpu/external_calls.hpp"
#include "xenon/cpu/runtime.hpp"
#include "xenon/cpu/state.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/gpu/backend.hpp"
#include "xenon/input/system.hpp"
#include "xenon/kernel/io_manager.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xam/xam_session.hpp"
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
  bool enable_audio{false};
  bool enable_network{false};
  memory::GuestTranslationMode memory_mode{memory::GuestTranslationMode::Auto};
  bool enable_dynamic_compilation{false};
  bool enable_logging{true};
  bool enable_export_diagnostics{true};

  // Optional path to a game-specific native extension library ("module" in
  // launcher terms: symbols, patches, and the recomp-driver-generated
  // compiled-code registry for one title). See docs/RUNTIME_HOST.md for the
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
  
  // Load game with optional content graph
  [[nodiscard]] SessionResult load_game(std::span<const std::byte> xex_bytes,
                                       std::string_view game_id = {});
  
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
  void set_state(SessionState new_state, std::string message = {});
  void set_error(std::string error);
  // Guest execution thread body: builds a CPU V2 ExecutionContext, binds the
  // native extension's compiled registry into it, looks up the entry point,
  // and invokes it. Runs entirely on execution_thread_.
  void run_execution();
#if defined(XENON_HAS_AUDIO)
  [[nodiscard]] bool invoke_audio_callback(cpu::GuestAddress callback,
                                           cpu::GuestAddress argument);
#endif
  void load_native_extension();
  void unload_native_extension() noexcept;

  SessionConfig config_{};
  mutable std::mutex status_mutex_{};
  SessionState state_{SessionState::Uninitialized};
  std::string last_error_{};
  std::unique_ptr<memory::AddressSpace> memory_{};
  std::unique_ptr<cpu::ExecutableCodeCache> code_cache_{};
  // shared_ptr: kernel::KernelIoManager takes shared ownership of the VFS.
  std::shared_ptr<filesystem::VirtualFileSystem> filesystem_{};
  std::unique_ptr<kernel::KernelIoManager> kernel_io_{};
  std::unique_ptr<input::InputSystem> input_{};
  std::unique_ptr<gpu::Backend> gpu_{};
#if defined(XENON_HAS_AUDIO)
  std::unique_ptr<audio::AudioSystem> audio_{};
#endif
  std::unique_ptr<xam::XamSession> xam_{};
  ExportRegistry export_registry_{};
  cpu::ExternalCallRegistry legacy_call_registry_{};
  std::optional<xbox::LoadedXex> loaded_xex_{};
  std::unique_ptr<xam::ContentGraph> content_graph_{};
  std::string game_id_{};
  std::unique_ptr<cpu::CpuState> main_cpu_state_{};
  std::uint64_t time_base_counter_{0};

  // Native extension (module) dynamic loading. See docs/RUNTIME_HOST.md for
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
#endif

  std::thread execution_thread_{};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> execution_active_{false};
};

}  // namespace xenon::core
