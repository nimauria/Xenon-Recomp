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
// Gen 11 (Autonomous Game Intake, see xenon/recomp/game_intake.hpp and
// docs/recomp/GAME_INTAKE_GEN11.md): the content source is first scanned for
// EVERY executable XEX module it contains, not only the mandatory root
// default.xex. A title with exactly one XEX (the overwhelming majority) is
// prepared exactly as before. A title with additional discovered XEX modules
// has every one of them independently analyzed/compiled/cached, and the
// outcome of all of them is recorded in a deterministic
// <cache-root>/game-compilation-graph.json manifest; a secondary module's
// failure does not prevent the primary/playable module from being ready.
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
#include <map>
#include <stdexcept>
#include "xenon/recomp/worker_pool.hpp"
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xenon/core/json.hpp"
#include "xenon/filesystem/xex_metadata.hpp"
#include "xenon/recomp/analysis_schema_json.hpp"
#include "xenon/recomp/artifact_cache.hpp"
#include "xenon/recomp/driver.hpp"
#include "xenon/recomp/game_intake.hpp"
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

struct PreparationWorkspace {
  std::filesystem::path path;
  explicit PreparationWorkspace(std::filesystem::path p) : path(std::move(p)) {
    std::filesystem::create_directories(path);
    if (!std::filesystem::create_directory(path / ".lock"))
      throw std::runtime_error("preparation workspace is busy (or has an abandoned .lock): " + path.string());
  }
  ~PreparationWorkspace() { std::error_code ec; std::filesystem::remove(path / ".lock", ec); }
};
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
  std::filesystem::path observations;
  std::filesystem::path knowledge;
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
      "                     [--observations <adaptive-observations.jsonl>]\n"
      "                     [--knowledge <knowledge.jsonl>]\n"
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
    else if (arg == "--observations") options.observations = next();
    else if (arg == "--knowledge") options.knowledge = next();
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

// ---------------------------------------------------------------------------
// Analyze + generate + compile + commit one already-identified XEX image
// (Analyzing/GeneratingSource/Compiling/Validating). Shared verbatim between
// the single-module fast path and the Gen 11 multi-module loop below, so the
// actual expensive/risky compilation logic has exactly one implementation.

struct ModuleBuildResult {
  bool ok{};
  int exit_code{};  // meaningful only when !ok: 3/4/5/6/7 matching the CLI contract
  xenon::recomp::ArtifactCacheEntry entry;
  std::string error;
};

