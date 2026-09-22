// xenon-prepare: the out-of-process automatic game preparation worker.
//
// Turns "a content source (disc image / directory / loose XEX) + an
// optionally-installed game module" into a cached, loadable
// xenon_game_module native extension, without ever requiring the end user to
// run the recompiler by hand. See docs/development/GAME_PREPARATION.md for the full
// pipeline, the CLI contract below, and the cache-invalidation rules.
//
// This binary deliberately does the ISO/content-format-aware work itself
// (reading default.xex - and an optional title update - directly out of a
// mounted disc image or directory, with no full-ISO extraction) and hands
// XEX Loader V2 / the Recomp Driver only already-parsed, in-memory XexImage
// data (DriverOptions::pre_parsed_image) - those stay completely unaware of
// where the bytes came from.
//
//   xenon-prepare --content <path> --cache-root <dir>
//                 [--module <hint-package-dir>] [--module-id <id>]
//                 [--title-update <path>] [--config Release|Debug]
//                 [--status-file <path>] [--stop-signal <path>]
//                 [--recomp-root <path>] [--force] [--query]
//
// --query: does not build anything. Prints one line of JSON to stdout and
// exits 0 (or non-zero with {"ok":false,"error":...}):
//   {"ok":true,"needsPreparation":bool,"status":"Fresh"|"Missing"|"Invalid",
//    "cacheKey":"...","titleId":"...","mediaId":"...",
//    "effectiveImageHash":"...","nativeExtensionPath":"..."}
//
// Prepare mode (default): progressively overwrites --status-file (if given)
// with {"phase":...,"percent":...,"task":...,"updatedAtEpochMs":...,
// "cacheKey":...,"titleId":...,"mediaId":...,"effectiveImageHash":...,
// "nativeExtensionPath":...,"error":...}. Creating --stop-signal requests
// cooperative cancellation; the active compiler child (and its full process
// tree) is terminated within a short, bounded time. Exit codes: 0 success
// (including "already Fresh, nothing to do"), 1 usage, 2 content/identity
// error, 3 analysis error, 4 source-generation error, 5 build error,
// 6 validation/commit error, 7 cancelled.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xenon/core/json.hpp"
#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/path.hpp"
#include "xenon/filesystem/read_only_content_device.hpp"
#include "xenon/filesystem/xex_metadata.hpp"
#include "xenon/recomp/analysis_schema_json.hpp"
#include "xenon/recomp/artifact_cache.hpp"
#include "xenon/recomp/driver.hpp"
#include "xenon/recomp/module_hint_provider.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_loader.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using xenon::filesystem::FsError;

// ---------------------------------------------------------------------------
// Small helpers.

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string default_target_arch() {
#if defined(_M_X64) || defined(__x86_64__)
  return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#else
  return "unknown";
#endif
}

bool read_whole_host_file(const std::filesystem::path& path, std::vector<std::byte>& out,
                          std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "unable to open file: " + path.string();
    return false;
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size < 0) {
    error = "unable to determine file size: " + path.string();
    return false;
  }
  out.assign(static_cast<std::size_t>(size), std::byte{0});
  file.seekg(0, std::ios::beg);
  if (!out.empty()) {
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  }
  if (!file && !file.eof()) {
    error = "failed reading file: " + path.string();
    return false;
  }
  return true;
}

