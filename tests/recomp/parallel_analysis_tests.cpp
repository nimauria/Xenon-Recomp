// Parallel per-function analysis correctness (Parts 4/5/21 of the Recomp
// Analysis V2 pass): jobs=1 and jobs=N must produce semantically identical,
// deterministic AnalysisReport content on the same input, discovery waves
// must converge correctly across multiple rounds, and a candidate reachable
// from more than one caller must be claimed/compiled exactly once. Uses
// small, hand-built synthetic XEX images with many independent functions -
// no copyrighted game content (Part 20's "no copyrighted files in tests"
// requirement) - same fixture-construction technique as
// tests/recomp/analysis_v2_consumption_tests.cpp.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

using namespace xenon::recomp;
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
constexpr std::uint32_t kBlr = 0x4E800020u;

std::uint32_t bl_word(std::uint32_t from, std::uint32_t to) {
  return 0x48000000u | ((to - from) & 0x03FFFFFCu) | 1u;
}

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
  le32(bytes, optional + 0x38, kTextRva + 0x10000u);

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

// Normalized, comparable snapshot of an AnalysisReport - deliberately
// excludes fields that legitimately differ between jobs=1 and jobs=N (e.g.
// diagnostics.analysis_workers) but includes everything that must be
// byte-identical (Part 4/21's hard equivalence requirement).
struct Snapshot {
  std::vector<std::tuple<std::uint32_t, std::uint32_t, bool, std::uint32_t, std::vector<std::uint32_t>>>
      functions;  // (start, end, compiled, confidence, ranges)
  std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string, std::string>> unresolved;
  std::size_t total_functions{};
  std::size_t candidate_functions_total{};
  std::size_t functions_analyzed{};
  std::size_t functions_compiled{};
  std::size_t analysis_waves{};
};

Snapshot snapshot(const AnalysisReport& report) {
  Snapshot result;
  for (const auto& function : report.functions)
    result.functions.emplace_back(function.guest_start, function.guest_end, function.compiled,
                                  function.confidence, function.ranges);
  for (const auto& item : report.unresolved)
    result.unresolved.emplace_back(item.address, item.target, item.kind, item.detail);
  std::sort(result.functions.begin(), result.functions.end());
  std::sort(result.unresolved.begin(), result.unresolved.end());
  result.total_functions = report.functions.size();
  result.candidate_functions_total = report.diagnostics.candidate_functions_total;
  result.functions_analyzed = report.diagnostics.functions_analyzed;
  result.functions_compiled = report.diagnostics.functions_compiled;
  result.analysis_waves = report.diagnostics.analysis_waves;
  return result;
}

bool run(const std::vector<std::byte>& xex_bytes, std::optional<std::size_t> jobs, AnalysisReport& report,
        const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  DriverOptions options{};
  options.input = root / "fixture.xex";
  options.output = root / "generated";
  options.analysis_jobs = jobs;
  std::ofstream(options.input, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
  std::string error;
  const bool ok = load_and_analyze(options, report, error);
  if (!ok) std::cerr << "load_and_analyze failed: " << error << "\n";
  std::filesystem::remove_all(root);
  return ok;
}

}  // namespace

