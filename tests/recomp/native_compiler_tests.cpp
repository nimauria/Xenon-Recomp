// Choosing the compiler for a generated game module (native_compiler.hpp).

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xenon/recomp/native_compiler.hpp"

using xenon::recomp::select_game_compiler;

int main() {
  const auto root = std::filesystem::temp_directory_path() / "xenon_native_compiler_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "msvc");
  std::filesystem::create_directories(root / "llvm");
  const auto cl = root / "msvc" / "cl.exe";
  const auto clang = root / "llvm" / "clang-cl.exe";
  const auto gcc = root / "c++";
  for (const auto& file : {cl, clang, gcc}) std::ofstream(file) << "x";
  const std::vector<std::filesystem::path> missing{root / "nowhere" / "clang-cl.exe"};
  const std::vector<std::filesystem::path> found{root / "nowhere" / "clang-cl.exe", clang};
  std::string error;

  // auto: clang-cl over cl.exe when one exists, else the native compiler.
  for (const char* request : {"", "auto", "AUTO"}) {
    auto choice = select_game_compiler(request, cl, found, error);
    assert(choice && choice->compiler == clang);
    choice = select_game_compiler(request, cl, missing, error);
    assert(choice && choice->compiler == cl);
  }
  // auto keeps a non-MSVC native compiler: clang-cl targets the MSVC ABI.
  {
    const auto choice = select_game_compiler("auto", gcc, found, error);
    assert(choice && choice->compiler == gcc);
  }
  // msvc/native: always the native compiler.
  for (const char* request : {"msvc", "native"}) {
    const auto choice = select_game_compiler(request, cl, found, error);
    assert(choice && choice->compiler == cl);
  }
  // clang: the first existing candidate, or an error naming the problem.
  {
    auto choice = select_game_compiler("clang", cl, found, error);
    assert(choice && choice->compiler == clang);
    error.clear();
    choice = select_game_compiler("clang", cl, missing, error);
    assert(!choice && error.find("clang-cl") != std::string::npos);
  }
  // An explicit path must exist.
  {
    auto choice = select_game_compiler(clang.string(), cl, missing, error);
    assert(choice && choice->compiler == clang);
    error.clear();
    choice = select_game_compiler((root / "absent.exe").string(), cl, found, error);
    assert(!choice && !error.empty());
  }
  // Every reason is a printable line.
  assert(!select_game_compiler("auto", cl, found, error)->reason.empty());

  std::filesystem::remove_all(root);
  return 0;
}
