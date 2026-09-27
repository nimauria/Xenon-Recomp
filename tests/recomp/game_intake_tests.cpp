// Gen 11 (Autonomous Game Intake) unit tests: discover_game_executables()/
// read_game_executable_bytes() over a directory, a loose .xex file, and a
// synthetic Xbox 360 disc image, plus GameCompilationGraph's JSON shape.
// Mirrors tests/filesystem/filesystem_tests.cpp's synthetic-GDFX-image
// construction technique (write_gdfx_entry et al.) so this exercises the
// real GdfxImageSource format, not a mock.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/filesystem/types.hpp"
#include "xenon/recomp/game_intake.hpp"

using namespace xenon::recomp;
namespace fs = xenon::filesystem;

namespace {

void write_le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}

void write_le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset + 0] = static_cast<std::byte>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  bytes[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xFFu);
  bytes[offset + 3] = static_cast<std::byte>((value >> 24u) & 0xFFu);
}

void write_gdfx_entry(std::vector<std::byte>& image, std::size_t offset, std::uint16_t left,
                      std::uint16_t right, std::uint32_t sector, std::uint32_t length,
                      std::uint8_t attributes, std::string_view name) {
  write_le16(image, offset + 0, left);
  write_le16(image, offset + 2, right);
  write_le32(image, offset + 4, sector);
  write_le32(image, offset + 8, length);
  image[offset + 12] = static_cast<std::byte>(attributes);
  image[offset + 13] = static_cast<std::byte>(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    image[offset + 14 + i] = static_cast<std::byte>(static_cast<unsigned char>(name[i]));
  }
}

std::vector<std::byte> make_minimal_xex(std::string_view marker) {
  // A byte-distinguishable, structurally-minimal blob. discover/read never
  // parse XEX headers themselves (that happens later, in xenon-prepare), so
  // these tests only need distinct, round-trippable bytes.
  std::vector<std::byte> bytes(0x20);
  bytes[0] = std::byte{'X'};
  bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'};
  bytes[3] = std::byte{'2'};
  for (std::size_t i = 0; i < marker.size() && 4 + i < bytes.size(); ++i) {
    bytes[4 + i] = static_cast<std::byte>(static_cast<unsigned char>(marker[i]));
  }
  return bytes;
}

// A three-entry root directory: "Data" (subdirectory), "default.xex" and
// "update.xex" (a secondary executable) - and one file nested inside "Data"
// that must NOT be discovered. parse_entry_tree() visits every node
// reachable via left/right ordinals without itself validating BST order, so
// a simple right-leaning chain is sufficient; only GdfxImageSource's own
// tests need a genuinely balanced tree.
std::vector<std::byte> make_two_xex_gdfx() {
  constexpr std::size_t kSector = 2048;
  constexpr std::uint32_t kRootSector = 40;
  constexpr std::uint32_t kDataSector = 41;
  constexpr std::uint32_t kXexSector = 50;
  constexpr std::uint32_t kUpdateXexSector = 51;
  constexpr std::uint32_t kIgnoredSector = 52;
  constexpr std::uint32_t kRootSize = 128;
  constexpr std::uint32_t kDataDirectorySize = 32;

  std::vector<std::byte> image(64 * kSector);
  constexpr std::size_t descriptor = 32 * kSector;
  constexpr std::string_view magic = "MICROSOFT*XBOX*MEDIA";
  for (std::size_t i = 0; i < magic.size(); ++i) {
    image[descriptor + i] = static_cast<std::byte>(static_cast<unsigned char>(magic[i]));
  }
  write_le32(image, descriptor + 20, kRootSector);
  write_le32(image, descriptor + 24, kRootSize);

  const auto root = kRootSector * kSector;
  // default.xex (11 chars): 14+11=25 -> padded to 28.
  write_gdfx_entry(image, root, 0, 7, kXexSector, 0x20, 0x20, "default.xex");
  // Data (4 chars): 14+4=18 -> padded to 20. Its own right ordinal (12*4=48)
  // reaches "update.xex" so the whole tree is still visited from the root.
  write_gdfx_entry(image, root + 28, 0, 12, kDataSector, kDataDirectorySize,
                   fs::FileAttributeDirectory, "Data");
  // update.xex (10 chars): 14+10=24 -> padded to 24.
  write_gdfx_entry(image, root + 48, 0, 0, kUpdateXexSector, 0x20, 0x20, "update.xex");

  const auto data = kDataSector * kSector;
  write_gdfx_entry(image, data, 0, 0, kIgnoredSector, 6, 0x20, "ignored.bin");

  const auto default_bytes = make_minimal_xex("default");
  std::copy(default_bytes.begin(), default_bytes.end(), image.begin() + kXexSector * kSector);
  const auto update_bytes = make_minimal_xex("update!!");
  std::copy(update_bytes.begin(), update_bytes.end(), image.begin() + kUpdateXexSector * kSector);
  constexpr std::string_view ignored = "not-a-xex";
  for (std::size_t i = 0; i < ignored.size(); ++i) {
    image[kIgnoredSector * kSector + i] = static_cast<std::byte>(static_cast<unsigned char>(ignored[i]));
  }
  return image;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool has_relative_path(const std::vector<DiscoveredExecutable>& modules, const std::string& path) {
  return std::any_of(modules.begin(), modules.end(),
                     [&](const auto& m) { return m.relative_path == path; });
}

}  // namespace

