// Running child processes (CMake, the compiler) so a stop signal can
// cancel them.

#include "prepare_internal.hpp"

namespace xenon::prepare_tool {

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

}  // namespace xenon::prepare_tool
