// xenon::recomp::WorkspaceLock: one live owner at a time, and a lock left by
// a preparation that died is taken over rather than blocking every later
// preparation. Regression: after a power cut xenon-prepare refused the AC6
// workspace forever ("busy (or has an abandoned .lock)") until the lock
// directory was deleted by hand.

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "xenon/recomp/workspace_lock.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using xenon::recomp::ProcessIdentity;
using xenon::recomp::WorkspaceLock;

std::filesystem::path fresh_workspace(const char* name) {
  const auto path = std::filesystem::temp_directory_path() / "xenon_workspace_lock_tests" / name;
  std::filesystem::remove_all(path);
  return path;
}

void write_owner(const std::filesystem::path& workspace, const ProcessIdentity& owner) {
  std::filesystem::create_directories(WorkspaceLock::lock_path(workspace));
  std::ofstream(WorkspaceLock::owner_path(workspace), std::ios::binary)
      << "{\"pid\":\"" << owner.pid << "\",\"startTime\":\"" << owner.start_time << "\"}\n";
}

bool acquiring_throws(const std::filesystem::path& workspace) {
  try {
    WorkspaceLock lock(workspace);
  } catch (const std::runtime_error& error) {
    assert(std::string(error.what()).find("busy") != std::string::npos);
    return true;
  }
  return false;
}

// The ID of a process that has started and exited.
std::uint64_t exited_process_id() {
#if defined(_WIN32)
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  wchar_t command[] = L"cmd.exe /c exit 0";
  const BOOL created = ::CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &info);
  assert(created);
  ::WaitForSingleObject(info.hProcess, INFINITE);
  const auto pid = static_cast<std::uint64_t>(info.dwProcessId);
  ::CloseHandle(info.hThread);
  ::CloseHandle(info.hProcess);
  return pid;
#else
  const pid_t child = ::fork();
  if (child == 0) ::_exit(0);
  int status = 0;
  ::waitpid(child, &status, 0);
  return static_cast<std::uint64_t>(child);
#endif
}

void test_one_live_owner_at_a_time() {
  const auto workspace = fresh_workspace("live");
  {
    WorkspaceLock lock(workspace);
    assert(!lock.reclaimed());
    assert(std::filesystem::exists(WorkspaceLock::owner_path(workspace)));
    // Held by this (running) process: a second acquisition is refused.
    assert(acquiring_throws(workspace));
    assert(std::filesystem::exists(WorkspaceLock::lock_path(workspace)) &&
           "a refused acquisition leaves the owner's lock alone");
  }
  assert(!std::filesystem::exists(WorkspaceLock::lock_path(workspace)) && "released on destruction");
  WorkspaceLock again(workspace);
  assert(!again.reclaimed());
}

void test_process_identity() {
  const auto self = xenon::recomp::current_process_identity();
  assert(self.pid != 0 && xenon::recomp::process_is_running(self));
  assert(!xenon::recomp::process_is_running({}));
  assert(!xenon::recomp::process_is_running({exited_process_id(), 0}));
  const auto boot = xenon::recomp::host_boot_time();
  assert(boot && *boot < std::filesystem::file_time_type::clock::now());
}

void test_lock_of_an_exited_owner_is_taken_over() {
  const auto workspace = fresh_workspace("exited");
  write_owner(workspace, {exited_process_id(), 0});
  WorkspaceLock lock(workspace);
  assert(lock.reclaimed());
  // The new lock records this process, which is running.
  assert(acquiring_throws(workspace));
}

void test_lock_whose_id_was_reused_is_taken_over() {
  // After a reboot the recorded ID can belong to an unrelated process: the
  // start time tells them apart (here, this process with another start).
  const auto self = xenon::recomp::current_process_identity();
  if (self.start_time == 0) {
    std::cout << "  (host reports no process start time; ID reuse check skipped)\n";
    return;
  }
  const auto workspace = fresh_workspace("reused");
  write_owner(workspace, {self.pid, self.start_time + 1u});
  WorkspaceLock lock(workspace);
  assert(lock.reclaimed());
}

void test_ownerless_lock_is_held_until_it_predates_boot() {
  const auto workspace = fresh_workspace("ownerless");
  std::filesystem::create_directories(WorkspaceLock::lock_path(workspace));
  // Created since boot: possibly a process between creating the lock and
  // recording itself, so it is respected.
  assert(acquiring_throws(workspace));
  const auto boot = xenon::recomp::host_boot_time();
  assert(boot);
  std::filesystem::last_write_time(WorkspaceLock::lock_path(workspace), *boot - std::chrono::hours(1));
  WorkspaceLock lock(workspace);
  assert(lock.reclaimed());
}

}  // namespace

int main() {
  test_one_live_owner_at_a_time();
  test_process_identity();
  test_lock_of_an_exited_owner_is_taken_over();
  test_lock_whose_id_was_reused_is_taken_over();
  test_ownerless_lock_is_held_until_it_predates_boot();
  std::filesystem::remove_all(std::filesystem::temp_directory_path() / "xenon_workspace_lock_tests");
  std::cout << "xenon_workspace_lock_tests passed\n";
  return 0;
}
