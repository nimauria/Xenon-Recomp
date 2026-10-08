// Small helpers for xenon-prepare: hashing, file reads, phase names.

#include "prepare_internal.hpp"

namespace xenon::prepare_tool {

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

}  // namespace xenon::prepare_tool
