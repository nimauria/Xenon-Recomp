#include "desktop.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
extern char **environ;
#endif
namespace xenon::launcher_next {
#ifdef _WIN32
namespace {
std::wstring wide(const std::string &text) { return std::filesystem::u8path(text).wstring(); }
std::wstring quote(const std::wstring &value) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (const auto c : value) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'\"') {
      out.append(slashes * 2 + 1, L'\\');
      out += c;
      slashes = 0;
      continue;
    }
    out.append(slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L'\"';
}
} // namespace
void open_desktop_target(const std::string &target) {
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(target).c_str(), nullptr,
                                              nullptr, SW_SHOWNORMAL)) <= 32)
    throw std::runtime_error("The operating system could not open that location");
}
bool launch_detached(const std::filesystem::path &executable,
                     const std::vector<std::string> &arguments) {
  auto command = quote(executable.wstring());
  for (const auto &argument : arguments)
    command += L' ' + quote(wide(argument));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NEW_PROCESS_GROUP, nullptr, executable.parent_path().c_str(), &startup,
                      &process))
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
ProcessIdentity launch_process(const std::filesystem::path &executable,
                               const std::vector<std::string> &arguments) {
  auto command = quote(executable.wstring());
  for (const auto &argument : arguments)
    command += L' ' + quote(wide(argument));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr,
                      executable.parent_path().c_str(), &startup, &process))
    throw std::runtime_error("Could not start the runtime host");
  FILETIME created{}, exited{}, kernel{}, user{};
  const auto ok = GetProcessTimes(process.hProcess, &created, &exited, &kernel, &user);
  const ProcessIdentity identity{
      process.dwProcessId,
      ok ? (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0};
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return identity;
}
bool process_running(ProcessIdentity identity) {
  if (!identity.pid || !identity.created)
    return false;
  const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                   static_cast<DWORD>(identity.pid));
  if (!process)
    return false;
  FILETIME created{}, exited{}, kernel{}, user{};
  const auto ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
  const auto token =
      (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  const auto running =
      ok && token == identity.created && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
}
#else
namespace {
std::uint64_t creation_token(pid_t pid) {
#ifdef __APPLE__
  proc_bsdinfo info{};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
    return 0;
  return info.pbi_start_tvsec * 1000000ull + info.pbi_start_tvusec;
#else
  std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  std::getline(file, line);
  const auto end = line.rfind(')');
  if (end == std::string::npos)
    return 0;
  std::istringstream fields(line.substr(end + 2));
  std::string field;
  for (int index = 0; index <= 19; ++index)
    if (!(fields >> field))
      return 0;
  try {
    return std::stoull(field);
  } catch (...) {
    return 0;
  }
#endif
}
} // namespace
bool launch_detached(const std::filesystem::path &executable,
                     const std::vector<std::string> &arguments) {
  auto file = executable.string();
  std::vector<std::string> values{file};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : values)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  pid_t pid{};
  return posix_spawnp(&pid, file.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
}
ProcessIdentity launch_process(const std::filesystem::path &executable,
                               const std::vector<std::string> &arguments) {
  auto file = executable.string();
  std::vector<std::string> values{file};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : values)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  pid_t pid{};
  if (posix_spawnp(&pid, file.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
    throw std::runtime_error("Could not start the runtime host");
  return {static_cast<std::uint64_t>(pid), creation_token(pid)};
}
bool process_running(ProcessIdentity identity) {
  if (!identity.pid || !identity.created)
    return false;
  int status{};
  const auto pid = static_cast<pid_t>(identity.pid);
  if (waitpid(pid, &status, WNOHANG) == pid)
    return false;
  return kill(pid, 0) == 0 && creation_token(pid) == identity.created;
}
void open_desktop_target(const std::string &target) {
#ifdef __APPLE__
  std::string opener = "/usr/bin/open";
#else
  std::string opener = "xdg-open";
#endif
  char *argv[] = {opener.data(), const_cast<char *>(target.c_str()), nullptr};
  pid_t child{};
  if (posix_spawnp(&child, opener.c_str(), nullptr, nullptr, argv, environ) != 0)
    throw std::runtime_error("The desktop opener is unavailable");
  int status{};
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("The desktop could not open that location");
}
#endif
} // namespace xenon::launcher_next
