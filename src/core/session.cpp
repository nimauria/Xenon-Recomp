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
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "xenon/xam/content_graph.hpp"
#include "xenon/xbox/xboxkrnl_rtl_exports.hpp"

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
  {
    std::lock_guard<std::mutex> observation_lock(adaptive_observation_mutex_);
    adaptive_observation_seen_.clear();
    dynamic_fallback_observation_seen_.clear();
  }

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
  if (main_thread_) {
    static_cast<void>(main_thread_->join());
  }

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
  unload_native_extension();

  // Shutdown in reverse order of initialization
  xam_.reset();

  input_bridge_.reset();
  if (input_) {
    input_->shutdown();
    input_.reset();
  }

  gpu_.reset();
  dynamic_fallback_.reset();
  code_cache_.reset();

  // Tear down the guest process/thread model before the memory it lives in.
  // main_thread_ was already joined above; resetting kernel_process_ (and
  // with it its ThreadManager) is then safe/idempotent.
  main_thread_.reset();
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
      std::cout << "[XenonSession] Guest exception 0x" << std::hex
                << static_cast<std::uint32_t>(record.code) << " at 0x" << record.address
                << std::dec << std::endl;
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
  
  // Register xboxkrnl RTL exports (RtlImageXexHeaderField, etc.)
  if (!xbox::register_xboxkrnl_rtl_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl RTL exports");
    return false;
  }

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
  kernel_process_->set_main_thread(main_thread_);
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
      std::cout << "[XenonSession] Unresolved "
                << (import.is_variable() ? "variable import: " : "import: ")
                << import.module << " '" << import.symbol << "' ordinal "
                << import.ordinal << std::endl;
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

  // Real guest process/thread model (XenonSession -> KernelProcess ->
  // KernelThread -> CPU V2), consuming XEX Loader V2's already-produced
  // output directly rather than reparsing default.xex. kernel_memory_ wraps
  // the same memory_ this session already uses (see session.hpp comment) -
  // no second Memory V2 instance or mapping set.
  kernel_memory_ = std::make_shared<kernel::KernelMemory>(memory_);
  kernel_process_ = std::make_shared<kernel::KernelProcess>(kernel_memory_);

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

