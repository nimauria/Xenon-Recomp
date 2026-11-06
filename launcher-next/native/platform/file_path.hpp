#pragma once
#include <filesystem>
namespace xenon::launcher_next {
inline std::filesystem::path normal_path(const std::filesystem::path &path) {
#ifdef _WIN32
  const auto &text = path.native();
  if (text.starts_with(L"\\\\?\\UNC\\"))
    return std::filesystem::path(L"\\\\" + text.substr(8));
  if (text.starts_with(L"\\\\?\\"))
    return std::filesystem::path(text.substr(4));
#endif
  return path;
}
// Keep ordinary paths in state, receipts and comparisons. Use the Win32 extended
// spelling only at I/O boundaries, so CRT streams also support long paths.
inline std::filesystem::path io_path(const std::filesystem::path &path) {
#ifdef _WIN32
  if (!path.is_absolute())
    return path;
  auto value = path.lexically_normal();
  value.make_preferred();
  const auto &text = value.native();
  if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\"))
    return value;
  if (text.starts_with(L"\\\\"))
    return std::filesystem::path(L"\\\\?\\UNC\\" + text.substr(2));
  return std::filesystem::path(L"\\\\?\\" + text);
#else
  return path;
#endif
}
} // namespace xenon::launcher_next
