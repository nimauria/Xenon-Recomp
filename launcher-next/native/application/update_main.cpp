#include "core/launcher_update.hpp"
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int count = 0;
  auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments || count != 2) {
    if (arguments)
      LocalFree(arguments);
    return 2;
  }
  const auto plan = std::filesystem::path(arguments[1]);
  LocalFree(arguments);
#else
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto plan = std::filesystem::u8path(argv[1]);
#endif
  try {
    xenon::launcher_next::apply_launcher_update(plan);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