std::uint32_t XenonSession::run_execution() {
  // Establishes this host thread as the active guest thread for anything
  // that resolves "current thread" via kernel::ThreadManager (thread_local),
  // so kernel/exception context below - and any future kernel export that
  // asks "who am I" - is scoped to the real KernelThread, not inferred.
  if (kernel_process_ && main_thread_) {
    kernel_process_->thread_manager().set_current_thread(main_thread_);
  }

  execution_active_.store(true);
  const auto entry = loaded_xex_->image.entry_point;

  cpu::ExecutionContext context(*main_cpu_state_, *memory_, *this);
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
    execution_active_.store(false);
    set_error("No compiled game code or dynamic fallback is available to execute");
    return 0xFFFFFFFFu;
  }

  auto* entry_fn = context.lookup_compiled(entry, cpu::CompiledLookupKind::Call);

  cpu::ExecutionResult result{};
  bool crashed = false;
  std::string crash_message;
  std::uint32_t crash_exit_code = 0xC0000005u;  // NTSTATUS-style default (access violation).
  const auto fail_execution = [&](std::string message, std::uint32_t exit_code) {
    crashed = true;
    crash_message = std::move(message);
    crash_exit_code = exit_code;
  };
  try {
    if (entry_fn) {
      result = entry_fn(context);
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
    if (!crashed && config_.enable_logging) {
      std::cout << "[XenonSession] Guest entry returned: reason="
                << cpu::flow_reason_name(result.reason)
                << " next=0x" << std::hex << std::uppercase << result.next_address
                << " detail=0x" << result.detail
                << " cia=0x" << main_cpu_state_->cia
                << " nia=0x" << main_cpu_state_->nia
                << " lr=0x" << main_cpu_state_->lr
                << " ctr=0x" << main_cpu_state_->ctr
                << " r1=0x" << main_cpu_state_->gpr[1]
                << " r3=0x" << main_cpu_state_->gpr[3] << std::dec << std::endl;
    }

    constexpr std::uint32_t kMaxTopLevelDispatches = 1'000'000u;
    for (std::uint32_t dispatch_count = 0u;
         !crashed && !stop_requested_.load() &&
         dispatch_count < kMaxTopLevelDispatches;) {
      switch (result.reason) {
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
          if (config_.enable_logging) {
            std::cout << "[XenonSession] Guest dispatch returned: reason="
                      << cpu::flow_reason_name(result.reason)
                      << " next=0x" << std::hex << std::uppercase
                      << result.next_address << " detail=0x" << result.detail
                      << " cia=0x" << main_cpu_state_->cia
                      << " nia=0x" << main_cpu_state_->nia
                      << " lr=0x" << main_cpu_state_->lr
                      << " ctr=0x" << main_cpu_state_->ctr
                      << " r1=0x" << main_cpu_state_->gpr[1]
                      << " r3=0x" << main_cpu_state_->gpr[3] << std::dec
                      << std::endl;
          }
          continue;
        }
        case cpu::FlowReason::Return:
          if (result.next_address != 0u || main_cpu_state_->lr != 0u) {
            std::ostringstream diagnostic;
            diagnostic << "Guest entry returned from a non-terminal compiled "
                       << "boundary (next=0x" << std::hex << std::uppercase
                       << result.next_address << ", lr=0x" << main_cpu_state_->lr
                       << ')';
            fail_execution(diagnostic.str(), 0xC000001Du);
          }
          break;
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
    if (!crashed && !stop_requested_.load() &&
        result.reason == cpu::FlowReason::Branch) {
      fail_execution("Guest execution exceeded the top-level dispatch limit",
                     0xC000001Du);
    }
  } catch (const memory::MemoryFault& fault) {
    // Real connection to the guest exception path (not a new subsystem):
    // Memory V2 already throws this on a genuine guest memory fault; route
    // it through kernel::ExceptionDispatcher, scoped to the thread that just
    // registered itself above. Also retain the guest CPU/memory state that
    // caused the first fault - "fault at 0" alone cannot distinguish a null
    // data dereference from an indirect call, bad ABI state, or bad mapping.
    crashed = true;
    const auto& info = fault.info();
    const auto record = kernel::ExceptionDispatcher::fault_to_exception(info);
    crash_exit_code = static_cast<std::uint32_t>(record.code);
    static_cast<void>(exception_dispatcher_.dispatch_exception(record));

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
    const auto page_state_name = [](memory::PageState state) {
      switch (state) {
        case memory::PageState::Free: return "free";
        case memory::PageState::Reserved: return "reserved";
        case memory::PageState::Committed: return "committed";
      }
      return "unknown";
    };
    const auto protect_string = [](memory::Protect protect) {
      std::string result;
      result += memory::has(protect, memory::Protect::Read) ? 'R' : '-';
      result += memory::has(protect, memory::Protect::Write) ? 'W' : '-';
      result += memory::has(protect, memory::Protect::Execute) ? 'X' : '-';
      if (memory::has(protect, memory::Protect::NoCache)) result += "|NC";
      if (memory::has(protect, memory::Protect::WriteCombine)) result += "|WC";
      return result;
    };

    std::ostringstream diagnostic;
    diagnostic << "Guest memory fault: " << fault.what()
               << " [cia=0x" << std::hex << std::uppercase << main_cpu_state_->cia
               << " nia=0x" << main_cpu_state_->nia
               << " lr=0x" << main_cpu_state_->lr
               << " ctr=0x" << main_cpu_state_->ctr
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
               << " r1=0x" << main_cpu_state_->gpr[1]
               << " r2=0x" << main_cpu_state_->gpr[2]
               << " r3=0x" << main_cpu_state_->gpr[3]
               << " r4=0x" << main_cpu_state_->gpr[4]
               << " r5=0x" << main_cpu_state_->gpr[5]
               << " r6=0x" << main_cpu_state_->gpr[6]
               << " r7=0x" << main_cpu_state_->gpr[7]
               << " r8=0x" << main_cpu_state_->gpr[8]
               << " r9=0x" << main_cpu_state_->gpr[9]
               << " r10=0x" << main_cpu_state_->gpr[10]
               << " r13=0x" << main_cpu_state_->gpr[13] << ']';
    crash_message = diagnostic.str();
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
    return crash_exit_code;
  }
  if (stop_requested_.load()) {
    set_state(SessionState::Stopped, "Game execution ended (stop requested)");
    return 0;
  }
  if (result.reason == cpu::FlowReason::Trap) {
    set_error("Game execution trapped (code " + std::to_string(result.detail) + ")");
    static_cast<void>(exception_dispatcher_.dispatch_exception(
        kernel::ExceptionRecord{kernel::ExceptionCode::IllegalInstruction, 0,
                                static_cast<std::uint32_t>(main_cpu_state_->cia), {}}));
    return result.detail;
  }
  set_state(SessionState::Stopped, "Game execution completed");
  return 0;
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

  cpu::ExecutionContext context(state, *memory_, *this);
  if (dynamic_fallback_) dynamic_fallback_->bind(context);
  compiled_registry_binder_(context);
  if (!context.compiled_lookup) return false;
  auto* fn = context.lookup_compiled(callback, cpu::CompiledLookupKind::Call);

  try {
    cpu::ExecutionResult result{};
    if (fn) {
      result = fn(context);
    } else {
      const auto fallback = context.try_dynamic_fallback(
          callback, cpu::CompiledLookupKind::Call);
      if (!fallback.handled) return false;
      result = fallback.result;
    }
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
        std::cout << "[XenonSession] Unresolved import call: " << import.module << "!"
                  << (import.symbol.empty() ? std::to_string(import.ordinal) : import.symbol)
                  << " at 0x" << std::hex << target << std::dec << std::endl;
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
    std::cout << "[XenonSession] Unresolved call target: 0x" << std::hex << target
              << " (neither compiled guest code nor a known import)" << std::dec << std::endl;
  }
  return {cpu::FlowReason::Trap, target,
          static_cast<std::uint32_t>(kernel::ExceptionCode::ProcedureNotFound)};
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
