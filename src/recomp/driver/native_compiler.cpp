#include "xenon/recomp/native_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

namespace xenon::recomp {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool is_file(const std::filesystem::path& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

std::optional<std::filesystem::path> first_existing(
    std::span<const std::filesystem::path> candidates) {
  for (const auto& candidate : candidates)
    if (is_file(candidate)) return candidate;
  return std::nullopt;
}

std::string environment(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read-only lookup on the preparing thread
#endif
  const char* value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return value ? std::string(value) : std::string();
}

}  // namespace

std::optional<GameCompilerChoice> select_game_compiler(
    std::string_view request, const std::filesystem::path& native_compiler,
    std::span<const std::filesystem::path> clang_cl_candidates, std::string& error) {
  const auto mode = lower(request);
  if (mode.empty() || mode == "auto") {
    if (lower(native_compiler.filename().string()) == "cl.exe") {
      if (const auto clang = first_existing(clang_cl_candidates))
        return GameCompilerChoice{*clang, "clang-cl (auto: optimizes every generated function)"};
    }
    return GameCompilerChoice{native_compiler, "the native compiler (auto: no clang-cl found)"};
  }
  if (mode == "msvc" || mode == "native")
    return GameCompilerChoice{native_compiler, "the native compiler (requested)"};
  if (mode == "clang" || mode == "clang-cl") {
    if (const auto clang = first_existing(clang_cl_candidates))
      return GameCompilerChoice{*clang, "clang-cl (requested)"};
    error = "clang-cl was requested for the game module but was not found (set CLANG_CL to its path)";
    return std::nullopt;
  }
  const std::filesystem::path path{std::string(request)};
  if (is_file(path)) return GameCompilerChoice{path, "the compiler at the requested path"};
  error = "the requested game module compiler does not exist: " + std::string(request);
  return std::nullopt;
}

std::vector<std::filesystem::path> default_clang_cl_candidates() {
  std::vector<std::filesystem::path> candidates;
  if (auto from_env = environment("CLANG_CL"); !from_env.empty()) candidates.emplace_back(from_env);
#if defined(_WIN32)
  candidates.emplace_back("C:/Program Files/LLVM/bin/clang-cl.exe");
  constexpr char kSeparator = ';';
  constexpr const char* kName = "clang-cl.exe";
#else
  constexpr char kSeparator = ':';
  constexpr const char* kName = "clang-cl";
#endif
  const auto path = environment("PATH");
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto end = std::min(path.find(kSeparator, begin), path.size());
    if (end > begin) candidates.push_back(std::filesystem::path(path.substr(begin, end - begin)) / kName);
    begin = end + 1;
  }
  return candidates;
}

}  // namespace xenon::recomp
