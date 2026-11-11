// xenon-extract: extracts an Xbox 360 game disc image (GDFX/XDVDFS .iso, or
// a .dvd descriptor naming one) into a host folder with Xenon's own
// filesystem layer (GdfxImageSource + materialize_content_source), so a
// title's extracted content matches exactly what the runtime would read.
//
//   xenon-extract <image.iso|image.dvd> <destination> [--overwrite]
//   xenon-extract <image.iso|image.dvd> --list
//
// The destination must be empty or absent unless --overwrite is given.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "xenon/filesystem/content_materializer.hpp"
#include "xenon/filesystem/gdfx_image_source.hpp"

namespace fs = std::filesystem;
using xenon::filesystem::DirectoryEntry;
using xenon::filesystem::FsError;
using xenon::filesystem::GdfxImageSource;

namespace {

struct Totals {
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uint64_t bytes{};
};

FsError walk(const GdfxImageSource& source, const std::string& path, bool print, Totals& totals) {
  std::vector<DirectoryEntry> entries;
  if (const auto error = source.list(path, entries); error != FsError::None) return error;
  for (const auto& entry : entries) {
    const auto child = path.empty() ? entry.name : path + "\\" + entry.name;
    if (entry.info.is_directory) {
      ++totals.directories;
      if (print) std::printf("%14s  %s\\\n", "<dir>", child.c_str());
      if (const auto error = walk(source, child, print, totals); error != FsError::None) return error;
    } else {
      ++totals.files;
      totals.bytes += entry.info.size;
      if (print) std::printf("%14llu  %s\n", static_cast<unsigned long long>(entry.info.size), child.c_str());
    }
  }
  return FsError::None;
}

int fail(const char* what, FsError error) {
  const auto name = xenon::filesystem::to_string(error);
  std::fprintf(stderr, "xenon-extract: %s: %.*s\n", what, static_cast<int>(name.size()), name.data());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: xenon-extract <image.iso|image.dvd> <destination> [--overwrite]\n"
                 "       xenon-extract <image.iso|image.dvd> --list\n");
    return 2;
  }
  const fs::path input = fs::u8path(argv[1]);
  const std::string_view second = argv[2];
  const bool list_only = second == "--list";
  const bool overwrite = argc > 3 && std::string_view(argv[3]) == "--overwrite";

  fs::path image;
  if (const auto error = xenon::filesystem::resolve_gdfx_image_path(input, image); error != FsError::None)
    return fail("cannot resolve the image", error);
  GdfxImageSource source(image);
  if (const auto error = source.initialize(); error != FsError::None)
    return fail("not a readable Xbox game disc image", error);
  const auto& info = source.info();
  std::printf("image %s\n  %llu bytes, game partition at 0x%llX, root sector 0x%X (%u bytes)\n",
              image.u8string().c_str(), static_cast<unsigned long long>(info.image_size),
              static_cast<unsigned long long>(info.game_partition_offset), info.root_sector,
              info.root_size);

  Totals totals;
  if (const auto error = walk(source, {}, list_only, totals); error != FsError::None)
    return fail("cannot list the image", error);
  std::printf("  %llu files in %llu folders, %llu bytes\n",
              static_cast<unsigned long long>(totals.files),
              static_cast<unsigned long long>(totals.directories),
              static_cast<unsigned long long>(totals.bytes));
  if (list_only) return 0;

  const fs::path destination = fs::u8path(argv[2]);
  std::error_code ec;
  if (fs::exists(destination, ec) && !fs::is_empty(destination, ec) && !overwrite) {
    std::fprintf(stderr, "xenon-extract: %s is not empty (pass --overwrite to extract over it)\n",
                 destination.u8string().c_str());
    return 1;
  }
  const auto started = std::chrono::steady_clock::now();
  std::printf("extracting to %s\n", destination.u8string().c_str());
  std::fflush(stdout);
  if (const auto error = xenon::filesystem::materialize_content_source(source, destination);
      error != FsError::None)
    return fail("extraction failed", error);
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("done: %llu files, %llu bytes in %.1f s\n",
              static_cast<unsigned long long>(totals.files),
              static_cast<unsigned long long>(totals.bytes), seconds);
  return 0;
}
