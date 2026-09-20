#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

namespace {

void be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 24);
  bytes[offset + 1] = static_cast<std::byte>(value >> 16);
  bytes[offset + 2] = static_cast<std::byte>(value >> 8);
  bytes[offset + 3] = static_cast<std::byte>(value);
}

void le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
  bytes[offset + 2] = static_cast<std::byte>(value >> 16);
  bytes[offset + 3] = static_cast<std::byte>(value >> 24);
}

std::vector<std::byte> make_xex() {
  // header_size must leave room for a full XexSecurityInfo (0x184 bytes,
  // see xex_loader.cpp) between the optional-header table (starting at
  // 0x18) and the PE body - production XEX2 parsing now requires real
  // security info (security_offset != 0), matching every retail XEX.
  constexpr std::size_t header = 0x200;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x280;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::uint32_t load_address = 0x80000000u;
  std::vector<std::byte> bytes(0x800, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, header); be32(bytes, 0x10, security); be32(bytes, 0x14, 0);

  // XexSecurityInfo: only header_size/image_size/load_address are
  // non-zero; everything else (rsa_signature, digests, page descriptors,
  // ...) is fine left zeroed for this compression=None/encryption=None
  // fixture with no page-descriptor-driven protection overrides.
  be32(bytes, security + 0x000, 0x184);                                    // security header_size
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));  // image_size
  be32(bytes, security + 0x110, load_address);                             // load_address

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C); le16(bytes, coff + 2, 1);
  le16(bytes, coff + 0x10, 0xE0); le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, 0x1000);
  le32(bytes, optional + 0x1C, load_address);
  le32(bytes, optional + 0x38, 0x2000);
  bytes[section] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  le32(bytes, section + 4, 0x20);
  le32(bytes, section + 0x0C, 0x1000);
  le32(bytes, section + 0x10, 0x20);
  le32(bytes, section + 0x14, 0x400);
  le32(bytes, section + 0x24, 0x60000020);
  // section's raw_pointer (0x400) is a file offset within the *effective
  // image* (the decompressed PE body, which for XEX_COMPRESSION_NONE starts
  // right at file offset `header`) - not within the outer XEX file - so the
  // actual bytes live at `header + 0x400`.
  be32(bytes, header + 0x400, 0x4E800020);
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "xenon_recomp_driver_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto input = root / "fixture.xex";
  const auto output = root / "generated";
  write_file(input, make_xex());

  xenon::recomp::DriverOptions options;
  options.input = input;
  options.output = output;
  xenon::recomp::AnalysisReport report;
  std::string error;
  assert(xenon::recomp::load_and_analyze(options, report, error));
  assert(report.functions.size() == 1);
  assert(report.functions.front().compiled);
  assert(xenon::recomp::generate_project(options, report, error));
  assert(std::filesystem::exists(output / "registry.cpp"));
  assert(std::filesystem::exists(output / "registry.hpp"));
  assert(std::filesystem::exists(output / "analysis.json"));
  assert(std::filesystem::exists(output / "manifest.txt"));
  assert(std::filesystem::exists(output / "CMakeLists.txt"));
  const auto first = std::filesystem::file_size(output / "functions" / "shard_000.cpp");
  assert(xenon::recomp::generate_project(options, report, error));
  assert(std::filesystem::file_size(output / "functions" / "shard_000.cpp") == first);
#ifdef XENON_SOURCE_ROOT
  const auto build = root / "build";
  const auto quote = [](const std::filesystem::path& value) {
    return std::string("\"") + value.string() + "\"";
  };
  const auto configure = "cmake -S " + quote(output) + " -B " + quote(build) +
                         " -DXENON_RECOMP_ROOT=" + quote(XENON_SOURCE_ROOT);
  assert(std::system(configure.c_str()) == 0);
  const auto compile = "cmake --build " + quote(build) + " --target xenon_game";
  assert(std::system(compile.c_str()) == 0);
#endif
  std::filesystem::remove_all(root);
  return 0;
}
