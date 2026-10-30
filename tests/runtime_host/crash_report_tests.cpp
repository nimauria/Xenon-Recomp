// Host crash reports (runtime_host/src/crash_report.hpp): the line format,
// and - by running this executable again in a mode that really crashes - that
// an access violation, a stack overflow on the main thread or on another
// thread, and an uncaught C++ exception each leave their line in crash.log.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#endif

#include "crash_report.hpp"

using xenon::runtime_host::CrashInfo;

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

volatile bool g_never = false;

int recurse_forever(int depth) {
  volatile char frame[4096];
  frame[0] = static_cast<char>(depth);
  if (g_never) return frame[0];
  return recurse_forever(depth + 1) + frame[0];
}

// The crashing half: install, then fail the way `mode` names.
int crash(std::string_view mode, const std::filesystem::path& directory) {
#if defined(_WIN32)
  // No Windows Error Reporting or CRT dialog may wait for a click.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  if (!xenon::runtime_host::install_crash_report(directory)) return 3;
  if (mode == "null-write") {
    volatile int* null_pointer = nullptr;
    *null_pointer = 1;
  } else if (mode == "overflow") {
    return recurse_forever(0);
  } else if (mode == "thread-overflow") {
    std::thread worker([] { recurse_forever(0); });
    worker.join();
  } else if (mode == "terminate") {
    throw std::runtime_error("nobody caught this");
  }
  return 4;
}

void expect_crash_line(const char* self, std::string_view mode, const std::filesystem::path& root,
                       const std::vector<std::string_view>& expected) {
  const auto directory = root / std::string(mode);
  std::filesystem::create_directories(directory);
  const std::string command = "\"\"" + std::string(self) + "\" " + std::string(mode) + " \"" +
                              directory.string() + "\"\"";
#if defined(_WIN32)
  const int status = std::system(command.c_str());
#else
  const int status = std::system(command.substr(1, command.size() - 2).c_str());
#endif
  assert(status != 0 && "the child must not exit normally");
  const auto lines = read_lines(directory / "crash.log");
  if (lines.size() != 1u) {
    std::cerr << mode << ": crash.log has " << lines.size() << " lines\n";
  }
  assert(lines.size() == 1u);
  for (const auto& text : expected) {
    if (lines[0].find(text) == std::string::npos) {
      std::cerr << mode << ": missing '" << text << "' in: " << lines[0] << "\n";
      assert(false);
    }
  }
}

void test_format() {
  char line[512];
  CrashInfo info{};
  info.code = 0xC00000FDu;
  info.address = 0x7FF6'1234'5678u;
  info.module = "xenon_runtime_host.exe";
  info.module_offset = 0x45678u;
  info.thread_id = 4242u;
  // 1700000000 is 2023-11-14T22:13:20Z.
  auto length = xenon::runtime_host::format_crash_line(line, sizeof(line), info, 1700000000, 7);
  assert(std::string(line, length) ==
         "2023-11-14T22:13:20.007Z fatal: host process crashed: exception 0xC00000FD "
         "(stack overflow) at 0x7FF612345678 (xenon_runtime_host.exe+0x45678) on host thread "
         "4242\n");

  CrashInfo access{};
  access.code = 0xC0000005u;
  access.address = 0x1000u;
  access.has_access = true;
  access.access_kind = 1u;
  access.access_address = 0u;
  access.thread_id = 1u;
  length = xenon::runtime_host::format_crash_line(line, sizeof(line), access, 0, 0);
  assert(std::string(line, length) ==
         "1970-01-01T00:00:00.000Z fatal: host process crashed: exception 0xC0000005 "
         "(access violation) at 0x1000, writing 0x0 on host thread 1\n");

  // An unnamed code has no name; a tiny buffer still ends in a newline.
  CrashInfo unknown{};
  unknown.code = 0x12345678u;
  length = xenon::runtime_host::format_crash_line(line, sizeof(line), unknown, 86399, 999);
  assert(std::string(line, length).starts_with("1970-01-01T23:59:59.999Z"));
  assert(std::string(line, length).find("0x12345678 on host thread") != std::string::npos);
  length = xenon::runtime_host::format_crash_line(line, 8, unknown, 0, 0);
  assert(length == 8u && line[7] == '\n');
  assert(xenon::runtime_host::crash_code_name(0xC0000409u, false) == "stack buffer overrun");
}

void test_installation(const std::filesystem::path& root) {
  assert(!xenon::runtime_host::install_crash_report({}));
  const auto directory = root / "direct";
  std::filesystem::create_directories(directory);
  assert(xenon::runtime_host::install_crash_report(directory));
  assert(xenon::runtime_host::crash_report_path() == directory / "crash.log");
  CrashInfo info{};
  info.code = 0xC0000094u;
  xenon::runtime_host::write_crash_report(info);
  xenon::runtime_host::write_crash_report(info);
  const auto lines = read_lines(directory / "crash.log");
  assert(lines.size() == 2u && lines[1].find("(integer divide by zero)") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3) return crash(argv[1], argv[2]);

  const auto root = std::filesystem::temp_directory_path() / "xenon_crash_report_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  test_format();
#if defined(_WIN32)
  expect_crash_line(argv[0], "null-write", root,
                    {" fatal: host process crashed: exception 0xC0000005 (access violation)",
                     "xenon_runtime_host_crash_report_tests", ", writing 0x0"});
  expect_crash_line(argv[0], "overflow", root, {"exception 0xC00000FD (stack overflow)"});
  expect_crash_line(argv[0], "thread-overflow", root, {"exception 0xC00000FD (stack overflow)"});
#else
  expect_crash_line(argv[0], "null-write", root, {"signal 11 (segmentation fault)", "0x0"});
  expect_crash_line(argv[0], "overflow", root, {"signal 11 (segmentation fault)"});
#endif
  expect_crash_line(argv[0], "terminate", root,
                    {" fatal: host process terminated: std::terminate (nobody caught this)"});
  // Installs handlers in this process, so it runs last.
  test_installation(root);

  std::filesystem::remove_all(root);
  std::cout << "crash report tests passed" << std::endl;
  return 0;
}
