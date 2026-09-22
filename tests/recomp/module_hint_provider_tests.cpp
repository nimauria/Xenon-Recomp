// ModuleHintProvider production integration tests (Part 2/7 of the
// Gracemeria readiness pass): a synthetic two-revision module package on
// disk (Part 2.6's "test module"), correct revision selection, wrong-
// revision rejection, and proof the Recomp Driver actually calls through
// FileModuleHintProvider (DriverOptions::hint_provider_v2), not merely that
// the provider class exists.

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/recomp/analysis_schema_json.hpp"
#include "xenon/recomp/driver.hpp"
#include "xenon/recomp/module_hint_provider.hpp"

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
constexpr std::uint32_t kDataRva = 0x20000u;
constexpr std::uint32_t kBlr = 0x4E800020u;

// Builds a distinct XEX per `salt` (salt padded into unused header bytes),
// so two "revisions" of the "same" title are two genuinely different
// effective images with two different effective_image_hash values - exactly
// what a base executable vs. a title-update-patched executable looks like to
// this schema (see hint_set_matches_identity()).
std::vector<std::byte> make_xex(std::uint32_t salt) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  constexpr std::size_t data_raw = kDataRva;
  constexpr std::size_t file_size = header + data_raw + 0x40;

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
  le16(bytes, coff + 2, 2);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);
  le32(bytes, optional + 0x1C, kLoadAddress);
  le32(bytes, optional + 0x38, kDataRva + 0x10000u);

  bytes[section + 0] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  le32(bytes, section + 4, 0x40);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, 0x40);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);

  const std::size_t data_section = section + 0x28;
  bytes[data_section + 0] = std::byte{'.'}; bytes[data_section + 1] = std::byte{'d'};
  bytes[data_section + 2] = std::byte{'a'}; bytes[data_section + 3] = std::byte{'t'};
  bytes[data_section + 4] = std::byte{'a'};
  le32(bytes, data_section + 4, 0x10);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, 0x10);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040);

  const std::size_t text_file_base = header + text_raw;
  be32(bytes, text_file_base, kBlr);
  // `salt` distinguishes this "revision"'s bytes (and therefore its
  // effective_image_hash) without affecting how the entry function decodes -
  // stored a few words after the entry point's blr, well inside the
  // declared .text VirtualSize/SizeOfRawData (0x40) but never reached by the
  // decoder (a terminal blr already ended the entry function at word 0).
  be32(bytes, text_file_base + 0x10, salt);

  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

}  // namespace

