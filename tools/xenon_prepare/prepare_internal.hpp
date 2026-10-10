#pragma once

// Private to the xenon-prepare tool: shared types and the helpers its
// translation units call. See main.cpp for the tool itself.

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


namespace xenon::prepare_tool {

struct PreparationWorkspace {
  std::filesystem::path path;
  explicit PreparationWorkspace(std::filesystem::path p) : path(std::move(p)) {
    std::filesystem::create_directories(path);
    if (!std::filesystem::create_directory(path / ".lock"))
      throw std::runtime_error("preparation workspace is busy (or has an abandoned .lock): " + path.string());
  }
  ~PreparationWorkspace() { std::error_code ec; std::filesystem::remove(path / ".lock", ec); }
};

std::string lower_ascii(std::string value);

std::int64_t now_epoch_ms();

std::string default_target_arch();

bool read_whole_host_file(const std::filesystem::path& path, std::vector<std::byte>& out,
                          std::string& error);

std::uint64_t hash_hint_set(const xenon::recomp::analysis::AnalysisHintSetV2& hint_set);

std::optional<std::filesystem::path> find_built_module(const std::filesystem::path& build_dir,
                                                        const std::string& config);

std::string json_string_or_empty(const std::filesystem::path& path);

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

const char* phase_name(Phase phase) noexcept;

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

bool cancellation_requested(const std::filesystem::path& stop_signal);

bool run_cancellable_command(const std::vector<std::string>& args,
                             const std::filesystem::path& stop_signal, std::string& error,
                             bool& cancelled);

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

void print_usage();

bool parse_args(int argc, char** argv, Options& options, std::string& error);

// ---------------------------------------------------------------------------
// Optional analysis feedback (--observations, --knowledge). Not given, or not
// created yet, is an empty set. A file that exists but cannot be loaded is
// reported as Failed and returns false; xenon-prepare then exits with 2.
bool load_optional_observations(const Options& options, StatusReporter& status,
                                std::vector<xenon::recomp::AdaptiveObservation>& observations);
bool load_optional_knowledge(const Options& options, StatusReporter& status,
                             std::vector<xenon::recomp::KnowledgeRecord>& records);

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
                                     StatusReporter& status);

}  // namespace xenon::prepare_tool