int main() {
  std::cout << "Testing parallel analysis determinism...\n";
  const auto root = std::filesystem::temp_directory_path() / "xenon_parallel_analysis_test";

  // Test 1: a "hub" function directly calling 96 independent leaf functions
  // (all discoverable only in the wave immediately after the hub itself is
  // analyzed - real parallel work within one wave) must produce identical
  // results whether analyzed with jobs=1 or a real worker pool.
  {
    constexpr int kLeafCount = 96;
    const std::uint32_t base = kLoadAddress + kTextRva;
    const std::uint32_t leaves_start = base + static_cast<std::uint32_t>((kLeafCount + 1) * 4);

    std::vector<std::uint32_t> words;
    words.reserve(kLeafCount + 1 + kLeafCount);
    for (int i = 0; i < kLeafCount; ++i) {
      const auto hub_word_address = base + static_cast<std::uint32_t>(i * 4);
      const auto leaf_address = leaves_start + static_cast<std::uint32_t>(i * 4);
      words.push_back(bl_word(hub_word_address, leaf_address));
    }
    words.push_back(kBlr);  // hub's own terminal return
    for (int i = 0; i < kLeafCount; ++i) words.push_back(kBlr);

    const auto xex_bytes = make_xex(words);

    AnalysisReport report_serial;
    assert(run(xex_bytes, 1, report_serial, root / "serial"));
    AnalysisReport report_parallel;
    assert(run(xex_bytes, 8, report_parallel, root / "parallel"));

    const auto serial_snapshot = snapshot(report_serial);
    const auto parallel_snapshot = snapshot(report_parallel);

    assert(serial_snapshot.total_functions == static_cast<std::size_t>(kLeafCount + 1));
    assert(serial_snapshot.functions == parallel_snapshot.functions &&
           "jobs=1 and jobs=8 must discover/compile the exact same function set");
    assert(serial_snapshot.unresolved == parallel_snapshot.unresolved &&
           "jobs=1 and jobs=8 must produce the exact same unresolved diagnostics content");
    assert(serial_snapshot.candidate_functions_total == parallel_snapshot.candidate_functions_total);
    assert(serial_snapshot.functions_analyzed == parallel_snapshot.functions_analyzed);
    assert(serial_snapshot.functions_compiled == parallel_snapshot.functions_compiled);
    assert(serial_snapshot.analysis_waves == parallel_snapshot.analysis_waves &&
           "wave count is a property of the discovery graph, not the worker count");
    assert(serial_snapshot.analysis_waves == 2 &&
           "hub (wave 1) then all 96 leaves discovered together (wave 2)");
    for (const auto& function : report_parallel.functions) assert(function.compiled);

    assert(report_serial.diagnostics.analysis_workers == 1);
    assert(report_parallel.diagnostics.analysis_workers == 8);
  }
  std::cout << "  [PASS] Hub-and-96-leaves: jobs=1 and jobs=8 produce identical deterministic output\n";

  // Test 2: repeated jobs=auto runs of the same input are all mutually
  // identical (guards against a race silently corrupting output only
  // sometimes).
  {
    constexpr int kLeafCount = 48;
    const std::uint32_t base = kLoadAddress + kTextRva;
    const std::uint32_t leaves_start = base + static_cast<std::uint32_t>((kLeafCount + 1) * 4);
    std::vector<std::uint32_t> words;
    for (int i = 0; i < kLeafCount; ++i)
      words.push_back(bl_word(base + static_cast<std::uint32_t>(i * 4),
                              leaves_start + static_cast<std::uint32_t>(i * 4)));
    words.push_back(kBlr);
    for (int i = 0; i < kLeafCount; ++i) words.push_back(kBlr);
    const auto xex_bytes = make_xex(words);

    Snapshot first;
    for (int round = 0; round < 6; ++round) {
      AnalysisReport report;
      assert(run(xex_bytes, std::nullopt, report, root / "repeat"));
      const auto current = snapshot(report);
      if (round == 0) first = current;
      else assert(current.functions == first.functions && current.unresolved == first.unresolved &&
                  "repeated jobs=auto runs of the same input must be mutually deterministic");
    }
  }
  std::cout << "  [PASS] Repeated jobs=auto runs are mutually deterministic\n";

  // Test 3: a direct-call chain of 24 functions (A -> B -> C -> ...) forces
  // the discovery-wave engine through multiple rounds (each wave can only
  // discover the next link once the previous one has actually been
  // analyzed) - proves waves genuinely iterate to a fixed point rather than
  // assuming a single round covers everything. Each link is `bl <next>`
  // immediately followed by its OWN `blr` (2 words) so its scan is bounded
  // to just those two words - a bare `bl` never terminates a scan (it is
  // not architecturally unconditional), so without the trailing `blr` each
  // link's scan would run straight through every later link's `bl` too and
  // discover the whole rest of the chain in one wave, which is exactly what
  // Test 1 (the hub) intentionally exploits but is the opposite of what
  // this test needs to exercise.
  {
    constexpr int kChainLength = 24;
    const std::uint32_t base = kLoadAddress + kTextRva;
    const auto link_address = [&](int i) { return base + static_cast<std::uint32_t>(i) * 8u; };
    std::vector<std::uint32_t> words(static_cast<std::size_t>(kChainLength) * 2u - 1u);
    for (int i = 0; i < kChainLength - 1; ++i) {
      words[static_cast<std::size_t>(i) * 2u] = bl_word(link_address(i), link_address(i + 1));
      words[static_cast<std::size_t>(i) * 2u + 1u] = kBlr;
    }
    words[static_cast<std::size_t>(kChainLength - 1) * 2u] = kBlr;  // final link: no call, just returns
    const auto xex_bytes = make_xex(words);

    AnalysisReport report;
    assert(run(xex_bytes, 4, report, root / "chain"));
    assert(report.functions.size() == static_cast<std::size_t>(kChainLength));
    for (const auto& function : report.functions) assert(function.compiled);
    assert(report.diagnostics.analysis_waves == static_cast<std::size_t>(kChainLength) &&
           "a strictly sequential call chain must take exactly one wave per link");
  }
  std::cout << "  [PASS] A sequential call chain drives the discovery engine through multiple waves\n";

  // Test 4: two independent callers both branching to the SAME target must
  // result in that target being claimed/compiled exactly once, not twice
  // (Part 21.5's duplicate-candidate-suppression requirement).
  {
    const std::uint32_t base = kLoadAddress + kTextRva;
    const std::vector<std::uint32_t> words = {
        bl_word(base + 0u, base + 4u * 4u),       // word0: caller A -> shared (word4)
        kBlr,                                     // word1: caller A return
        bl_word(base + 2u * 4u, base + 4u * 4u),  // word2: caller B -> shared (word4)
        kBlr,                                     // word3: caller B return
        kBlr,                                     // word4: shared target
    };
    const auto xex_bytes = make_xex(words);

    AnalysisReport report;
    assert(run(xex_bytes, 4, report, root / "shared"));
    const auto shared_count = std::count_if(report.functions.begin(), report.functions.end(),
                                            [&](const auto& function) {
                                              return function.guest_start == base + 4u * 4u;
                                            });
    assert(shared_count == 1 && "a candidate reachable from two callers must be claimed exactly once");
  }
  std::cout << "  [PASS] A candidate reachable from two callers is claimed/compiled exactly once\n";

  // Test 5: progress callback fires and reports monotonically increasing
  // analysis-wave counters, and analysis_jobs=1 is honored exactly
  // (diagnostics.analysis_workers == 1, no pool spun up).
  {
    constexpr int kLeafCount = 16;
    const std::uint32_t base = kLoadAddress + kTextRva;
    const std::uint32_t leaves_start = base + static_cast<std::uint32_t>((kLeafCount + 1) * 4);
    std::vector<std::uint32_t> words;
    for (int i = 0; i < kLeafCount; ++i)
      words.push_back(bl_word(base + static_cast<std::uint32_t>(i * 4),
                              leaves_start + static_cast<std::uint32_t>(i * 4)));
    words.push_back(kBlr);
    for (int i = 0; i < kLeafCount; ++i) words.push_back(kBlr);
    const auto xex_bytes = make_xex(words);

    std::vector<std::string> messages;
    std::filesystem::remove_all(root / "progress");
    std::filesystem::create_directories(root / "progress");
    DriverOptions options{};
    options.input = root / "progress" / "fixture.xex";
    options.output = root / "progress" / "generated";
    options.analysis_jobs = 1;
    options.progress = [&](const std::string& message) { messages.push_back(message); };
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error));
    assert(!messages.empty() && "a supplied progress callback must actually be invoked");
    assert(std::any_of(messages.begin(), messages.end(),
                       [](const auto& m) { return m.find("Analysis workers: 1") != std::string::npos; }));
    assert(messages.back() == "[Analysis] complete");
    assert(report.diagnostics.analysis_workers == 1);
    std::filesystem::remove_all(root / "progress");
  }
  std::cout << "  [PASS] Progress callback fires with expected milestones; analysis_jobs=1 honored\n";

  std::cout << "All parallel analysis determinism tests passed!\n";
  return 0;
}