// Reads default.xex out of `content`, which may be a directory containing a
// root default.xex, a loose .xex file, or an Xbox 360 disc image
// (.iso/.xgd/.dvd) - resolved and read entirely via GdfxImageSource, never
// extracted to a temporary file (Part 11).
bool load_base_xex_bytes(const std::filesystem::path& content, std::vector<std::byte>& out,
                         std::string& error) {
  std::error_code ec;
  if (std::filesystem::is_directory(content, ec)) {
    for (auto it = std::filesystem::directory_iterator(content, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec) &&
          xenon::filesystem::guest_path_equal(it->path().filename().string(), "default.xex")) {
        return read_whole_host_file(it->path(), out, error);
      }
    }
    error = "no default.xex found in directory: " + content.string();
    return false;
  }

  const auto extension = lower_ascii(content.extension().string());
  if (extension == ".xex") {
    return read_whole_host_file(content, out, error);
  }

  if (extension == ".iso" || extension == ".xgd" || extension == ".dvd") {
    std::filesystem::path image_path;
    if (xenon::filesystem::resolve_gdfx_image_path(content, image_path) != FsError::None) {
      error = "unable to resolve disc image path: " + content.string();
      return false;
    }
    auto source = std::make_shared<xenon::filesystem::GdfxImageSource>(image_path);
    if (source->initialize() != FsError::None) {
      error = "not a valid Xbox 360 disc image: " + image_path.string();
      return false;
    }
    std::vector<xenon::filesystem::DirectoryEntry> entries;
    if (source->list({}, entries) != FsError::None) {
      error = "unable to list disc image root directory: " + image_path.string();
      return false;
    }
    std::string found;
    for (const auto& entry : entries) {
      if (!entry.info.is_directory &&
          xenon::filesystem::guest_path_equal(entry.name, "default.xex")) {
        found = entry.name;
        break;
      }
    }
    if (found.empty()) {
      error = "disc image does not contain a root default.xex: " + image_path.string();
      return false;
    }
    if (xenon::filesystem::read_all(*source, found, out) != FsError::None) {
      error = "failed to read default.xex from disc image: " + image_path.string();
      return false;
    }
    return true;
  }

  error = "unrecognized content source (expected a directory, .xex, .iso, .xgd, or .dvd): " +
          content.string();
  return false;
}

std::uint64_t hash_hint_set(const xenon::recomp::analysis::AnalysisHintSetV2& hint_set) {
  const auto json_text = xenon::recomp::analysis::to_json(hint_set).dump();
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(json_text.data()),
                                         json_text.size());
  const auto digest = xenon::xbox::crypto::sha1(bytes);
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8 && i < digest.size(); ++i) {
    value = (value << 8) | std::to_integer<std::uint64_t>(digest[i]);
  }
  return value;
}

std::optional<std::filesystem::path> find_built_module(const std::filesystem::path& build_dir,
                                                        const std::string& config) {
  const std::vector<std::filesystem::path> candidates = {
      build_dir / config / "xenon_game_module.dll",
      build_dir / "xenon_game_module.dll",
      build_dir / config / "libxenon_game_module.so",
      build_dir / "libxenon_game_module.so",
      build_dir / config / "xenon_game_module.so",
      build_dir / "xenon_game_module.so",
      build_dir / config / "libxenon_game_module.dylib",
      build_dir / "libxenon_game_module.dylib",
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
  }
  return std::nullopt;
}

std::string json_string_or_empty(const std::filesystem::path& path) {
  return path.empty() ? std::string() : path.string();
}

// ---------------------------------------------------------------------------
// Structured progress reporting (Part 13/20). Mirrors runtime_host's
// StatusWriter atomic-publish idiom (temp file + rename) without introducing
// a link dependency on runtime_host (this is a different executable target).

enum class Phase {
  Inspecting,
  ApplyingTitleUpdate,
  AnalyzingExecutable,
  GeneratingSource,
  Compiling,
  Validating,
  Complete,
  Failed,
  Cancelled,
};

const char* phase_name(Phase phase) noexcept {
  switch (phase) {
    case Phase::Inspecting: return "Inspecting";
    case Phase::ApplyingTitleUpdate: return "ApplyingTitleUpdate";
    case Phase::AnalyzingExecutable: return "AnalyzingExecutable";
    case Phase::GeneratingSource: return "GeneratingSource";
    case Phase::Compiling: return "Compiling";
    case Phase::Validating: return "Validating";
    case Phase::Complete: return "Complete";
    case Phase::Failed: return "Failed";
    case Phase::Cancelled: return "Cancelled";
  }
  return "Unknown";
}

class StatusReporter {
 public:
  explicit StatusReporter(std::filesystem::path status_file) : status_file_(std::move(status_file)) {}

  void set_identity(std::string title_id, std::string media_id, std::string effective_image_hash) {
    title_id_ = std::move(title_id);
    media_id_ = std::move(media_id);
    effective_image_hash_ = std::move(effective_image_hash);
  }
  void set_cache_key(std::string digest) { cache_key_ = std::move(digest); }
  void set_native_extension_path(std::string path) { native_extension_path_ = std::move(path); }

