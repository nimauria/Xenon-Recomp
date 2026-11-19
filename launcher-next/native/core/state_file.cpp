#include "state_file.hpp"
#include "platform/file_path.hpp"
#include <fstream>
#include <random>
#include <sstream>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string read_bytes(const fs::path &path, std::uintmax_t limit, bool tail) {
#ifdef _WIN32
  struct File {
    HANDLE value;
    ~File() {
      if (value != INVALID_HANDLE_VALUE)
        CloseHandle(value);
    }
  } file{INVALID_HANDLE_VALUE};
  DWORD open_error = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 12; ++attempt) {
    file.value = CreateFileW(io_path(path).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value != INVALID_HANDLE_VALUE)
      break;
    open_error = GetLastError();
    if (open_error != ERROR_FILE_NOT_FOUND && open_error != ERROR_ACCESS_DENIED &&
        open_error != ERROR_SHARING_VIOLATION && open_error != ERROR_DELETE_PENDING)
      break;
    if (attempt < 11)
      Sleep(2);
  }
  if (file.value == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Could not read launcher file: Win32 " + std::to_string(open_error));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
      (!tail && static_cast<std::uintmax_t>(size.QuadPart) > limit))
    throw std::runtime_error("Invalid or oversized launcher file");
  const auto length = std::min<std::uintmax_t>(size.QuadPart, limit);
  LARGE_INTEGER position{};
  position.QuadPart = tail ? size.QuadPart - static_cast<LONGLONG>(length) : 0;
  if (!SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN))
    throw std::runtime_error("Could not seek launcher file");
  std::string result(static_cast<std::size_t>(length), '\0');
  std::size_t offset = 0;
  while (offset < result.size()) {
    DWORD count = 0;
    if (!ReadFile(file.value, result.data() + offset,
                  static_cast<DWORD>(std::min<std::size_t>(result.size() - offset, 1024 * 1024)),
                  &count, nullptr) ||
        !count)
      throw std::runtime_error("Could not read complete launcher file");
    offset += count;
  }
  return result;
#else
  if (!fs::is_regular_file(path))
    throw std::runtime_error("Invalid launcher file");
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("Could not read launcher file");
  const auto end = input.tellg();
  if (end < 0)
    throw std::runtime_error("Could not inspect launcher file size");
  const auto size = static_cast<std::uintmax_t>(end);
  if (!tail && size > limit)
    throw std::runtime_error("Oversized launcher file");
  const auto length = std::min(size, limit);
  input.seekg(static_cast<std::streamoff>(tail ? size - length : 0));
  std::string result(static_cast<std::size_t>(length), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  if (static_cast<std::size_t>(input.gcount()) != result.size())
    throw std::runtime_error("Could not read complete launcher file");
  return result;
#endif
}
} // namespace
std::string make_id() {
  std::random_device random;
  std::ostringstream out;
  out << std::hex;
  for (int i = 0; i < 4; ++i)
    out << random() << '-';
  return out.str();
}
Json read_document(const fs::path &path, std::uintmax_t limit) {
  return Json::parse(read_bytes(path, limit, false));
}
std::string read_text_tail(const fs::path &path, std::uintmax_t limit) {
  auto result = read_bytes(path, limit, true);
  // A bounded UTF-8 tail may begin partway through a character.
  auto start = std::size_t{};
  while (start < result.size() && (static_cast<unsigned char>(result[start]) & 0xC0) == 0x80)
    ++start;
  result.erase(0, start);
  return result;
}
void write_document(const fs::path &path, const Json &value) {
  fs::create_directories(io_path(path.parent_path()));
  auto temporary = path;
  temporary += "." + make_id() + ".tmp";
  try {
    {
      std::ofstream output(io_path(temporary), std::ios::binary);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << value.dump(2) << '\n';
      output.flush();
    }
#ifdef _WIN32
    bool committed = false;
    DWORD commit_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 12; ++attempt) {
      if (MoveFileExW(io_path(temporary).c_str(), io_path(path).c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        committed = true;
        break;
      }
      commit_error = GetLastError();
      if (commit_error != ERROR_ACCESS_DENIED && commit_error != ERROR_SHARING_VIOLATION &&
          commit_error != ERROR_LOCK_VIOLATION && commit_error != ERROR_DELETE_PENDING)
        break;
      if (attempt < 11)
        Sleep(5);
    }
    if (!committed)
      throw std::runtime_error("Could not commit launcher state: Win32 " +
                               std::to_string(commit_error));
#else
    fs::rename(io_path(temporary), io_path(path));
#endif
  } catch (...) {
    std::error_code ignored;
    fs::remove(io_path(temporary), ignored);
    throw;
  }
}
} // namespace xenon::launcher_next
