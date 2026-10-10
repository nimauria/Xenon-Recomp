#pragma once

// Configure arguments that make a nested CMake build (a generated project
// built against a snapshot of the Xenon tree) use the generator and C++
// compiler of the build that compiled the test. Without them a nested
// configure on Windows falls back to the Visual Studio generator, which
// compiles a project's files one at a time even with --parallel; that ran
// these tests up to CTest's 900 s timeout on CI. Matching the outer build
// also means the nested project is built with the toolchain under test.
// The definitions come from xenon_test_uses_nested_cmake() in
// tests/CMakeLists.txt.

#include <string>
#include <string_view>

namespace xenon::test {

inline std::string nested_cmake_toolchain_args() {
  std::string args;
#if defined(XENON_TEST_CMAKE_GENERATOR)
  args += " -G \"" XENON_TEST_CMAKE_GENERATOR "\"";
#endif
#if defined(XENON_TEST_CMAKE_GENERATOR_PLATFORM)
  if (std::string_view(XENON_TEST_CMAKE_GENERATOR_PLATFORM).size() != 0)
    args += " -A \"" XENON_TEST_CMAKE_GENERATOR_PLATFORM "\"";
#endif
#if defined(XENON_TEST_CMAKE_MAKE_PROGRAM)
  if (std::string_view(XENON_TEST_CMAKE_MAKE_PROGRAM).size() != 0)
    args += " -DCMAKE_MAKE_PROGRAM=\"" XENON_TEST_CMAKE_MAKE_PROGRAM "\"";
#endif
#if defined(XENON_TEST_CXX_COMPILER)
  args += " -DCMAKE_CXX_COMPILER=\"" XENON_TEST_CXX_COMPILER "\"";
#endif
  return args;
}

}  // namespace xenon::test