  void report(Phase phase, int percent, const std::string& task, const std::string& error = {}) {
    if (status_file_.empty()) return;
    xenon::core::JsonValue root = xenon::core::JsonValue::make_object();
    root.set("phase", std::string(phase_name(phase)));
    root.set("percent", static_cast<double>(percent));
    root.set("task", task);
    root.set("updatedAtEpochMs", static_cast<double>(now_epoch_ms()));
    if (!cache_key_.empty()) root.set("cacheKey", cache_key_);
    if (!title_id_.empty()) root.set("titleId", title_id_);
    if (!media_id_.empty()) root.set("mediaId", media_id_);
    if (!effective_image_hash_.empty()) root.set("effectiveImageHash", effective_image_hash_);
    if (!native_extension_path_.empty()) root.set("nativeExtensionPath", native_extension_path_);
    if (!error.empty()) root.set("error", error);

    std::error_code ec;
    if (status_file_.has_parent_path()) {
      std::filesystem::create_directories(status_file_.parent_path(), ec);
    }
    const auto temp_path = status_file_.string() + ".tmp";
    {
      std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
      out << root.dump();
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, status_file_, rename_error);
  }

 private:
  std::filesystem::path status_file_;
  std::string title_id_;
  std::string media_id_;
  std::string effective_image_hash_;
  std::string cache_key_;
  std::string native_extension_path_;
};

