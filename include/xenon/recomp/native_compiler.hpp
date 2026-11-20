#pragma once

// Which C++ compiler builds a generated game module.
//
// Generated guest functions can be very large. MSVC's global optimizer is far
// worse than linear in function size, so the code generator turns it off for
// functions over a size limit, and those functions then run unoptimized:
// AC6's busiest audio mixer function compiled to an 84 KB stack frame and
// 95,000 instructions under MSVC, against 1.4 KB and 64,000 under clang-cl,
// which optimizes every function. clang-cl uses the MSVC ABI and standard
// library, so a module it builds links and loads exactly as an MSVC one.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xenon::recomp {

struct GameCompilerChoice {
  std::filesystem::path compiler;
  // One line for the preparation log: what was chosen and why.
  std::string reason;
};

// `request` (XENON_GAME_COMPILER or --compiler):
//   ""/"auto"       clang-cl when the native compiler is MSVC's cl.exe and a
//                   candidate exists, else the native compiler;
//   "msvc"/"native" the native compiler (the one Xenon itself was built with);
//   "clang"         the first existing clang-cl candidate, an error if none;
//   anything else   a path to a compiler executable, which must exist.
// Returns nullopt, with `error` set, when the request cannot be met.
[[nodiscard]] std::optional<GameCompilerChoice> select_game_compiler(
    std::string_view request, const std::filesystem::path& native_compiler,
    std::span<const std::filesystem::path> clang_cl_candidates, std::string& error);

// Where clang-cl is looked for, in order: $CLANG_CL, the LLVM installer's
// default location, then every PATH directory.
[[nodiscard]] std::vector<std::filesystem::path> default_clang_cl_candidates();

}  // namespace xenon::recomp