int main() {
  std::cout << "Testing ModuleHintProvider production integration...\n";

  const auto module_root = std::filesystem::temp_directory_path() / "xenon_module_hint_provider_test";
  std::filesystem::remove_all(module_root);
  std::filesystem::create_directories(module_root / "revisions");

  // Two genuinely different "revisions" (e.g. base XEX vs. base+TU) of a
  // synthetic test module, each with DIFFERENT function hints and a
  // different native replacement - Part 2.6's requirement.
  const auto revision_a_bytes = make_xex(0xAAAAAAAAu);
  const auto revision_b_bytes = make_xex(0xBBBBBBBBu);
  xbox::XexImage image_a{}, image_b{};
  std::string parse_error;
  assert(xbox::parse_xex_image(revision_a_bytes, image_a, &parse_error));
  assert(xbox::parse_xex_image(revision_b_bytes, image_b, &parse_error));
  const auto identity_a = xbox::compute_effective_identity(image_a);
  const auto identity_b = xbox::compute_effective_identity(image_b);
  const auto hash_a = xbox::format_effective_image_hash(identity_a.effective_image_hash);
  const auto hash_b = xbox::format_effective_image_hash(identity_b.effective_image_hash);
  assert(hash_a != hash_b && "two differently-salted synthetic XEX images must hash differently");

  write_text_file(module_root / "manifest.json",
                  R"json({"moduleName": "xenon_test_module", "revisions": [")json" + hash_a +
                      R"json(", ")json" + hash_b + R"json("]})json");

  AnalysisHintSetV2 hints_a{};
  hints_a.module_name = "xenon_test_module";
  hints_a.identity = identity_a;
  FunctionHint fn_a{};
  fn_a.address = kLoadAddress + kTextRva;
  fn_a.name = "revision_a_entry";
  hints_a.functions.push_back(fn_a);
  hints_a.native_replacements.push_back(NativeReplacement{0x82000000u, NativeReplacementKind::Memcpy, "memcpy"});

  AnalysisHintSetV2 hints_b{};
  hints_b.module_name = "xenon_test_module";
  hints_b.identity = identity_b;
  FunctionHint fn_b{};
  fn_b.address = kLoadAddress + kTextRva;
  fn_b.name = "revision_b_entry";  // different name from revision A
  hints_b.functions.push_back(fn_b);
  SwitchTableHint table_b{};
  table_b.site = 0x82000010u;
  table_b.explicit_targets = {0x82000020u};
  hints_b.switches.push_back(table_b);  // a hint category revision A does not have

  std::filesystem::create_directories(module_root / "revisions" / hash_a);
  std::filesystem::create_directories(module_root / "revisions" / hash_b);
  write_text_file(module_root / "revisions" / hash_a / "analysis.json", to_json(hints_a).dump(2));
  write_text_file(module_root / "revisions" / hash_b / "analysis.json", to_json(hints_b).dump(2));

  const FileModuleHintProvider provider(module_root);
  assert(provider.manifest_loaded());
  assert(provider.module_name() == "xenon_test_module");

  // Test 1: correct provider selection for revision A.
  {
    AnalysisHintSetV2 resolved{};
    std::string error;
    assert(provider.provide(identity_a, resolved, error) && error.empty());
    assert(resolved.functions.size() == 1 && resolved.functions[0].name == "revision_a_entry");
    assert(resolved.native_replacements.size() == 1);
    assert(resolved.switches.empty());
  }
  std::cout << "  [PASS] Provider selects revision A's hints for revision A's identity\n";

  // Test 2: correct provider selection for revision B (distinctly different data).
  {
    AnalysisHintSetV2 resolved{};
    std::string error;
    assert(provider.provide(identity_b, resolved, error) && error.empty());
    assert(resolved.functions.size() == 1 && resolved.functions[0].name == "revision_b_entry");
    assert(resolved.native_replacements.empty());
    assert(resolved.switches.size() == 1);
  }
  std::cout << "  [PASS] Provider selects revision B's hints for revision B's identity\n";

  // Test 3: an identity with no matching revision on disk is a clear,
  // explicit failure - never the nearest-looking revision.
  {
    auto unknown_identity = identity_a;
    unknown_identity.effective_image_hash.fill(std::byte{0xEE});
    AnalysisHintSetV2 resolved{};
    std::string error;
    assert(!provider.provide(unknown_identity, resolved, error));
    assert(!error.empty());
    assert(error.find("xenon_test_module") != std::string::npos);
  }
  std::cout << "  [PASS] An unknown revision is rejected with a clear, specific error\n";

  // Test 4: a provider pointed at a directory with no manifest fails cleanly
  // (never crashes) on every call.
  {
    const auto empty_root = std::filesystem::temp_directory_path() / "xenon_module_hint_provider_test_empty";
    std::filesystem::remove_all(empty_root);
    std::filesystem::create_directories(empty_root);
    const FileModuleHintProvider empty_provider(empty_root);
    assert(!empty_provider.manifest_loaded());
    AnalysisHintSetV2 resolved{};
    std::string error;
    assert(!empty_provider.provide(identity_a, resolved, error));
    assert(!error.empty());
    std::filesystem::remove_all(empty_root);
  }
  std::cout << "  [PASS] A module with no manifest fails cleanly rather than crashing\n";

  // Test 5: the Recomp Driver actually calls through hint_provider_v2 - real
  // end-to-end proof, not just that FileModuleHintProvider works in
  // isolation. Analyzing revision A's own XEX through the driver, with the
  // provider attached, must pick up revision A's FunctionHint name.
  {
    const auto root = std::filesystem::temp_directory_path() / "xenon_module_hint_provider_driver_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto input = root / "fixture.xex";
    write_file(input, revision_a_bytes);

    DriverOptions options{};
    options.input = input;
    options.output = root / "generated";
    options.hint_provider_v2 = &provider;

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.hint_set_v2.has_value());
    assert(report.hint_set_v2->functions.size() == 1 &&
           report.hint_set_v2->functions[0].name == "revision_a_entry");

    bool found_named_function = false;
    for (const auto& function : report.functions)
      if (function.guest_start == kLoadAddress + kTextRva) {
        found_named_function = true;
        assert(function.name == "revision_a_entry" &&
               "the driver must actually apply the provider's FunctionHint name, not merely "
               "store the resolved hint set unused");
      }
    assert(found_named_function);
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] The Recomp Driver actually consumes hints resolved through hint_provider_v2\n";

  // Test 6: analyzing revision B's XEX through the SAME provider must select
  // revision B's data, not revision A's (proves selection is per-analysis,
  // not a first-match/cached shortcut).
  {
    const auto root = std::filesystem::temp_directory_path() / "xenon_module_hint_provider_driver_test_b";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto input = root / "fixture.xex";
    write_file(input, revision_b_bytes);

    DriverOptions options{};
    options.input = input;
    options.output = root / "generated";
    options.hint_provider_v2 = &provider;

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.hint_set_v2->functions[0].name == "revision_b_entry");
    std::filesystem::remove_all(root);
  }
  std::cout << "  [PASS] The same provider correctly selects revision B's data for revision B's XEX\n";

  std::filesystem::remove_all(module_root);
  std::cout << "All ModuleHintProvider tests passed!\n";
  return 0;
}
