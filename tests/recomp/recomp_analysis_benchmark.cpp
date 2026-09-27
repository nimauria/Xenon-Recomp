// Synthetic Recomp Analysis V2 benchmark (Part 20 of the pass). Exercises
// thousands of independent functions - the shape a real large title (e.g.
// Project Gracemeria's ~10,527 known Ace Combat 6 function starts) presents
// to the analyzer - without any copyrighted game content: every function
// body here is a run of `ori r0,r0,0` (a real, safe, valid PPC instruction)
// followed by `blr`, and every function start is supplied the way a real
// title's revision profile would (explicit FunctionHint entries with known
// end addresses), so the analyzer takes the same "hint-seeded, no inference
// needed" path Project Gracemeria's real hint set exercises.
//
// Not a correctness test (no ctest registration - see CMakeLists.txt's
// XENON_BUILD_BENCHMARKS gate, matching tests/memory/memory_benchmarks.cpp's
// pattern) - though it does assert jobs=1 and jobs=auto discover/compile the
// identical function set, since a benchmark that silently regressed
// correctness while getting faster would be worse than useless.
//
// Usage: xenon_recomp_analysis_benchmark [function_count] [words_per_function]

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

using namespace xenon::recomp;
using namespace xenon::recomp::analysis;
namespace xbox = xenon::xbox;

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

constexpr std::uint32_t kLoadAddress = 0x80000000u;
constexpr std::uint32_t kTextRva = 0x10000u;
constexpr std::uint32_t kNop = 0x60000000u;  // ori r0,r0,0
constexpr std::uint32_t kBlr = 0x4E800020u;

std::vector<std::byte> make_xex(const std::vector<std::uint32_t>& text_words) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  const std::size_t file_size = header + text_raw + text_words.size() * 4u + 0x100u;

  std::vector<std::byte> bytes(file_size, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));
  be32(bytes, 0x14, 0);

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 1);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);
  le32(bytes, optional + 0x1C, kLoadAddress);
  le32(bytes, optional + 0x38, kTextRva + static_cast<std::uint32_t>(text_words.size() * 4u) + 0x10000u);

  const auto text_size = static_cast<std::uint32_t>(text_words.size() * 4u + 0x40u);
  bytes[section + 0] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  le32(bytes, section + 4, text_size);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, text_size);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);

  const std::size_t text_file_base = header + text_raw;
  for (std::size_t i = 0; i < text_words.size(); ++i) be32(bytes, text_file_base + i * 4u, text_words[i]);
  return bytes;
}

struct Timing {
  std::int64_t wall_ms{};
  std::size_t functions{};
  std::size_t compiled{};
  std::size_t waves{};
  std::size_t workers{};
};

Timing run_once(const std::vector<std::byte>& xex_bytes, const AnalysisHintSetV2& hints,
                std::optional<std::size_t> jobs, const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  DriverOptions options{};
  options.input = root / "fixture.xex";
  options.output = root / "generated";
  options.analysis_jobs = jobs;
  options.codegen_jobs = jobs;
  options.hint_set_v2 = hints;
  std::ofstream(options.input, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

  AnalysisReport report;
  std::string error;
  const auto start = std::chrono::steady_clock::now();
  if (!load_and_analyze(options, report, error)) {
    std::cerr << "benchmark: load_and_analyze failed: " << error << "\n";
    std::exit(1);
  }
  const auto analysis_done = std::chrono::steady_clock::now();
  if (!generate_project(options, report, error)) {
    std::cerr << "benchmark: generate_project failed: " << error << "\n";
    std::exit(1);
  }
  const auto codegen_done = std::chrono::steady_clock::now();

  Timing timing;
  timing.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(codegen_done - start).count();
  timing.functions = report.functions.size();
  timing.compiled = report.diagnostics.functions_compiled;
  timing.waves = report.diagnostics.analysis_waves;
  timing.workers = report.diagnostics.analysis_workers;
  std::cout << "    analysis=" << std::chrono::duration_cast<std::chrono::milliseconds>(analysis_done - start).count()
            << "ms codegen=" << std::chrono::duration_cast<std::chrono::milliseconds>(codegen_done - analysis_done).count()
            << "ms functions=" << timing.functions << " compiled=" << timing.compiled
            << " waves=" << timing.waves << " workers=" << timing.workers << "\n";
  std::filesystem::remove_all(root);
  return timing;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t function_count = argc > 1 ? static_cast<std::size_t>(std::stoul(argv[1])) : 3000;
  const std::size_t words_per_function = argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2])) : 20;

  std::cout << "Xenon Recomp Analysis V2 synthetic benchmark\n"
            << "  function_count=" << function_count << " words_per_function=" << words_per_function << "\n";

  const std::uint32_t base = kLoadAddress + kTextRva;
  std::vector<std::uint32_t> words(function_count * words_per_function);
  AnalysisHintSetV2 hints{};
  hints.functions.reserve(function_count);
  for (std::size_t f = 0; f < function_count; ++f) {
    const auto function_start = base + static_cast<std::uint32_t>(f * words_per_function * 4u);
    for (std::size_t w = 0; w + 1 < words_per_function; ++w)
      words[f * words_per_function + w] = kNop;
    words[f * words_per_function + words_per_function - 1] = kBlr;

    FunctionHint hint{};
    hint.address = function_start;
    hint.end = function_start + static_cast<std::uint32_t>(words_per_function * 4u);
    hints.functions.push_back(hint);
  }
  const auto xex_bytes = make_xex(words);

  xbox::XexImage image{};
  std::string parse_error;
  if (!xbox::parse_xex_image(xex_bytes, image, &parse_error)) {
    std::cerr << "benchmark: failed to parse synthetic fixture: " << parse_error << "\n";
    return 1;
  }
  hints.identity = xbox::compute_effective_identity(image);

  const auto root = std::filesystem::temp_directory_path() / "xenon_recomp_analysis_benchmark";

  std::cout << "  [jobs=1]\n";
  const auto serial = run_once(xex_bytes, hints, 1, root / "serial");
  std::cout << "  [jobs=auto]\n";
  const auto parallel = run_once(xex_bytes, hints, std::nullopt, root / "parallel");

  assert(serial.functions == parallel.functions && "jobs=1 and jobs=auto must discover the same function set");
  assert(serial.compiled == parallel.compiled && "jobs=1 and jobs=auto must compile the same number of functions");
  assert(serial.compiled == function_count && "every hint-seeded function should compile in this fixture");

  const double speedup = parallel.wall_ms > 0
                             ? static_cast<double>(serial.wall_ms) / static_cast<double>(parallel.wall_ms)
                             : 0.0;
  std::cout << "\nResult: jobs=1 " << serial.wall_ms << "ms, jobs=auto (" << parallel.workers << " workers) "
            << parallel.wall_ms << "ms, speedup=" << std::fixed << std::setprecision(2) << speedup << "x\n";
  return 0;
}