bool cancellation_requested(const std::filesystem::path& stop_signal) {
  if (stop_signal.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(stop_signal, ec);
}

// ---------------------------------------------------------------------------
// Cancellable child-process execution (Part 19). A plain std::system() call
// cannot be interrupted, and terminating only the immediate `cmake --build`
// process leaves the actual compiler/linker descendants it spawns running -
// so cancellation here always targets the whole process tree: a Windows Job
// Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, or a POSIX process group
// signalled as a unit.

bool run_cancellable_command(const std::vector<std::string>& args,
                             const std::filesystem::path& stop_signal, std::string& error,
                             bool& cancelled) {
  cancelled = false;
  error.clear();

#if defined(_WIN32)
  std::wstring command_line;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i != 0) command_line += L' ';
    command_line += L'"';
    command_line += std::wstring(args[i].begin(), args[i].end());
    command_line += L'"';
  }

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                      CREATE_SUSPENDED, nullptr, nullptr, &startup_info,
                                      &process_info);
  if (!created) {
    error = "failed to start process (CreateProcessW error " +
            std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    if (job != nullptr) CloseHandle(job);
    return false;
  }
  if (job != nullptr) AssignProcessToJobObject(job, process_info.hProcess);
  ResumeThread(process_info.hThread);

  int exit_code = 1;
  for (;;) {
    const auto wait_result = WaitForSingleObject(process_info.hProcess, 250);
    if (wait_result == WAIT_OBJECT_0) {
      DWORD code = 1;
      GetExitCodeProcess(process_info.hProcess, &code);
      exit_code = static_cast<int>(code);
      break;
    }
    if (cancellation_requested(stop_signal)) {
      cancelled = true;
      if (job != nullptr) {
        TerminateJobObject(job, 1);
      } else {
        TerminateProcess(process_info.hProcess, 1);
      }
      WaitForSingleObject(process_info.hProcess, 5000);
      break;
    }
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  if (job != nullptr) CloseHandle(job);

  if (cancelled) {
    error = "cancelled";
    return false;
  }
  if (exit_code != 0) {
    error = "process exited with code " + std::to_string(exit_code);
    return false;
  }
  return true;
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    error = "fork() failed";
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  setpgid(pid, pid);  // best-effort; avoids a race with the child's own setpgid

  int exit_code = 1;
  for (;;) {
    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
      break;
    }
    if (cancellation_requested(stop_signal)) {
      cancelled = true;
      kill(-pid, SIGTERM);
      bool exited = false;
      for (int i = 0; i < 20 && !exited; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        exited = waitpid(pid, &status, WNOHANG) == pid;
      }
      if (!exited) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (cancelled) {
    error = "cancelled";
    return false;
  }
  if (exit_code != 0) {
    error = "process exited with code " + std::to_string(exit_code);
    return false;
  }
  return true;
#endif
}

// ---------------------------------------------------------------------------
// Argument parsing.

struct Options {
  std::filesystem::path content;
  std::filesystem::path title_update;
  std::filesystem::path module_dir;
  std::string module_id;
  std::filesystem::path cache_root;
  std::filesystem::path status_file;
  std::filesystem::path stop_signal;
  std::filesystem::path recomp_root;
  std::string config{"Release"};
  bool force{false};
  bool query{false};
};

void print_usage() {
  std::cout <<
      "usage: xenon-prepare --content <path> --cache-root <dir>\n"
      "                     [--module <hint-package-dir>] [--module-id <id>]\n"
      "                     [--title-update <path>] [--config Release|Debug]\n"
      "                     [--status-file <path>] [--stop-signal <path>]\n"
      "                     [--recomp-root <path>] [--force] [--query]\n";
}

bool parse_args(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (arg == "--content") options.content = next();
    else if (arg == "--title-update") options.title_update = next();
    else if (arg == "--module") options.module_dir = next();
    else if (arg == "--module-id") options.module_id = next();
    else if (arg == "--cache-root") options.cache_root = next();
    else if (arg == "--status-file") options.status_file = next();
    else if (arg == "--stop-signal") options.stop_signal = next();
    else if (arg == "--recomp-root") options.recomp_root = next();
    else if (arg == "--config") options.config = next();
    else if (arg == "--force") options.force = true;
    else if (arg == "--query") options.query = true;
    else if (arg == "--help" || arg == "-h") { print_usage(); std::exit(0); }
    else { error = "unknown argument: " + arg; return false; }
  }
  if (options.content.empty() || options.cache_root.empty()) {
    error = "--content and --cache-root are required";
    return false;
  }
#if defined(XENON_SOURCE_ROOT)
  if (options.recomp_root.empty()) options.recomp_root = XENON_SOURCE_ROOT;
#endif
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_args(argc, argv, options, error)) {
    std::cerr << "xenon-prepare: " << error << "\n";
    print_usage();
    return 1;
  }

  try {
    StatusReporter status(options.status_file);

    // --- Phase: Inspecting -------------------------------------------------
    status.report(Phase::Inspecting, 0, "Reading executable identity from content source");
    std::vector<std::byte> base_bytes;
    if (!load_base_xex_bytes(options.content, base_bytes, error)) {
      status.report(Phase::Failed, 0, "Inspecting game", error);
      std::cerr << "xenon-prepare: " << error << "\n";
      return 2;
    }

    xenon::xbox::XexImage base_image;
    if (!xenon::xbox::parse_xex_image(base_bytes, base_image, &error)) {
      const auto message = "default.xex is not a valid, supported XEX: " + error;
      status.report(Phase::Failed, 0, "Inspecting game", message);
      std::cerr << "xenon-prepare: " << message << "\n";
      return 2;
    }

    xenon::xbox::XexImage effective_image = base_image;
    bool title_update_applied = false;
    if (!options.title_update.empty()) {
      status.report(Phase::ApplyingTitleUpdate, 5, "Applying selected title update");
      std::vector<std::byte> update_bytes;
      if (!read_whole_host_file(options.title_update, update_bytes, error)) {
        status.report(Phase::Failed, 5, "Applying title update", error);
        std::cerr << "xenon-prepare: " << error << "\n";
        return 2;
      }
      xenon::xbox::XexImage patched;
      if (!xenon::xbox::apply_title_update(base_image, update_bytes, patched, &error)) {
        const auto message = "title update could not be applied: " + error;
        status.report(Phase::Failed, 5, "Applying title update", message);
        std::cerr << "xenon-prepare: " << message << "\n";
        return 2;
      }
      effective_image = std::move(patched);
      title_update_applied = true;
    }

    const auto identity = xenon::xbox::compute_effective_identity(
        base_image, title_update_applied ? &effective_image : nullptr);
    const auto title_id_hex = xenon::filesystem::format_xbox_id(identity.title_id);
    const auto media_id_hex = xenon::filesystem::format_xbox_id(identity.media_id);
    const auto effective_hash_hex =
        xenon::xbox::format_effective_image_hash(identity.effective_image_hash);
    status.set_identity(title_id_hex, media_id_hex, effective_hash_hex);

    // --- Resolve the (optional) module hint package -------------------------
    std::unique_ptr<xenon::recomp::FileModuleHintProvider> hint_provider;
    std::optional<xenon::recomp::analysis::AnalysisHintSetV2> hint_set;
    std::string module_id = options.module_id;
    std::string module_compatibility_version = "1";
    std::uint64_t hint_set_hash = 0;

    if (!options.module_dir.empty()) {
      hint_provider = std::make_unique<xenon::recomp::FileModuleHintProvider>(options.module_dir);
      if (!hint_provider->manifest_loaded()) {
        const auto message = "module '" + options.module_dir.string() + "': " + hint_provider->manifest_error();
        status.report(Phase::Failed, 5, "Resolving module", message);
        std::cerr << "xenon-prepare: " << message << "\n";
        return 2;
      }
      if (module_id.empty()) module_id = hint_provider->module_name();
      module_compatibility_version = hint_provider->compatibility_version();

      xenon::recomp::analysis::AnalysisHintSetV2 resolved{};
      std::string hint_error;
      if (!hint_provider->provide(identity, resolved, hint_error)) {
        status.report(Phase::Failed, 5, "Resolving module", hint_error);
        std::cerr << "xenon-prepare: " << hint_error << "\n";
        return 2;
      }
      hint_set_hash = hash_hint_set(resolved);
      hint_set = std::move(resolved);
    }

    xenon::recomp::ArtifactCacheKey key;
    key.title_id = identity.title_id;
    key.media_id = identity.media_id;
    key.effective_image_hash = effective_hash_hex;
    key.module_id = module_id;
    key.module_compatibility_version = module_compatibility_version;
    key.hint_set_hash = hint_set_hash;
    key.target_arch = default_target_arch();
    key.build_config = options.config;
    status.set_cache_key(key.digest());

    xenon::recomp::ArtifactCacheStore store(options.cache_root);
    store.clean_stale_staging();
    const auto existing = store.lookup(key);

    if (options.query) {
      xenon::core::JsonValue root = xenon::core::JsonValue::make_object();
      root.set("ok", true);
      root.set("needsPreparation", existing.status != xenon::recomp::ArtifactCacheStatus::Fresh);
      root.set("status", existing.status == xenon::recomp::ArtifactCacheStatus::Fresh ? "Fresh"
                        : existing.status == xenon::recomp::ArtifactCacheStatus::Invalid ? "Invalid"
                                                                                          : "Missing");
      root.set("cacheKey", key.digest());
      root.set("titleId", title_id_hex);
      root.set("mediaId", media_id_hex);
      root.set("effectiveImageHash", effective_hash_hex);
      root.set("nativeExtensionPath", json_string_or_empty(existing.native_extension_path));
      std::cout << root.dump() << "\n";
      return 0;
    }

    if (!options.force && existing.status == xenon::recomp::ArtifactCacheStatus::Fresh) {
      status.set_native_extension_path(existing.native_extension_path.string());
      status.report(Phase::Complete, 100, "Already prepared - launching cached module");
      std::cout << "xenon-prepare: already prepared: " << existing.native_extension_path << "\n";
      return 0;
    }

    if (cancellation_requested(options.stop_signal)) {
      status.report(Phase::Cancelled, 0, "Cancelled before analysis began");
      return 7;
    }

    // --- Phase: AnalyzingExecutable ------------------------------------------
    status.report(Phase::AnalyzingExecutable, 15,
                  "Discovering functions and applying analysis hints");
    xenon::recomp::DriverOptions driver_options;
    driver_options.pre_parsed_image = effective_image;
    driver_options.hint_set_v2 = hint_set;
    // Progress milestones (Part 18 of the Recomp Analysis V2 pass): a large
    // real-title analysis used to leave this tool's status file (and so any
    // UI reading it, e.g. the launcher) frozen at "15%" for the entire
    // discovery/compile pass with no visible movement. Forwarding the
    // driver's own progress strings as the task text - at the phase's
    // existing percent, which analysis/codegen alone do not otherwise
    // subdivide - at least proves forward progress is happening and shows
    // live function/wave counts, worker counts, etc.
    driver_options.progress = [&status](const std::string& message) {
      status.report(Phase::AnalyzingExecutable, 15, message);
    };

    xenon::recomp::AnalysisReport report;
    std::string driver_error;
    if (!xenon::recomp::load_and_analyze(driver_options, report, driver_error)) {
      status.report(Phase::Failed, 15, "Analyzing executable", driver_error);
      std::cerr << "xenon-prepare: " << driver_error << "\n";
      return 3;
    }

    if (cancellation_requested(options.stop_signal)) {
      status.report(Phase::Cancelled, 15, "Cancelled after analysis");
      return 7;
    }

    // --- Phase: GeneratingSource ---------------------------------------------
    status.report(Phase::GeneratingSource, 40, "Generating native C++ source");
    driver_options.progress = [&status](const std::string& message) {
      status.report(Phase::GeneratingSource, 40, message);
    };
    auto staging = store.begin_staging(key);
    driver_options.output = staging.directory() / "generated";
    if (!xenon::recomp::generate_project(driver_options, report, driver_error)) {
      staging.discard();
      status.report(Phase::Failed, 40, "Generating native source", driver_error);
      std::cerr << "xenon-prepare: " << driver_error << "\n";
      return 4;
    }

    if (cancellation_requested(options.stop_signal)) {
      staging.discard();
      status.report(Phase::Cancelled, 40, "Cancelled after source generation");
      return 7;
    }

    if (options.recomp_root.empty()) {
      staging.discard();
      const auto message =
          "no Xenon-Recomp source root available (pass --recomp-root explicitly)";
      status.report(Phase::Failed, 40, "Configuring native build", message);
      std::cerr << "xenon-prepare: " << message << "\n";
      return 5;
    }

    // --- Phase: Compiling -----------------------------------------------------
    const auto build_dir = staging.directory() / "build";
    status.report(Phase::Compiling, 55, "Configuring native build");
    const std::vector<std::string> configure_args = {
        "cmake", "-S", driver_options.output.string(), "-B", build_dir.string(),
        "-DXENON_RECOMP_ROOT=" + options.recomp_root.string(),
        "-DCMAKE_BUILD_TYPE=" + options.config};
    bool cancelled = false;
    if (!run_cancellable_command(configure_args, options.stop_signal, error, cancelled)) {
      staging.discard();
      status.report(cancelled ? Phase::Cancelled : Phase::Failed, 55,
                    "Configuring native build", cancelled ? std::string() : error);
      if (cancelled) return 7;
      std::cerr << "xenon-prepare: configure failed: " << error << "\n";
      return 5;
    }

    status.report(Phase::Compiling, 70, "Compiling and linking native code");
    const std::vector<std::string> build_args = {
        "cmake", "--build", build_dir.string(), "--target", "xenon_game_module",
        "--config", options.config};
    if (!run_cancellable_command(build_args, options.stop_signal, error, cancelled)) {
      staging.discard();
      status.report(cancelled ? Phase::Cancelled : Phase::Failed, 70,
                    "Compiling native code", cancelled ? std::string() : error);
      if (cancelled) return 7;
      std::cerr << "xenon-prepare: build failed: " << error << "\n";
      return 5;
    }

    // --- Phase: Validating ------------------------------------------------------
    status.report(Phase::Validating, 90, "Validating compiled module");
    const auto built_module = find_built_module(build_dir, options.config);
    if (!built_module.has_value()) {
      staging.discard();
      const auto message = "native build succeeded but produced no locatable xenon_game_module "
                           "shared library under " + build_dir.string();
      status.report(Phase::Failed, 90, "Validating module", message);
      std::cerr << "xenon-prepare: " << message << "\n";
      return 6;
    }

#if defined(_WIN32)
    const std::string native_extension_name = "xenon_game_module.dll";
#elif defined(__APPLE__)
    const std::string native_extension_name = "xenon_game_module.dylib";
#else
    const std::string native_extension_name = "xenon_game_module.so";
#endif
    std::error_code copy_ec;
    std::filesystem::copy_file(*built_module, staging.directory() / native_extension_name,
                               std::filesystem::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
      staging.discard();
      const auto message = "failed to stage the compiled module: " + copy_ec.message();
      status.report(Phase::Failed, 90, "Validating module", message);
      std::cerr << "xenon-prepare: " << message << "\n";
      return 6;
    }

    xenon::recomp::ArtifactCacheEntry final_entry;
    std::string commit_error;
    if (!staging.commit(native_extension_name, final_entry, commit_error)) {
      status.report(Phase::Failed, 90, "Validating module", commit_error);
      std::cerr << "xenon-prepare: " << commit_error << "\n";
      return 6;
    }

    status.set_native_extension_path(final_entry.native_extension_path.string());
    status.report(Phase::Complete, 100, "Preparation complete");
    std::cout << "xenon-prepare: prepared " << final_entry.native_extension_path << "\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "xenon-prepare: unexpected error: " << exception.what() << "\n";
    return 2;
  }
}