int main() {
  std::cout << "Testing Gen 11 game intake discovery...\n";
  const auto root = std::filesystem::temp_directory_path() / "xenon_game_intake_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  // 1. A directory with only the mandatory default.xex yields exactly one
  // discovered executable - byte-identical to every prior single-module
  // xenon-prepare invocation.
  {
    const auto dir = root / "single";
    write_file(dir / "default.xex", make_minimal_xex("only"));
    std::vector<DiscoveredExecutable> modules;
    std::string error;
    assert(discover_game_executables(dir, modules, error) && error.empty());
    assert(modules.size() == 1);
    assert(modules.front().relative_path == "default.xex");
    assert(modules.front().is_primary);
    std::vector<std::byte> bytes;
    assert(read_game_executable_bytes(dir, modules.front().relative_path, bytes, error));
    assert(bytes == make_minimal_xex("only"));
  }
  std::cout << "  [PASS] A directory with only default.xex discovers exactly one executable\n";

  // 2. A directory with default.xex plus additional nested .xex files
  // discovers all of them, primary first, secondaries sorted deterministically.
  {
    const auto dir = root / "multi";
    write_file(dir / "default.xex", make_minimal_xex("primary"));
    write_file(dir / "media" / "zzz_last.xex", make_minimal_xex("z"));
    write_file(dir / "media" / "aaa_first.xex", make_minimal_xex("a"));
    write_file(dir / "notes.txt", {});  // never mistaken for an executable
    std::vector<DiscoveredExecutable> modules;
    std::string error;
    assert(discover_game_executables(dir, modules, error) && error.empty());
    assert(modules.size() == 3);
    assert(modules[0].relative_path == "default.xex" && modules[0].is_primary);
    assert(modules[1].relative_path == "media/aaa_first.xex" && !modules[1].is_primary);
    assert(modules[2].relative_path == "media/zzz_last.xex" && !modules[2].is_primary);
    std::vector<std::byte> primary_bytes, secondary_bytes;
    assert(read_game_executable_bytes(dir, modules[0].relative_path, primary_bytes, error));
    assert(primary_bytes == make_minimal_xex("primary"));
    assert(read_game_executable_bytes(dir, modules[1].relative_path, secondary_bytes, error));
    assert(secondary_bytes == make_minimal_xex("a"));
  }
  std::cout << "  [PASS] A directory with additional nested .xex files discovers all of them, sorted\n";

  // 3. A directory missing the mandatory root default.xex is a hard error,
  // matching every prior single-module xenon-prepare invocation exactly.
  {
    const auto dir = root / "missing_primary";
    write_file(dir / "update.xex", make_minimal_xex("orphan"));
    std::vector<DiscoveredExecutable> modules;
    std::string error;
    assert(!discover_game_executables(dir, modules, error));
    assert(!error.empty());
  }
  std::cout << "  [PASS] A directory with no root default.xex is a hard discovery error\n";

  // 4. A loose .xex file always yields exactly one primary executable.
  {
    const auto file = root / "loose.xex";
    write_file(file, make_minimal_xex("loose"));
    std::vector<DiscoveredExecutable> modules;
    std::string error;
    assert(discover_game_executables(file, modules, error) && error.empty());
    assert(modules.size() == 1);
    assert(modules.front().is_primary);
    std::vector<std::byte> bytes;
    assert(read_game_executable_bytes(file, modules.front().relative_path, bytes, error));
    assert(bytes == make_minimal_xex("loose"));
  }
  std::cout << "  [PASS] A loose .xex file always discovers exactly one primary executable\n";

  // 5. An unrecognized content source is a clear, reported error.
  {
    const auto file = root / "notes.md";
    write_file(file, {});
    std::vector<DiscoveredExecutable> modules;
    std::string error;
    assert(!discover_game_executables(file, modules, error));
    assert(!error.empty());
  }
  std::cout << "  [PASS] An unrecognized content source is a clear discovery error\n";

  // 6. A synthetic Xbox 360 disc image with a root default.xex and a
  // secondary update.xex nested next to an unrelated subdirectory discovers
  // both, ignores the non-.xex file, and reads each module's exact bytes
  // straight out of the image (never extracted to a temp file).
  {
    const auto image_path = root / "disc.iso";
    write_file(image_path, make_two_xex_gdfx());
    std::vector<DiscoveredExecutable> modules;
    std::string error;
    assert(discover_game_executables(image_path, modules, error) && error.empty());
    assert(modules.size() == 2);
    assert(modules[0].relative_path == "default.xex" && modules[0].is_primary);
    assert(modules[1].relative_path == "update.xex" && !modules[1].is_primary);
    assert(!has_relative_path(modules, "Data/ignored.bin"));

    std::vector<std::byte> primary_bytes, secondary_bytes;
    assert(read_game_executable_bytes(image_path, "default.xex", primary_bytes, error));
    assert(std::equal(primary_bytes.begin(), primary_bytes.begin() + 0x20,
                      make_minimal_xex("default").begin()));
    assert(read_game_executable_bytes(image_path, "update.xex", secondary_bytes, error));
    assert(std::equal(secondary_bytes.begin(), secondary_bytes.begin() + 0x20,
                      make_minimal_xex("update!!").begin()));
  }
  std::cout << "  [PASS] A disc image with a secondary XEX module discovers and reads both\n";

  // 7. GameCompilationGraph's JSON shape is stable and carries every field a
  // caller (launcher, diagnostic tool) needs per module.
  {
    GameCompilationGraph graph;
    ModuleCompilationRecord primary;
    primary.relative_path = "default.xex";
    primary.is_primary = true;
    primary.status = ModulePreparationStatus::Prepared;
    primary.title_id = "41560855";
    primary.native_extension_path = "/cache/entries/abc/xenon_game_module.dll";
    graph.modules.push_back(primary);
    ModuleCompilationRecord secondary;
    secondary.relative_path = "update.xex";
    secondary.status = ModulePreparationStatus::Failed;
    secondary.error = "unsupported PPC instruction";
    secondary.hints_unavailable = true;
    graph.modules.push_back(secondary);

    const auto json_text = graph.to_json().dump();
    assert(json_text.find("\"schemaVersion\"") != std::string::npos);
    assert(json_text.find("\"default.xex\"") != std::string::npos);
    assert(json_text.find("\"Prepared\"") != std::string::npos);
    assert(json_text.find("\"update.xex\"") != std::string::npos);
    assert(json_text.find("\"Failed\"") != std::string::npos);
    assert(json_text.find("unsupported PPC instruction") != std::string::npos);
  }
  std::cout << "  [PASS] GameCompilationGraph serializes every module's outcome to JSON\n";

  std::filesystem::remove_all(root);
  std::cout << "All Gen 11 game intake discovery tests passed!\n";
  return 0;
}
