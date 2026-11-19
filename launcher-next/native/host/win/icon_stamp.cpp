// Stamp our branding into the copied CEF bootstrap, leaving vendor code and
// sandbox support intact. This build helper is never shipped in the package.
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdint>
#include <stdexcept>
namespace {
struct Resource {
  WORD type;
  WORD id;
  std::wstring name;
  WORD language;
};
BOOL CALLBACK collect_language(HMODULE, LPCWSTR type, LPCWSTR name, WORD language,
                               LONG_PTR context) {
  auto &resources = *reinterpret_cast<std::vector<Resource> *>(context);
  resources.push_back(
      {static_cast<WORD>(reinterpret_cast<ULONG_PTR>(type)),
       IS_INTRESOURCE(name) ? static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name)) : WORD{},
       IS_INTRESOURCE(name) ? std::wstring{} : name, language});
  return TRUE;
}
BOOL CALLBACK collect_name(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR context) {
  return EnumResourceLanguagesW(module, type, name, collect_language, context);
}
LPCWSTR resource_name(const Resource &resource) {
  return resource.name.empty() ? MAKEINTRESOURCEW(resource.id) : resource.name.c_str();
}
unsigned word(const std::vector<unsigned char> &bytes, std::size_t at) {
  return unsigned(bytes.at(at)) | unsigned(bytes.at(at + 1)) << 8;
}
std::uint32_t dword(const std::vector<unsigned char> &bytes, std::size_t at) {
  return word(bytes, at) | std::uint32_t(word(bytes, at + 2)) << 16;
}
void append(std::vector<unsigned char> &bytes, unsigned value) {
  bytes.push_back(value & 255);
  bytes.push_back((value >> 8) & 255);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("Supply copied bootstrap and ICO paths");
    const auto file = std::filesystem::path(argv[2]);
    if (std::filesystem::file_size(file) > 1024 * 1024)
      throw std::runtime_error("Icon is oversized");
    std::ifstream input(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() < 6 || word(bytes, 0) != 0 || word(bytes, 2) != 1)
      throw std::runtime_error("Invalid ICO header");
    const auto count = word(bytes, 4);
    if (!count || count > 256 || bytes.size() < 6 + 16 * count)
      throw std::runtime_error("Invalid ICO entries");
    std::vector<unsigned char> group{0, 0, 1, 0};
    append(group, count);
    for (unsigned i = 0; i < count; ++i) {
      const auto at = 6 + 16 * i, size = dword(bytes, at + 8), offset = dword(bytes, at + 12);
      if (offset > bytes.size() || size > bytes.size() - offset || !size)
        throw std::runtime_error("Invalid ICO image range");
      group.insert(group.end(), bytes.begin() + at, bytes.begin() + at + 12);
      append(group, i + 1);
    }
    // Replace every vendor icon language variant. Leaving the en-US images in
    // place can make Explorer prefer Chromium art over our neutral PNG entries.
    std::vector<Resource> old_icons;
    const auto module =
        LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module)
      throw std::runtime_error("Could not inspect copied bootstrap resources");
    bool inspected = true;
    for (const auto type : {RT_ICON, RT_GROUP_ICON})
      if (!EnumResourceNamesW(module, type, collect_name, reinterpret_cast<LONG_PTR>(&old_icons)) &&
          GetLastError() != ERROR_RESOURCE_TYPE_NOT_FOUND)
        inspected = false;
    FreeLibrary(module);
    if (!inspected)
      throw std::runtime_error("Could not enumerate copied bootstrap icons");
    const auto handle = BeginUpdateResourceW(argv[1], FALSE);
    if (!handle)
      throw std::runtime_error("Could not open copied bootstrap resources");
    bool success = true;
    for (const auto &resource : old_icons)
      if (success)
        success = UpdateResourceW(handle, MAKEINTRESOURCEW(resource.type), resource_name(resource),
                                  resource.language, nullptr, 0) != FALSE;
    for (unsigned i = 0; i < count && success; ++i) {
      const auto at = 6 + 16 * i;
      success =
          UpdateResourceW(handle, RT_ICON, MAKEINTRESOURCEW(i + 1), 0,
                          bytes.data() + dword(bytes, at + 12), dword(bytes, at + 8)) != FALSE;
    }
    if (success)
      success = UpdateResourceW(handle, RT_GROUP_ICON, MAKEINTRESOURCEW(1), 0, group.data(),
                                static_cast<DWORD>(group.size())) != FALSE;
    // Keep resource names referenced by vendor dialogs, with Xenon images too.
    for (const auto &resource : old_icons)
      if (success && resource.type == reinterpret_cast<ULONG_PTR>(RT_GROUP_ICON))
        success = UpdateResourceW(handle, RT_GROUP_ICON, resource_name(resource), resource.language,
                                  group.data(), static_cast<DWORD>(group.size())) != FALSE;
    const auto update_error = GetLastError();
    if (!EndUpdateResourceW(handle, !success))
      throw std::runtime_error("Could not commit Xenon bootstrap icon: Win32 " +
                               std::to_string(GetLastError()));
    if (!success)
      throw std::runtime_error("Could not write Xenon bootstrap icon: Win32 " +
                               std::to_string(update_error));
    std::cout << "Xenon icon embedded in CEF bootstrap\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