ModuleBuildResult prepare_one_module(const xenon::xbox::XexImage& effective_image,
                                     const std::optional<xenon::recomp::analysis::AnalysisHintSetV2>& hint_set,
                                     const std::vector<xenon::recomp::AdaptiveObservation>& adaptive_observations,
                                     const std::vector<xenon::recomp::KnowledgeRecord>& knowledge_records,
                                     const xenon::recomp::ArtifactCacheKey& key,
                                     xenon::recomp::ArtifactCacheStore& store, const Options& options,
                                     const std::string& title_id_hex, const std::string& media_id_hex,
                                     const std::string& module_relative_path, bool is_primary,
                                     StatusReporter& status) {
  ModuleBuildResult result;

  if (cancellation_requested(options.stop_signal)) {
    status.report(Phase::Cancelled, 0, "Cancelled before analysis began");
    result.exit_code = 7;
    result.error = "cancelled";
    return result;
  }

  // --- Phase: AnalyzingExecutable ------------------------------------------
  status.report(Phase::AnalyzingExecutable, 15,
                is_primary ? "Discovering functions and applying analysis hints"
                           : "Discovering functions and applying analysis hints (" +
                                 module_relative_path + ")");
  xenon::recomp::DriverOptions driver_options;
  driver_options.graph_cache = options.cache_root / "graph-v1";
  driver_options.shard_function_count = 1;
  driver_options.pre_parsed_image = effective_image;
  driver_options.hint_set_v2 = hint_set;
  driver_options.adaptive_observations = adaptive_observations;
  driver_options.knowledge_records = knowledge_records;
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
    result.exit_code = 3;
    result.error = driver_error;
    return result;
  }

  if (cancellation_requested(options.stop_signal)) {
    status.report(Phase::Cancelled, 15, "Cancelled after analysis");
    result.exit_code = 7;
    result.error = "cancelled";
    return result;
  }

  // --- Phase: GeneratingSource ---------------------------------------------
  status.report(Phase::GeneratingSource, 40, "Generating native C++ source");
  driver_options.progress = [&status](const std::string& message) {
    status.report(Phase::GeneratingSource, 40, message);
  };
  auto staging = store.begin_staging(key);
  // Title/revision is not an object-cache identity. Workspace is merely the
  // stable CMake build location, exclusively held until publication ends.
  // Gen 11: the relative path is part of this key too, since two different
  // XEX modules of the same title each need their own independent nested
  // CMake build/generated project, never sharing one workspace directory.
  const auto workspace_key = xenon::recomp::graph::digest(
      title_id_hex + ":" + media_id_hex + ":" + module_relative_path + ":" + options.config + ":" +
      options.recomp_root.string());
  PreparationWorkspace workspace(options.cache_root / "workspaces" / workspace_key);
  driver_options.output = workspace.path / "generated";
  if (!xenon::recomp::generate_project(driver_options, report, driver_error)) {
    staging.discard();
    status.report(Phase::Failed, 40, "Generating native source", driver_error);
    result.exit_code = 4;
    result.error = driver_error;
    return result;
  }

  if (cancellation_requested(options.stop_signal)) {
    staging.discard();
    status.report(Phase::Cancelled, 40, "Cancelled after source generation");
    result.exit_code = 7;
    result.error = "cancelled";
    return result;
  }

  // --- Phase: Compiling -----------------------------------------------------
  const auto build_dir = workspace.path / "build";
  status.report(Phase::Compiling, 55, "Configuring native build");
  const std::vector<std::string> configure_args = {
      XENON_CMAKE_COMMAND, "-S", driver_options.output.string(), "-B", build_dir.string(), "-G", "Ninja",
      "-DCMAKE_CXX_COMPILER=" + std::string(XENON_NATIVE_COMPILER),
      "-DXENON_GRAPH_CACHE=" + (driver_options.graph_cache / "native").string(),
      "-DXENON_RECOMP_ROOT=" + options.recomp_root.string(),
      "-DCMAKE_BUILD_TYPE=" + options.config};
  bool cancelled = false;
  std::string command_error;
  if (!run_cancellable_command(configure_args, options.stop_signal, command_error, cancelled)) {
    staging.discard();
    status.report(cancelled ? Phase::Cancelled : Phase::Failed, 55,
                  "Configuring native build", cancelled ? std::string() : command_error);
    result.exit_code = cancelled ? 7 : 5;
    result.error = cancelled ? "cancelled" : command_error;
    return result;
  }

  status.report(Phase::Compiling, 70, "Compiling and linking native code");
  const std::vector<std::string> build_args = {
      XENON_CMAKE_COMMAND, "--build", build_dir.string(), "--target", "xenon_game_module",
      "--parallel", std::to_string(std::min<std::size_t>(8, xenon::recomp::resolve_worker_count({}))),
      "--config", options.config};
  if (!run_cancellable_command(build_args, options.stop_signal, command_error, cancelled)) {
    staging.discard();
    status.report(cancelled ? Phase::Cancelled : Phase::Failed, 70,
                  "Compiling native code", cancelled ? std::string() : command_error);
    result.exit_code = cancelled ? 7 : 5;
    result.error = cancelled ? "cancelled" : command_error;
    return result;
  }

  // --- Phase: Validating ------------------------------------------------------
  status.report(Phase::Validating, 90, "Validating compiled module");
  const auto built_module = find_built_module(build_dir, options.config);
  if (!built_module.has_value()) {
    staging.discard();
    const auto message = "native build succeeded but produced no locatable xenon_game_module "
                         "shared library under " + build_dir.string();
    status.report(Phase::Failed, 90, "Validating module", message);
    result.exit_code = 6;
    result.error = message;
    return result;
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
    result.exit_code = 6;
    result.error = message;
    return result;
  }

  xenon::recomp::ArtifactCacheEntry final_entry;
  std::string commit_error;
  if (!staging.commit(native_extension_name, final_entry, commit_error)) {
    status.report(Phase::Failed, 90, "Validating module", commit_error);
    result.exit_code = 6;
    result.error = commit_error;
    return result;
  }

  if (is_primary) {
    status.set_native_extension_path(final_entry.native_extension_path.string());
  }
  result.ok = true;
  result.entry = final_entry;
  return result;
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

    // --- Phase: Inspecting (Gen 11: discover every executable XEX module
    // the content source actually contains, not just default.xex) ---------
    status.report(Phase::Inspecting, 0, "Discovering executable modules in content source");
    std::vector<xenon::recomp::DiscoveredExecutable> discovered_modules;
    if (!xenon::recomp::discover_game_executables(options.content, discovered_modules, error)) {
      status.report(Phase::Failed, 0, "Inspecting game", error);
      std::cerr << "xenon-prepare: " << error << "\n";
      return 2;
    }

    xenon::recomp::ArtifactCacheStore store(options.cache_root);

    // ------------------------------------------------------------------
    // Single-module fast path: the overwhelming common case (one root
    // default.xex, no additional discovered executables) runs the exact
    // same sequence of operations xenon-prepare has always run for it, so
    // every existing single-module CLI/status.json/query-JSON contract is
    // unchanged byte-for-byte.
    // ------------------------------------------------------------------
    if (discovered_modules.size() == 1) {
      const auto& module = discovered_modules.front();
      status.report(Phase::Inspecting, 0, "Reading executable identity from content source");
      std::vector<std::byte> base_bytes;
      if (!xenon::recomp::read_game_executable_bytes(options.content, module.relative_path,
                                                      base_bytes, error)) {
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

      // --- Resolve the (optional) module hint package -----------------------
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

      // Runtime-learning feedback is part of preparation identity. The trace
      // is append-only and may not exist before the first launch; absence
      // means an empty fact set. Repeated hits are aggregated by the loader
      // and excluded from the cache fingerprint, so only NEW control-flow
      // facts cause a rebuild on the next Play.
      std::vector<xenon::recomp::AdaptiveObservation> adaptive_observations;
      if (!options.observations.empty()) {
        std::error_code observation_ec;
        if (std::filesystem::exists(options.observations, observation_ec) && !observation_ec) {
          std::string observation_error;
          if (!xenon::recomp::load_adaptive_observations(
                  options.observations, adaptive_observations, observation_error)) {
            const auto message = "adaptive observation trace could not be loaded: " +
                                 observation_error;
            status.report(Phase::Failed, 5, "Loading adaptive analysis feedback", message);
            std::cerr << "xenon-prepare: " << message << "\n";
            return 2;
          }
        }
      }
      adaptive_observations.erase(
          std::remove_if(adaptive_observations.begin(), adaptive_observations.end(),
                         [&](const auto& observation) {
                           return !observation.image_hash.empty() &&
                                  observation.image_hash != effective_hash_hex;
                         }),
          adaptive_observations.end());
      const auto adaptive_observation_hash =
          xenon::recomp::adaptive_observation_fingerprint(adaptive_observations);

      // Gen 9 universal knowledge base. Unlike address-scoped adaptive
      // traces, knowledge records are intentionally allowed to originate
      // from another executable revision; the matcher validates normalized
      // code/CFG identity before any record becomes evidence.
      std::vector<xenon::recomp::KnowledgeRecord> knowledge_records;
      if (!options.knowledge.empty()) {
        std::error_code knowledge_ec;
        if (std::filesystem::exists(options.knowledge, knowledge_ec) && !knowledge_ec) {
          std::string knowledge_error;
          if (!xenon::recomp::load_knowledge_base(
                  options.knowledge, knowledge_records, knowledge_error)) {
            const auto message = "knowledge base could not be loaded: " + knowledge_error;
            status.report(Phase::Failed, 5, "Loading universal analysis knowledge", message);
            std::cerr << "xenon-prepare: " << message << "\n";
            return 2;
          }
        }
      }
      const auto knowledge_base_hash =
          xenon::recomp::knowledge_base_fingerprint(knowledge_records);

      xenon::recomp::ArtifactCacheKey key;
      key.title_id = identity.title_id;
      key.media_id = identity.media_id;
      key.effective_image_hash = effective_hash_hex;
      key.xex_relative_path = module.relative_path;
      key.module_id = module_id;
      key.module_compatibility_version = module_compatibility_version;
      key.hint_set_hash = hint_set_hash;
      key.adaptive_observation_hash = adaptive_observation_hash;
      key.knowledge_base_hash = knowledge_base_hash;
      key.preparation_identity = xenon::recomp::graph::preparation_identity(
          options.recomp_root, XENON_CMAKE_COMMAND, XENON_NATIVE_COMPILER);
      key.target_arch = default_target_arch();
      key.build_config = options.config;
      status.set_cache_key(key.digest());

      // Staging belongs to its worker; never remove another process's active build.
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
        root.set("adaptiveObservationHash", static_cast<double>(adaptive_observation_hash));
        root.set("adaptiveObservationCount", static_cast<double>(adaptive_observations.size()));
        root.set("knowledgeBaseHash", std::to_string(knowledge_base_hash));
        root.set("knowledgeRecordCount", static_cast<double>(knowledge_records.size()));
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

      const auto build = prepare_one_module(effective_image, hint_set, adaptive_observations,
                                            knowledge_records, key, store, options, title_id_hex,
                                            media_id_hex, module.relative_path, /*is_primary=*/true,
                                            status);
      if (!build.ok) {
        if (build.exit_code != 7) std::cerr << "xenon-prepare: " << build.error << "\n";
        return build.exit_code;
      }

      status.report(Phase::Complete, 100, "Preparation complete");
      std::cout << "xenon-prepare: prepared " << build.entry.native_extension_path << "\n";
      return 0;
    }

    // ------------------------------------------------------------------
    // Gen 11: more than one executable module was discovered (a title
    // shipping additional bootable/dispatchable XEX modules beyond the
    // mandatory root default.xex). Prepare every one of them and record the
    // outcome in a Game Compilation Graph manifest. The primary module's
    // failure remains a hard, fatal error exactly like the single-module
    // path above; a secondary module's failure is recorded and reported but
    // does not, by itself, prevent the primary/playable module from being
    // ready - "scan every executable, compile every required module" does
    // not mean one obscure secondary XEX can hold the whole title hostage.
    // ------------------------------------------------------------------
    std::unique_ptr<xenon::recomp::FileModuleHintProvider> hint_provider;
    if (!options.module_dir.empty()) {
      hint_provider = std::make_unique<xenon::recomp::FileModuleHintProvider>(options.module_dir);
      if (!hint_provider->manifest_loaded()) {
        const auto message = "module '" + options.module_dir.string() + "': " + hint_provider->manifest_error();
        status.report(Phase::Failed, 5, "Resolving module", message);
        std::cerr << "xenon-prepare: " << message << "\n";
        return 2;
      }
    }
    const std::string module_compatibility_version =
        hint_provider ? hint_provider->compatibility_version() : std::string("1");

    std::vector<xenon::recomp::AdaptiveObservation> all_adaptive_observations;
    if (!options.observations.empty()) {
      std::error_code observation_ec;
      if (std::filesystem::exists(options.observations, observation_ec) && !observation_ec) {
        std::string observation_error;
        if (!xenon::recomp::load_adaptive_observations(
                options.observations, all_adaptive_observations, observation_error)) {
          const auto message = "adaptive observation trace could not be loaded: " + observation_error;
          status.report(Phase::Failed, 5, "Loading adaptive analysis feedback", message);
          std::cerr << "xenon-prepare: " << message << "\n";
          return 2;
        }
      }
    }

    std::vector<xenon::recomp::KnowledgeRecord> knowledge_records;
    if (!options.knowledge.empty()) {
      std::error_code knowledge_ec;
      if (std::filesystem::exists(options.knowledge, knowledge_ec) && !knowledge_ec) {
        std::string knowledge_error;
        if (!xenon::recomp::load_knowledge_base(options.knowledge, knowledge_records, knowledge_error)) {
          const auto message = "knowledge base could not be loaded: " + knowledge_error;
          status.report(Phase::Failed, 5, "Loading universal analysis knowledge", message);
          std::cerr << "xenon-prepare: " << message << "\n";
          return 2;
        }
      }
    }
    const auto knowledge_base_hash = xenon::recomp::knowledge_base_fingerprint(knowledge_records);

    if (options.recomp_root.empty()) {
      const auto message = "no Xenon-Recomp source root available (pass --recomp-root explicitly)";
      status.report(Phase::Failed, 0, "Configuring native build", message);
      std::cerr << "xenon-prepare: " << message << "\n";
      return 5;
    }
    const auto preparation_identity_value = xenon::recomp::graph::preparation_identity(
        options.recomp_root, XENON_CMAKE_COMMAND, XENON_NATIVE_COMPILER);

    xenon::recomp::GameCompilationGraph graph;
    graph.modules.reserve(discovered_modules.size());
    auto query_modules = xenon::core::JsonValue::make_array();

    bool primary_ok = false;
    int primary_exit_code = 0;
    std::string primary_error;
    std::string primary_query_status = "Missing";
    std::string primary_query_cache_key, primary_query_title_id, primary_query_media_id,
        primary_query_hash, primary_query_native_path;
    bool primary_query_needs_preparation = true;

    for (const auto& module : discovered_modules) {
      xenon::recomp::ModuleCompilationRecord record;
      record.relative_path = module.relative_path;
      record.is_primary = module.is_primary;

      const auto fail_module = [&](const std::string& message) {
        record.status = xenon::recomp::ModulePreparationStatus::Failed;
        record.error = message;
        if (module.is_primary) {
          primary_exit_code = primary_exit_code != 0 ? primary_exit_code : 2;
          primary_error = message;
        } else {
          std::cerr << "xenon-prepare: secondary module '" << module.relative_path
                    << "' failed: " << message << "\n";
        }
      };

      std::vector<std::byte> base_bytes;
      std::string module_error;
      if (!xenon::recomp::read_game_executable_bytes(options.content, module.relative_path,
                                                      base_bytes, module_error)) {
        fail_module(module_error);
        graph.modules.push_back(record);
        continue;
      }

      xenon::xbox::XexImage base_image;
      if (!xenon::xbox::parse_xex_image(base_bytes, base_image, &module_error)) {
        fail_module("'" + module.relative_path + "' is not a valid, supported XEX: " + module_error);
        graph.modules.push_back(record);
        continue;
      }

      xenon::xbox::XexImage effective_image = base_image;
      bool title_update_applied = false;
      if (module.is_primary && !options.title_update.empty()) {
        status.report(Phase::ApplyingTitleUpdate, 5, "Applying selected title update");
        std::vector<std::byte> update_bytes;
        if (!read_whole_host_file(options.title_update, update_bytes, module_error)) {
          fail_module(module_error);
          graph.modules.push_back(record);
          continue;
        }
        xenon::xbox::XexImage patched;
        if (!xenon::xbox::apply_title_update(base_image, update_bytes, patched, &module_error)) {
          fail_module("title update could not be applied: " + module_error);
          graph.modules.push_back(record);
          continue;
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
      record.title_id = title_id_hex;
      record.media_id = media_id_hex;
      record.effective_image_hash = effective_hash_hex;
      if (module.is_primary) status.set_identity(title_id_hex, media_id_hex, effective_hash_hex);

      // A curated module-hint package with no data for a SECONDARY module's
      // specific revision is not fatal: most secondary/rare executables will
      // never have curated per-revision hint data, and requiring it would
      // defeat the entire point of automatic discovery. For the primary
      // module this stays exactly as fatal as it always was.
      std::string module_id = options.module_id;
      std::optional<xenon::recomp::analysis::AnalysisHintSetV2> hint_set;
      std::uint64_t hint_set_hash = 0;
      if (hint_provider) {
        if (module_id.empty()) module_id = hint_provider->module_name();
        xenon::recomp::analysis::AnalysisHintSetV2 resolved{};
        std::string hint_error;
        if (hint_provider->provide(identity, resolved, hint_error)) {
          hint_set_hash = hash_hint_set(resolved);
          hint_set = std::move(resolved);
        } else if (module.is_primary) {
          fail_module(hint_error);
          graph.modules.push_back(record);
          continue;
        } else {
          record.hints_unavailable = true;
        }
      }

      auto adaptive_observations = all_adaptive_observations;
      adaptive_observations.erase(
          std::remove_if(adaptive_observations.begin(), adaptive_observations.end(),
                         [&](const auto& observation) {
                           return !observation.image_hash.empty() &&
                                  observation.image_hash != effective_hash_hex;
                         }),
          adaptive_observations.end());
      const auto adaptive_observation_hash =
          xenon::recomp::adaptive_observation_fingerprint(adaptive_observations);

      xenon::recomp::ArtifactCacheKey key;
      key.title_id = identity.title_id;
      key.media_id = identity.media_id;
      key.effective_image_hash = effective_hash_hex;
      key.xex_relative_path = module.relative_path;
      key.module_id = module_id;
      key.module_compatibility_version = module_compatibility_version;
      key.hint_set_hash = hint_set_hash;
      key.adaptive_observation_hash = adaptive_observation_hash;
      key.knowledge_base_hash = knowledge_base_hash;
      key.preparation_identity = preparation_identity_value;
      key.target_arch = default_target_arch();
      key.build_config = options.config;
      record.cache_key = key.digest();
      if (module.is_primary) status.set_cache_key(key.digest());

      const auto existing = store.lookup(key);

      if (options.query) {
        auto entry_json = xenon::core::JsonValue::make_object();
        entry_json.set("relativePath", module.relative_path);
        entry_json.set("isPrimary", module.is_primary);
        const std::string entry_status =
            existing.status == xenon::recomp::ArtifactCacheStatus::Fresh ? "Fresh"
            : existing.status == xenon::recomp::ArtifactCacheStatus::Invalid ? "Invalid"
                                                                              : "Missing";
        entry_json.set("status", entry_status);
        entry_json.set("cacheKey", key.digest());
        entry_json.set("nativeExtensionPath", json_string_or_empty(existing.native_extension_path));
        query_modules.append(std::move(entry_json));
        record.status = existing.status == xenon::recomp::ArtifactCacheStatus::Fresh
                           ? xenon::recomp::ModulePreparationStatus::Fresh
                           : xenon::recomp::ModulePreparationStatus::Pending;
        record.native_extension_path = json_string_or_empty(existing.native_extension_path);
        if (module.is_primary) {
          primary_query_status = entry_status;
          primary_query_needs_preparation = existing.status != xenon::recomp::ArtifactCacheStatus::Fresh;
          primary_query_cache_key = key.digest();
          primary_query_title_id = title_id_hex;
          primary_query_media_id = media_id_hex;
          primary_query_hash = effective_hash_hex;
          primary_query_native_path = json_string_or_empty(existing.native_extension_path);
        }
        graph.modules.push_back(record);
        continue;
      }

      if (!options.force && existing.status == xenon::recomp::ArtifactCacheStatus::Fresh) {
        record.status = xenon::recomp::ModulePreparationStatus::Fresh;
        record.native_extension_path = existing.native_extension_path.string();
        if (module.is_primary) {
          primary_ok = true;
          status.set_native_extension_path(existing.native_extension_path.string());
        }
        graph.modules.push_back(record);
        continue;
      }

      const auto build = prepare_one_module(effective_image, hint_set, adaptive_observations,
                                            knowledge_records, key, store, options, title_id_hex,
                                            media_id_hex, module.relative_path, module.is_primary,
                                            status);
      if (!build.ok) {
        if (build.exit_code == 7) {
          record.status = xenon::recomp::ModulePreparationStatus::Skipped;
          graph.modules.push_back(record);
          primary_exit_code = 7;
          break;  // cancellation stops the whole batch, not just this module
        }
        fail_module(build.error);
        graph.modules.push_back(record);
        continue;
      }

      record.status = xenon::recomp::ModulePreparationStatus::Prepared;
      record.native_extension_path = build.entry.native_extension_path.string();
      if (module.is_primary) primary_ok = true;
      graph.modules.push_back(record);
    }

    // Always write the Game Compilation Graph manifest so a caller (the
    // launcher, a diagnostic tool, a later re-run) can see every discovered
    // module's outcome in one place, not only whichever one this specific
    // invocation happened to be asked about.
    {
      std::error_code ec;
      std::filesystem::create_directories(options.cache_root, ec);
      std::ofstream manifest(options.cache_root / "game-compilation-graph.json",
                             std::ios::binary | std::ios::trunc);
      manifest << graph.to_json().dump(2);
    }

    if (options.query) {
      xenon::core::JsonValue root = xenon::core::JsonValue::make_object();
      root.set("ok", true);
      root.set("needsPreparation", primary_query_needs_preparation);
      root.set("status", primary_query_status);
      root.set("cacheKey", primary_query_cache_key);
      root.set("titleId", primary_query_title_id);
      root.set("mediaId", primary_query_media_id);
      root.set("effectiveImageHash", primary_query_hash);
      root.set("nativeExtensionPath", primary_query_native_path);
      root.set("modules", std::move(query_modules));
      std::cout << root.dump() << "\n";
      return 0;
    }

    if (primary_exit_code == 7) {
      status.report(Phase::Cancelled, 0, "Cancelled");
      return 7;
    }
    if (!primary_ok) {
      status.report(Phase::Failed, 0, "Preparing primary module", primary_error);
      std::cerr << "xenon-prepare: " << primary_error << "\n";
      return primary_exit_code != 0 ? primary_exit_code : 2;
    }

    const auto primary_it = std::find_if(graph.modules.begin(), graph.modules.end(),
                                         [](const auto& record) { return record.is_primary; });
    status.report(Phase::Complete, 100, "Preparation complete");
    std::cout << "xenon-prepare: prepared "
              << (primary_it != graph.modules.end() ? primary_it->native_extension_path
                                                    : std::string())
              << " (" << graph.modules.size() << " module(s) discovered)\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "xenon-prepare: unexpected error: " << exception.what() << "\n";
    return 2;
  }
}
