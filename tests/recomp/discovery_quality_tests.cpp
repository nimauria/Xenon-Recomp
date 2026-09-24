// Recomp Analysis V3 (discovery-quality pass) tests: provenance/evidence
// combination, confidence, generic XEX metadata seeding (TLS callbacks),
// bounded CTR dataflow resolution, invalid-vs-unsupported PPC
// classification, the conservative prologue heuristic, validated tail
// calls, and switch/jump-table bounds-check gating. Same small hand-built
// synthetic XEX2 fixture technique as tests/recomp/analysis_v2_consumption_tests.cpp.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
constexpr std::uint32_t kBlr = 0x4E800020u;
constexpr std::uint32_t kBctrl = 0x4E800421u;   // bctrl: opcode19, XO=528, LK=1
constexpr std::uint32_t kBctr = 0x4E800420u;    // bctr: opcode19, XO=528

std::uint32_t bl_word(std::uint32_t from, std::uint32_t to) {
  return 0x48000000u | ((to - from) & 0x03FFFFFCu) | 1u;
}
std::uint32_t b_word(std::uint32_t from, std::uint32_t to) {
  return 0x48000000u | ((to - from) & 0x03FFFFFCu);
}
// lis rd, val  ==  addis rd, 0, val
std::uint32_t lis_word(std::uint32_t rd, std::uint16_t val) {
  return (15u << 26) | (rd << 21) | (0u << 16) | val;
}
// addi rd, ra, imm
std::uint32_t addi_word(std::uint32_t rd, std::uint32_t ra, std::uint16_t imm) {
  return (14u << 26) | (rd << 21) | (ra << 16) | imm;
}
// ori ra, rs, uimm  (destination is the ra field, source is rs/rt field)
std::uint32_t ori_word(std::uint32_t ra, std::uint32_t rs, std::uint16_t uimm) {
  return (24u << 26) | (rs << 21) | (ra << 16) | uimm;
}
// mtctr rs  ==  mtspr 9, rs  (XFX form: spr encoded split low5/high5 swapped)
std::uint32_t mtctr_word(std::uint32_t rs) {
  constexpr std::uint32_t kCtrSpr = 9u;
  const std::uint32_t enc = ((kCtrSpr & 0x1Fu) << 5) | ((kCtrSpr >> 5) & 0x1Fu);
  return (31u << 26) | (rs << 21) | (enc << 11) | (467u << 1);
}
// mflr rd == mfspr 8, rd
std::uint32_t mflr_word(std::uint32_t rd) {
  constexpr std::uint32_t kLrSpr = 8u;
  const std::uint32_t enc = ((kLrSpr & 0x1Fu) << 5) | ((kLrSpr >> 5) & 0x1Fu);
  return (31u << 26) | (rd << 21) | (enc << 11) | (339u << 1);
}
// lwz rt, disp(ra)
std::uint32_t lwz_word(std::uint32_t rt, std::uint32_t ra, std::int16_t disp) {
  return (32u << 26) | (rt << 21) | (ra << 16) | static_cast<std::uint16_t>(disp);
}

// stwu rs, disp(ra)
std::uint32_t stwu_word(std::uint32_t rs, std::uint32_t ra, std::int16_t disp) {
  return (37u << 26) | (rs << 21) | (ra << 16) | static_cast<std::uint16_t>(disp);
}
// cmplwi ra, uimm  (crfD=0, L=0)
std::uint32_t cmplwi_word(std::uint32_t ra, std::uint16_t uimm) {
  return (10u << 26) | (0u << 23) | (ra << 16) | uimm;
}
// bc (conditional branch, B-form), BO/BI arbitrary non-terminal values, small displacement
std::uint32_t bc_word(std::uint32_t from, std::uint32_t to, std::uint32_t bo, std::uint32_t bi) {
  return (16u << 26) | (bo << 21) | (bi << 16) | ((to - from) & 0xFFFCu);
}

std::vector<std::byte> make_xex(const std::vector<std::uint32_t>& text_words) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  const std::size_t file_size =
      header + static_cast<std::size_t>(kTextRva) + text_words.size() * 4u + 0x100u;

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
  le32(bytes, section + 0x14, kTextRva);
  le32(bytes, section + 0x24, 0x60000020);

  const std::size_t text_file_base = header + static_cast<std::size_t>(kTextRva);
  for (std::size_t i = 0; i < text_words.size(); ++i) be32(bytes, text_file_base + i * 4u, text_words[i]);
  return bytes;
}

bool run(const std::vector<std::byte>& xex_bytes, const std::optional<AnalysisHintSetV2>& hints,
        AnalysisReport& report, const std::filesystem::path& root,
        const std::optional<xbox::XexTls>& tls = std::nullopt,
        std::optional<std::size_t> jobs = std::nullopt) {
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  DriverOptions options{};
  options.input = root / "fixture.xex";
  options.output = root / "generated";
  options.analysis_jobs = jobs;
  if (hints) options.hint_set_v2 = hints;
  std::ofstream(options.input, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));

  if (tls) {
    // Inject a TLS callback directly onto an already-parsed image (Part 2
    // test): exercising the recomp driver's OWN consumption of
    // XexImage::tls, independent of xex_loader.cpp's PE-TLS-directory byte
    // parsing correctness, which is a different subsystem/test surface.
    xbox::XexImage image{};
    std::string parse_error;
    if (!xbox::parse_xex_image(xex_bytes, image, &parse_error)) return false;
    image.tls = *tls;
    options.pre_parsed_image = image;
  }

  std::string error;
  const bool ok = load_and_analyze(options, report, error);
  if (!ok) std::cerr << "load_and_analyze failed: " << error << "\n";
  std::filesystem::remove_all(root);
  return ok;
}

const DiscoveredFunction* find_function(const AnalysisReport& report, std::uint32_t address) {
  const auto it = std::find_if(report.functions.begin(), report.functions.end(),
                               [address](const auto& f) { return f.guest_start == address; });
  return it == report.functions.end() ? nullptr : &*it;
}

const GuestEntryPoint* find_entry(const AnalysisReport& report, std::uint32_t address) {
  const auto it = std::find_if(report.entries.begin(), report.entries.end(),
                               [address](const auto& entry) { return entry.address == address; });
  return it == report.entries.end() ? nullptr : &*it;
}

bool has_source(const DiscoveredFunction& function, DiscoverySource source) {
  return std::find(function.sources.begin(), function.sources.end(), source) != function.sources.end();
}

bool owns_address(const DiscoveredFunction& function, std::uint32_t address) {
  for (std::size_t i = 0; i + 1u < function.ranges.size(); i += 2u)
    if (address >= function.ranges[i] && address < function.ranges[i + 1u]) return true;
  return false;
}

}  // namespace

int main() {
  std::cout << "Testing Recomp Analysis V3 discovery quality...\n";
  const auto root = std::filesystem::temp_directory_path() / "xenon_discovery_quality_test";
  const std::uint32_t base = kLoadAddress + kTextRva;

  // Test 1: confidence_for_sources()/discovery_source_base_confidence() -
  // direct unit coverage of the evidence model itself (Part 11).
  {
    assert(discovery_source_base_confidence(DiscoverySource::EntryPoint) == 100);
    assert(discovery_source_base_confidence(DiscoverySource::PrologueHeuristic) <
           discovery_source_base_confidence(DiscoverySource::DirectCall));
    const auto single = confidence_for_sources({DiscoverySource::DirectCall});
    const auto corroborated =
        confidence_for_sources({DiscoverySource::DirectCall, DiscoverySource::ValidatedTailCall});
    assert(corroborated > single &&
           "two independent sources must be at least as strong as, and here strictly stronger than, one");
    assert(confidence_for_sources({}) > 0 && "an empty evidence list must never produce zero confidence");
  }
  std::cout << "  [PASS] confidence_for_sources() rewards multiple independent evidence sources\n";

  // Test 2: a function reached by both a direct call (from one caller) and
  // a validated tail call (a non-linked branch from a DIFFERENT function)
  // ends up with BOTH sources attached (Part 1's "a candidate may have
  // multiple evidence sources").
  {
    // word0: caller A -> word3 (bl, direct call)
    // word1: caller A return
    // word2: caller B -> word3 (b, tail call - non-linked branch to another function's start)
    // word3: shared callee (blr)
    // Caller B (word2) is not otherwise reachable (nothing calls/branches to
    // it), so it needs its own seed - a FunctionHint - purely to make sure
    // ITS scan actually runs and populates its own branch_references; the
    // point under test is what THAT scan's target then gets tagged with.
    const std::vector<std::uint32_t> words = {
        bl_word(base + 0u, base + 3u * 4u),
        kBlr,
        b_word(base + 2u * 4u, base + 3u * 4u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint caller_b{};
    caller_b.address = base + 2u * 4u;
    hints.functions.push_back(caller_b);
    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "multi_evidence"));
    const auto* callee = find_function(report, base + 3u * 4u);
    assert(callee != nullptr && callee->compiled);
    assert(has_source(*callee, DiscoverySource::DirectCall));
    assert(has_source(*callee, DiscoverySource::ValidatedTailCall));
    assert(callee->sources.size() >= 2);
  }
  std::cout << "  [PASS] A function reached by both a direct call and a validated tail call carries both\n";

  // Test 3: a XEX TLS directory callback address is seeded and discovered
  // purely from generic XEX metadata (Part 2) - no hint required.
  {
    const std::vector<std::uint32_t> words = {kBlr, kBlr};
    const auto xex_bytes = make_xex(words);
    xbox::XexTls tls{};
    tls.callback_address = base + 4u;  // word 1
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "tls_callback", tls));
    const auto* callback_function = find_function(report, base + 4u);
    assert(callback_function != nullptr && callback_function->compiled);
    assert(has_source(*callback_function, DiscoverySource::TlsCallback));
    assert(report.diagnostics.candidates_from_tls_callbacks == 1u);
  }
  std::cout << "  [PASS] A XEX TLS callback address is discovered generically, no hint required\n";

  // Test 4: bounded CTR dataflow (Part 7) - the exact `lis/ori/mtctr/bctrl`
  // pattern statically resolves its call target with no module hint.
  {
    const auto target = base + 5u * 4u;
    const std::vector<std::uint32_t> words = {
        lis_word(11u, static_cast<std::uint16_t>(target >> 16)),
        ori_word(11u, 11u, static_cast<std::uint16_t>(target & 0xFFFFu)),
        mtctr_word(11u),
        kBctrl,
        kBlr,   // caller's own return (word 4)
        kBlr,   // resolved target (word 5)
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "ctr_dataflow"));
    const auto* callee = find_function(report, target);
    assert(callee != nullptr && callee->compiled &&
           "a target resolved purely by generic CTR dataflow must be discovered and compiled");
    assert(has_source(*callee, DiscoverySource::ResolvedIndirectCall));
    assert(report.diagnostics.resolved_indirect_via_dataflow == 1u);
    assert(std::none_of(report.unresolved.begin(), report.unresolved.end(), [&](const auto& item) {
      return item.kind == "indirect-call";
    }));
  }
  std::cout << "  [PASS] lis/ori/mtctr/bctrl resolves its call target via bounded generic dataflow\n";

  // Test 5: an indirect call through a CTR value that is NOT a compile-time
  // constant (loaded from memory, not materialized locally) remains
  // correctly unresolved - the dataflow tracker must never fabricate a
  // target when runtime state genuinely matters.
  {
    // lwz r11, 0(r3); mtctr r11; bctrl - r11's value is a MEMORY LOAD, which
    // the whitelist does not track, so it must invalidate gpr_constant[11]
    // and leave ctr_constant unset.
    constexpr std::uint32_t kLwzR11R3 = (32u << 26) | (11u << 21) | (3u << 16) | 0u;
    const std::vector<std::uint32_t> words = {kLwzR11R3, mtctr_word(11u), kBctrl, kBlr};
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "ctr_unresolved"));
    assert(std::any_of(report.unresolved.begin(), report.unresolved.end(), [](const auto& item) {
      return item.kind == "indirect-call";
    }) && "a genuinely runtime-dependent CTR call must remain unresolved, never fabricated");
    assert(report.diagnostics.resolved_indirect_via_dataflow == 0u);
  }
  std::cout << "  [PASS] A runtime-dependent CTR call (loaded from memory) remains correctly unresolved\n";

  // Test 6: invalid-ppc vs unsupported-ppc classification (Part 5) - a word
  // whose primary opcode has zero cataloged entries at all classifies as
  // invalid-ppc; a word sharing a cataloged primary opcode but no matching
  // specific encoding classifies as unsupported-ppc, never collapsed
  // together.
  {
    constexpr std::uint32_t kGenuinelyInvalid = 0x00000000u;  // primary opcode 0: no cataloged entries
    // Primary opcode 31 (0x7C......) is heavily used (many X/XO/XFX-form
    // integer/control instructions); an all-ones word under that primary
    // is exceedingly unlikely to match any cataloged pattern, while the
    // primary itself is definitely cataloged.
    constexpr std::uint32_t kUnsupportedUnderKnownPrimary = 0x7FFFFFFFu;
    const std::vector<std::uint32_t> words = {kGenuinelyInvalid};
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "invalid_ppc"));
    assert(std::any_of(report.unresolved.begin(), report.unresolved.end(),
                       [](const auto& item) { return item.kind == "invalid-ppc"; }));
    assert(report.diagnostics.invalid_ppc_sites >= 1u);

    const std::vector<std::uint32_t> words2 = {kUnsupportedUnderKnownPrimary};
    const auto xex_bytes2 = make_xex(words2);
    AnalysisReport report2;
    assert(run(xex_bytes2, std::nullopt, report2, root / "unsupported_ppc"));
    const bool found_unsupported =
        std::any_of(report2.unresolved.begin(), report2.unresolved.end(),
                   [](const auto& item) { return item.kind == "unsupported-ppc" || item.kind == "unsupported-vmx"; });
    // The exact classification depends on whether this word happens to
    // match a cataloged pattern at all (extremely unlikely for an
    // all-ones payload under a real, densely-populated primary opcode);
    // if the catalog genuinely has zero entries for this exact word's
    // primary, this assertion documents that expectation explicitly
    // rather than silently accepting either outcome.
    assert(found_unsupported && "an all-ones word under a cataloged primary opcode must classify as "
                                "unsupported (a real instruction family Xenon doesn't fully implement), "
                                "never collapsed into invalid-ppc");
  }
  std::cout << "  [PASS] invalid-ppc and unsupported-ppc are never collapsed into one diagnostic\n";

  // Test 7: the conservative prologue heuristic tags SUPPORTING evidence on
  // an already-otherwise-discovered candidate; it never independently
  // creates a new one (Part 4's false-positive-prevention requirement).
  {
    // word0 is always seeded (EntryPoint), so it becomes its own function
    // regardless; the point under test is word1 - reachable by NOTHING
    // (not the entry, not hinted, not called/branched to) - which must NOT
    // become a function just because word2's hinted prologue scan happens
    // to start nearby.
    FunctionHint fn{};
    fn.address = base + 2u * 4u;
    const std::vector<std::uint32_t> words = {
        kBlr,                                    // word 0: entry point (its own function)
        kBlr,                                    // word 1: unreachable filler
        stwu_word(1u, 1u, -32),                 // word 2: hinted function's real prologue opening
        mflr_word(0u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    hints.functions.push_back(fn);
    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "prologue"));
    const auto* function = find_function(report, fn.address);
    assert(function != nullptr && function->compiled);
    assert(has_source(*function, DiscoverySource::PrologueHeuristic) &&
           "a recognized stwu-based prologue at a candidate's first instruction must be tagged");
    assert(has_source(*function, DiscoverySource::ModuleHint));
    // No function may ever be created FROM the heuristic alone: word1,
    // reachable by nothing, must not have spawned a candidate - only the
    // entry point (word0) and the hinted function (word2) may exist.
    assert(report.functions.size() == 2u);
    assert(find_function(report, base + 1u * 4u) == nullptr);
  }
  std::cout << "  [PASS] Prologue heuristic adds supporting evidence only, never creates a candidate alone\n";

  // Test 8: switch/jump-table recovery (Part 8) only fires when a bounds
  // check appeared recently before the indirect branch - the same
  // unresolved bctr with no nearby compare recovers nothing.
  {
    // Function A: cmplwi r3,3; bc(skip); mtctr-less bctr with no resolvable
    // CTR value, but WITH a compare shortly before -> recovery scan runs.
    // Table words directly follow the bctr, one of which points at word
    // "target" (a real blr elsewhere in the image).
    const auto site = base + 2u * 4u;
    const auto table_word_addr = site + 4u;
    const auto target = base + 6u * 4u;
    std::vector<std::uint32_t> words = {
        cmplwi_word(3u, 3u),                    // word0: bounds check
        bc_word(base + 4u, base + 4u + 4u, 4u, 2u),  // word1: (harmless conditional, keeps scan going)
        kBctr,                                  // word2: unresolved indirect branch (site)
        target,                                 // word3: recovered table entry (an address, not an instruction)
        0u,                                     // word4: padding
        0u,                                     // word5: padding
        kBlr,                                   // word6: target
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    // allow_partial so the (deliberately) data-shaped table word doesn't
    // abort compilation of whatever function happens to scan into it.
    std::filesystem::remove_all(root / "switch_with_compare");
    std::filesystem::create_directories(root / "switch_with_compare");
    DriverOptions options{};
    options.input = root / "switch_with_compare" / "fixture.xex";
    options.output = root / "switch_with_compare" / "generated";
    options.allow_partial = true;
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    const bool recovered =
        std::any_of(report.warnings.begin(), report.warnings.end(),
                   [](const auto& w) { return w.find("recovered possible jump-table target") != std::string::npos; });
    assert(recovered && "a bounds check shortly before an unresolved indirect branch must enable recovery");
    (void)table_word_addr;
    std::filesystem::remove_all(root / "switch_with_compare");
  }
  std::cout << "  [PASS] Jump-table recovery fires when a bounds check appears shortly before the branch\n";

  {
    // Same shape, but with NO compare anywhere before the bctr - recovery
    // must NOT fire (Part 8: never treat every unresolved indirect branch
    // as a possible table).
    std::vector<std::uint32_t> words = {
        kBlr,               // word0: unrelated, no compare
        kBlr,               // word1: unrelated, no compare
        kBctr,              // word2: unresolved indirect branch, entry point unreachable via blr above so hint it
        base + 6u * 4u,     // word3: pointer-shaped data
        0u, 0u,
        kBlr,                // word6
    };
    FunctionHint fn{};
    fn.address = base + 2u * 4u;
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    hints.functions.push_back(fn);
    AnalysisReport report;
    std::filesystem::remove_all(root / "switch_without_compare");
    std::filesystem::create_directories(root / "switch_without_compare");
    DriverOptions options{};
    options.input = root / "switch_without_compare" / "fixture.xex";
    options.output = root / "switch_without_compare" / "generated";
    options.allow_partial = true;
    options.hint_set_v2 = hints;
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    const bool recovered =
        std::any_of(report.warnings.begin(), report.warnings.end(),
                   [](const auto& w) { return w.find("recovered possible jump-table target") != std::string::npos; });
    assert(!recovered &&
           "with no bounds check nearby, pointer-shaped data must NOT be promoted to a discovered target");
    std::filesystem::remove_all(root / "switch_without_compare");
  }
  std::cout << "  [PASS] Jump-table recovery does NOT fire without a nearby bounds check\n";

  // Test 9: unaligned and non-executable candidates are still rejected with
  // explicit diagnostics, never silently (Part 6 - re-confirms V2's fix,
  // now exercised through the richer V3 candidate paths too).
  {
    const std::vector<std::uint32_t> words = {kBlr};
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint misaligned{};
    misaligned.address = base + 1u;  // deliberately not 4-byte aligned
    hints.functions.push_back(misaligned);
    FunctionHint unmapped{};
    unmapped.address = 0x90000000u;  // deliberately outside any section
    hints.functions.push_back(unmapped);
    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "bad_seeds"));
    assert(std::any_of(report.unresolved.begin(), report.unresolved.end(),
                       [](const auto& item) { return item.kind == "candidate-unaligned"; }));
    assert(std::any_of(report.unresolved.begin(), report.unresolved.end(),
                       [](const auto& item) { return item.kind == "seed"; }));
    assert(report.diagnostics.function_candidates_rejected_unaligned >= 1u);
    assert(report.diagnostics.function_candidates_rejected_nonexec >= 1u);
  }
  std::cout << "  [PASS] Misaligned and non-executable candidates are rejected with explicit diagnostics\n";

  // Test 10 (tail-call over-discovery fix): a genuinely conditional branch's
  // target is an ordinary basic-block edge (the "else" path), never an
  // independent function, even though nothing else claims that address
  // before the parent's own scan reaches it.
  {
    // word0: cmplwi r3,3          (entry)
    // word1: bc (non-terminal) -> word3 (skip the "else" body)
    // word2: addi r3,r3,1          (the "else" body, dead-ends into word3)
    // word3: blr
    const std::vector<std::uint32_t> words = {
        cmplwi_word(3u, 3u),
        bc_word(base + 1u * 4u, base + 3u * 4u, 4u, 2u),
        addi_word(3u, 3u, 1u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "intra_function_branch"));
    assert(report.functions.size() == 1u &&
           "a conditional branch's target must never spawn a second function");
    assert(find_function(report, base + 3u * 4u) == nullptr);
    const auto* entry = find_function(report, base);
    assert(entry != nullptr && entry->compiled && entry->guest_end == base + 4u * 4u);
  }
  std::cout << "  [PASS] A conditional branch's target stays an internal basic block, never a function\n";

  // Test 11 (tail-call over-discovery fix): a loop's terminal back-edge to a
  // MID-function address (not the function's own start - the trivial case
  // the pre-existing `claimed` dedup would mask) must not spawn a spurious
  // duplicate function.
  {
    // word0: mflr r0                (entry)
    // word1: addi r3,r3,-1          (loop_top)
    // word2: cmplwi r3,0
    // word3: bc (non-terminal) -> word5 (loop exit)
    // word4: b (terminal, back-edge) -> word1 (loop_top, NOT word0)
    // word5: blr                    (exit path)
    const std::vector<std::uint32_t> words = {
        mflr_word(0u),
        addi_word(3u, 3u, static_cast<std::uint16_t>(-1)),
        cmplwi_word(3u, 0u),
        bc_word(base + 3u * 4u, base + 5u * 4u, 4u, 2u),
        b_word(base + 4u * 4u, base + 1u * 4u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "loop_back_edge"));
    assert(report.functions.size() == 1u &&
           "a loop's terminal back-edge to a mid-function address must not spawn a duplicate function");
    assert(find_function(report, base + 1u * 4u) == nullptr);
    const auto* entry = find_function(report, base);
    assert(entry != nullptr && entry->compiled && entry->guest_end == base + 6u * 4u &&
           owns_address(*entry, base + 5u * 4u) &&
           "the loop exit block at the tentative end must be folded into the final extent");
  }
  std::cout << "  [PASS] A loop's terminal back-edge to a mid-function address stays internal\n";

  // Test 12 (tail-call over-discovery fix): a shared epilogue reached by
  // TERMINAL branches from two independently strong (hinted) functions is
  // preserved as exactly one function, corroborated by ValidatedTailCall
  // from both callers - legitimate split/shared-tail handling must survive
  // the conservative rewrite, not just the over-discovery case.
  {
    // word0: caller A's own body, terminal branch -> word4 (shared epilogue)
    // word1: caller B's own body, terminal branch -> word4 (shared epilogue)
    // word2/3: padding (never reached, keeps addresses simple)
    // word4: shared epilogue (blr)
    const std::vector<std::uint32_t> words = {
        b_word(base + 0u * 4u, base + 4u * 4u),
        b_word(base + 1u * 4u, base + 4u * 4u),
        kBlr,
        kBlr,
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    // caller A (word0) needs no hint - it is already the natural entry
    // point; only caller B (word1), otherwise unreachable, needs one.
    FunctionHint caller_b{};
    caller_b.address = base + 1u * 4u;
    hints.functions.push_back(caller_b);
    FunctionHint shared_epilogue{};
    shared_epilogue.address = base + 4u * 4u;
    hints.functions.push_back(shared_epilogue);
    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "shared_epilogue"));
    const auto matching = [&](std::uint32_t address) {
      return static_cast<std::size_t>(std::count_if(
          report.functions.begin(), report.functions.end(),
          [address](const auto& function) { return function.guest_start == address; }));
    };
    assert(matching(base + 4u * 4u) == 1u &&
           "a shared epilogue reached from two callers must be exactly one function, not duplicated");
    const auto* epilogue = find_function(report, base + 4u * 4u);
    assert(epilogue != nullptr && epilogue->compiled);
    assert(has_source(*epilogue, DiscoverySource::DirectBranch));
    assert(has_source(*epilogue, DiscoverySource::ValidatedTailCall));
  }
  std::cout << "  [PASS] A shared epilogue reached by two strong callers stays a single, corroborated function\n";

  // Test 13 (CFG closure): a conditional direct branch target that lands
  // exactly at the primary range's tentative exclusive end is still
  // intra-procedural reachable code. It must extend the owning function's
  // CFG/extent rather than be promoted to a synthetic
  // function or left as branch-into-unknown-code. This is the generic shape
  // of the AC6 boundary bug this regression protects against.
  {
    // word0: compare
    // word1: conditional branch -> word3
    // word2: blr                     (fallthrough arm terminates; primary
    //                                 scan therefore ends at word3)
    // word3: addi                    (conditional target == primary end)
    // word4: blr
    const std::vector<std::uint32_t> words = {
        cmplwi_word(3u, 0u),
        bc_word(base + 1u * 4u, base + 3u * 4u, 4u, 2u),
        kBlr,
        addi_word(3u, 3u, 5u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    const auto local_target = base + 3u * 4u;

    AnalysisReport report_serial;
    assert(run(xex_bytes, std::nullopt, report_serial, root / "boundary_cfg_serial", std::nullopt, 1u));
    AnalysisReport report_parallel;
    assert(run(xex_bytes, std::nullopt, report_parallel, root / "boundary_cfg_parallel", std::nullopt, 8u));

    for (const auto* report : {&report_serial, &report_parallel}) {
      assert(report->functions.size() == 1u &&
             "a conditional target at the tentative end must not spawn a synthetic function");
      const auto* entry = find_function(*report, base);
      assert(entry != nullptr && entry->compiled);
      assert(has_source(*entry, DiscoverySource::EntryPoint));
      assert(entry->guest_end > local_target &&
             "a reachable target exactly at the tentative end must extend the final function extent");
      assert(owns_address(*entry, local_target) &&
             "the conditional target must be materialized as an owned local block");
      assert(find_function(*report, local_target) == nullptr);
      assert(std::none_of(report->unresolved.begin(), report->unresolved.end(), [&](const auto& item) {
        return item.kind == "branch-into-unknown-code" && item.target == local_target;
      }) && "a materialized local target must not remain branch-into-unknown-code");
      const auto block = std::find_if(entry->ir.blocks.begin(), entry->ir.blocks.end(), [&](const auto& item) {
        return item.guest_address == local_target;
      });
      assert(block != entry->ir.blocks.end() &&
             "the recovered local continuation must become a real IR basic block");
      assert(std::any_of(entry->ir.blocks.begin(), entry->ir.blocks.end(), [&](const auto& source_block) {
        return std::any_of(source_block.successors.begin(), source_block.successors.end(), [&](const auto& edge) {
          return edge.target == local_target && edge.local;
        });
      }) && "the direct edge to the recovered continuation must be local, never runtime-dispatched");
    }
    assert(report_serial.functions.size() == report_parallel.functions.size());
    assert(report_serial.diagnostics.analysis_waves == report_parallel.diagnostics.analysis_waves &&
           "discovery must be deterministic regardless of worker count");
  }
  std::cout << "  [PASS] Conditional targets at tentative range ends materialize as local CFG blocks\n";

  // Test 14 (diagnostic provenance): two different branch instructions to
  // the same unresolved target are two distinct findings. `address` must be
  // the ACTUAL PPC branch instruction site, never the containing function's
  // start address. Exact duplicate emissions for one physical site are still
  // deduplicated by the final sort+unique pass.
  {
    const auto external_target = base + 10u * 4u;
    std::vector<std::uint32_t> words(11u, 0u);
    words[0] = bc_word(base + 0u * 4u, external_target, 4u, 2u);
    words[1] = bc_word(base + 1u * 4u, external_target, 4u, 2u);
    words[2] = kBlr;
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "dedup_unresolved"));
    const auto count = static_cast<std::size_t>(std::count_if(
        report.unresolved.begin(), report.unresolved.end(), [&](const auto& item) {
          return item.kind == "branch-into-unknown-code" && item.target == external_target;
        }));
    assert(count == 2u &&
           "two physical branch sites to one orphan target must retain both exact source addresses");
    assert(std::any_of(report.unresolved.begin(), report.unresolved.end(), [&](const auto& item) {
      return item.kind == "branch-into-unknown-code" && item.target == external_target &&
             item.address == base;
    }));
    assert(std::any_of(report.unresolved.begin(), report.unresolved.end(), [&](const auto& item) {
      return item.kind == "branch-into-unknown-code" && item.target == external_target &&
             item.address == base + 4u;
    }));
  }
  std::cout << "  [PASS] Unresolved branch diagnostics preserve exact PPC source sites\n";

  // Test 15 (resolved-indirect reconciliation): a target resolved purely via
  // the bounded CTR dataflow tracker (Part 7) increments
  // resolved_indirect_via_dataflow, and the resulting function's
  // ResolvedIndirect provenance is reflected 1:1 in
  // functions_with_resolved_indirect_provenance - the two counters, computed
  // on entirely different code paths, must agree for this simple case.
  {
    const auto target = base + 5u * 4u;
    const std::vector<std::uint32_t> words = {
        lis_word(11u, static_cast<std::uint16_t>(target >> 16)),
        ori_word(11u, 11u, static_cast<std::uint16_t>(target & 0xFFFFu)),
        mtctr_word(11u),
        kBctrl,
        kBlr,
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "resolved_indirect_reconcile"));
    const auto* callee = find_function(report, target);
    assert(callee != nullptr && callee->compiled && has_source(*callee, DiscoverySource::ResolvedIndirectCall));
    assert(report.diagnostics.resolved_indirect_via_dataflow == 1u);
    assert(report.diagnostics.resolved_indirect_via_jump_table == 0u);
    assert(report.diagnostics.functions_with_resolved_indirect_provenance == 1u &&
           "the distinct-function count must match the single dataflow resolution event here");
  }
  std::cout << "  [PASS] Dataflow-resolved-indirect provenance and diagnostics agree\n";

  // Test 16 (resolved-indirect reconciliation, jump-table path): a target
  // recovered via the switch/jump-table heuristic (Part 8) increments the
  // DISTINCT resolved_indirect_via_jump_table counter (never conflated with
  // resolved_indirect_via_dataflow), and is reflected in
  // functions_with_resolved_indirect_provenance exactly like the dataflow
  // case above.
  {
    const auto target = base + 6u * 4u;
    std::vector<std::uint32_t> words = {
        cmplwi_word(3u, 3u),
        bc_word(base + 4u, base + 4u + 4u, 4u, 2u),
        kBctr,
        target,
        0u,
        0u,
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    std::filesystem::remove_all(root / "resolved_indirect_jump_table");
    std::filesystem::create_directories(root / "resolved_indirect_jump_table");
    DriverOptions options{};
    options.input = root / "resolved_indirect_jump_table" / "fixture.xex";
    options.output = root / "resolved_indirect_jump_table" / "generated";
    options.allow_partial = true;
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    const auto* callee = find_function(report, target);
    assert(callee == nullptr &&
           "a switch case reached by non-linked indirect control flow must not be forced into a semantic function");
    const auto* case_entry = find_entry(report, target);
    assert(case_entry != nullptr && case_entry->kind == GuestEntryKind::AlternateBlock &&
           std::find(case_entry->sources.begin(), case_entry->sources.end(),
                     DiscoverySource::ResolvedIndirectBranch) != case_entry->sources.end());
    assert(report.diagnostics.resolved_indirect_via_jump_table >= 1u);
    assert(report.diagnostics.resolved_indirect_via_dataflow == 0u);
    assert(report.diagnostics.functions_with_resolved_indirect_provenance == 0u &&
           "resolved branch targets absorbed into an owner are entries, not functions");
    std::filesystem::remove_all(root / "resolved_indirect_jump_table");
  }
  std::cout << "  [PASS] Jump-table-resolved-indirect provenance and diagnostics agree, distinctly from dataflow\n";

  // Test 17 (pre-codegen direct-control-flow invariant): generation must
  // fail before native C++ is written if an analyzed function contains a
  // statically-known external branch/fallthrough whose target is neither a
  // local CFG block nor a dispatchable compiled entry. This is the final
  // safety net for any ownership defect that survives analysis.
  {
    const std::vector<std::uint32_t> words = {kBlr, kBlr};
    const auto xex_bytes = make_xex(words);
    const auto test_root = root / "codegen_control_flow_invariant";
    std::filesystem::remove_all(test_root);
    std::filesystem::create_directories(test_root);

    DriverOptions options{};
    options.input = test_root / "fixture.xex";
    options.output = test_root / "generated";
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()),
               static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error));
    assert(report.functions.size() == 1u);
    assert(report.functions.front().compiled);
    assert(!report.functions.front().ir.blocks.empty());

    report.functions.front().ir.blocks.front().successors.push_back(
        {base + 4u, xenon::cpu::ir::EdgeKind::Branch, false});
    error.clear();
    assert(!generate_project(options, report, error));
    assert(error.find("without a materialized local CFG block or dispatchable compiled entry") !=
           std::string::npos);
    std::filesystem::remove_all(test_root);
  }
  std::cout << "  [PASS] Codegen rejects unmaterialized external direct-control-flow targets\n";

  // Test 18 (Region + Entry): a runtime-observed address that is already an
  // internal basic block of a stronger semantic function must NOT survive as
  // a duplicate overlapping function. It becomes a first-class alternate
  // guest entry and codegen emits a tiny wrapper that dispatches directly to
  // the owner's existing block switch.
  {
    const std::vector<std::uint32_t> words = {
        cmplwi_word(3u, 0u),
        bc_word(base + 1u * 4u, base + 3u * 4u, 4u, 2u),
        kBlr,
        addi_word(3u, 3u, 5u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    const auto alternate = base + 3u * 4u;
    const auto test_root = root / "region_entry_runtime_observation";
    std::filesystem::remove_all(test_root);
    std::filesystem::create_directories(test_root);

    DriverOptions options{};
    options.input = test_root / "fixture.xex";
    options.output = test_root / "generated";
    options.analysis_jobs = 1u;
    AdaptiveObservation observation{};
    observation.address = alternate;
    observation.site = base + 4u;
    observation.kind = AdaptiveObservationKind::ExecutedEntry;
    observation.hits = 3u;
    observation.owner_hint = base;
    options.adaptive_observations.push_back(observation);
    std::ofstream(options.input, std::ios::binary)
        .write(reinterpret_cast<const char*>(xex_bytes.data()),
               static_cast<std::streamsize>(xex_bytes.size()));

    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.functions.size() == 1u);
    assert(find_function(report, alternate) == nullptr &&
           "a weak observed interior entry must be absorbed, not emitted as an overlapping function");
    const auto* entry = find_entry(report, alternate);
    assert(entry != nullptr);
    assert(entry->kind == GuestEntryKind::AlternateBlock);
    assert(entry->owner_function == base && entry->block == alternate);
    assert(std::find(entry->sources.begin(), entry->sources.end(), DiscoverySource::RuntimeObservation) !=
           entry->sources.end());
    assert(report.diagnostics.weak_functions_absorbed == 1u);
    assert(report.diagnostics.alternate_entries_materialized == 1u);

    assert(generate_project(options, report, error) && error.empty());
    const auto wrapper_symbol = report.functions.front().name + "_entry_" + [&] {
      std::ostringstream value;
      value << std::hex << std::uppercase << alternate;
      return value.str();
    }() + "_v2";
    std::ifstream registry(options.output / "registry.cpp");
    const std::string registry_text((std::istreambuf_iterator<char>(registry)), {});
    assert(registry_text.find(wrapper_symbol) != std::string::npos &&
           "alternate entry must be published by generated registry.cpp");
    bool wrapper_emitted = false;
    for (const auto& item : std::filesystem::directory_iterator(options.output / "functions")) {
      if (!item.is_regular_file()) continue;
      std::ifstream shard(item.path());
      const std::string text((std::istreambuf_iterator<char>(shard)), {});
      if (text.find(wrapper_symbol) != std::string::npos) {
        wrapper_emitted = true;
        break;
      }
    }
    assert(wrapper_emitted && "the canonical function's shard must define the alternate-entry wrapper");
    std::filesystem::remove_all(test_root);
  }
  std::cout << "  [PASS] Runtime-observed interior blocks collapse into generated alternate entries\n";

  // Test 19 (Dead-Rising-style dropped-edge recovery): two independent
  // semantic owners branching BACKWARD to the same otherwise-unproven
  // executable address are enough to recover that orphan after ordinary
  // local CFG closure. Forward targets are now closed directly as blocks, so
  // using backward cross-function edges here specifically exercises the
  // multi-source orphan recovery pass rather than the normal CFG worklist.
  {
    const auto orphan = base + 2u * 4u;
    const auto caller_a = base + 4u * 4u;
    const auto caller_b = base + 6u * 4u;
    const std::vector<std::uint32_t> words = {
        kBlr,
        0u,
        addi_word(3u, 3u, 1u),
        kBlr,
        b_word(caller_a, orphan),
        0u,
        b_word(caller_b, orphan),
        0u,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint first_caller{};
    first_caller.address = caller_a;
    hints.functions.push_back(first_caller);
    FunctionHint second_caller{};
    second_caller.address = caller_b;
    hints.functions.push_back(second_caller);
    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "multi_source_orphan"));
    const auto* recovered = find_function(report, orphan);
    assert(recovered != nullptr && recovered->compiled);
    assert(has_source(*recovered, DiscoverySource::GapRecovery));
    assert(report.diagnostics.orphan_entries_recovered == 1u);
  }
  std::cout << "  [PASS] Multi-source orphan edges recover dropped executable regions generically\n";

  // Test 20 (static vtable/function-pointer recovery): a structurally diverse
  // run of executable pointers in a non-code section seeds dispatchable targets
  // without a title-specific function list. Repeating one pointer many times is
  // deliberately NOT sufficient evidence of a table.
  {
    const auto target_a = base + 4u * 4u;
    const auto target_b = base + 8u * 4u;
    const auto target_c = base + 12u * 4u;
    const std::vector<std::uint32_t> words = {
        kBlr, 0u, 0u, 0u,
        mflr_word(0u), kBlr, 0u, 0u,
        stwu_word(1u, 1u, -32), kBlr, 0u, 0u,
        mflr_word(0u), kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    xbox::XexSection pointer_section{};
    pointer_section.name = ".rdata";
    pointer_section.virtual_address = kLoadAddress + 0x30000u;
    pointer_section.virtual_size = 24u;
    pointer_section.raw_size = 24u;
    pointer_section.readable = true;
    pointer_section.executable = false;
    pointer_section.bytes.resize(24u);
    const std::array<std::uint32_t, 6> table = {
        target_a, target_b, target_c, target_a, target_b, target_c};
    for (std::size_t i = 0; i < table.size(); ++i)
      be32(pointer_section.bytes, i * 4u, table[i]);
    image.sections.push_back(pointer_section);

    const auto test_root = root / "static_pointer_table";
    DriverOptions options{};
    options.pre_parsed_image = image;
    options.output = test_root / "generated";
    options.analysis_jobs = 1u;
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    for (const auto target : {target_a, target_b, target_c}) {
      const auto* recovered = find_function(report, target);
      assert(recovered != nullptr && recovered->compiled);
      assert(has_source(*recovered, DiscoverySource::PointerTable));
    }
    assert(report.diagnostics.pointer_tables_discovered == 1u);
    assert(report.diagnostics.pointer_table_targets_discovered == 3u);

    // Replace the table with six identical pointer-shaped constants: this must
    // no longer create a static pointer table by structure alone.
    for (std::size_t i = 0; i < 6u; ++i)
      be32(image.sections.back().bytes, i * 4u, target_a);
    AnalysisReport repeated_report;
    options.pre_parsed_image = image;
    assert(load_and_analyze(options, repeated_report, error) && error.empty());
    assert(repeated_report.diagnostics.pointer_tables_discovered == 0u);
    assert(repeated_report.diagnostics.pointer_table_targets_discovered == 0u);
    std::filesystem::remove_all(test_root);
  }
  std::cout << "  [PASS] Static pointer-table scanning requires diverse executable-entry structure\n";

  // Test 21 (adaptive trace ingestion): runtime JSONL is crash-safe and may
  // contain repeated observations. Ingest must merge identical facts and
  // preserve distinct call/branch evidence.
  {
    const auto trace = root / "adaptive-observations.jsonl";
    std::filesystem::create_directories(root);
    {
      std::ofstream out(trace);
      out << "{\"address\":0x821F7DA8,\"site\":0x821F7D90,\"kind\":\"indirect-branch-target\",\"hits\":1}\n";
      out << "{\"address\":0x821F7DA8,\"site\":0x821F7D90,\"kind\":\"indirect-branch-target\",\"hits\":2}\n";
      out << "{\"address\":0x82382A68,\"site\":0x82382000,\"kind\":\"indirect-call-target\",\"hits\":1}\n";
    }
    std::vector<AdaptiveObservation> observations;
    std::string error;
    assert(load_adaptive_observations(trace, observations, error) && error.empty());
    assert(observations.size() == 2u);
    const auto merged = std::find_if(observations.begin(), observations.end(), [](const auto& observation) {
      return observation.address == 0x821F7DA8u;
    });
    assert(merged != observations.end() && merged->hits == 3u);
    const auto fingerprint = adaptive_observation_fingerprint(observations);
    auto repeated = observations;
    repeated.front().hits += 1000u;
    assert(adaptive_observation_fingerprint(repeated) == fingerprint &&
           "repeat hit counts must not invalidate a prepared artifact");
    AdaptiveObservation new_fact{};
    new_fact.address = 0x82382A6Cu;
    new_fact.site = 0x82382000u;
    new_fact.kind = AdaptiveObservationKind::IndirectBranchTarget;
    repeated.push_back(new_fact);
    assert(adaptive_observation_fingerprint(repeated) != fingerprint &&
           "a new adaptive control-flow fact must invalidate preparation cache identity");
    std::filesystem::remove(trace);
  }
  std::cout << "  [PASS] Adaptive runtime observation traces ingest and deduplicate deterministically\n";

  // Test 21b: runtime learning is revision-scoped. A trace from another
  // effective image may reuse the same guest address and must not seed this
  // analysis.
  {
    const std::vector<std::uint32_t> words = {kBlr};
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    DriverOptions options{};
    options.pre_parsed_image = image;
    options.output = root / "adaptive_revision_scope";
    options.analysis_jobs = 1u;
    AdaptiveObservation stale{};
    stale.address = base;
    stale.site = base;
    stale.kind = AdaptiveObservationKind::ExecutedEntry;
    stale.image_hash = "0000000000000000000000000000000000000000";
    options.adaptive_observations.push_back(stale);
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    assert(report.diagnostics.adaptive_observations_consumed == 0u);
    assert(report.diagnostics.adaptive_observations_rejected_revision == 1u);
  }
  std::cout << "  [PASS] Adaptive observations are scoped to the exact effective executable revision\n";

  // Test 22 (forward terminal branch closure): a non-linked forward `b` must
  // not become a tail-call/function boundary merely because it crosses the
  // current tentative extent. This is the generic form of AC6's
  // 0x821F7D50 -> 0x821F7DA8 failure class.
  {
    const auto target = base + 2u * 4u;
    const std::vector<std::uint32_t> words = {
        b_word(base, target),
        0u,
        addi_word(3u, 3u, 1u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "forward_terminal_cfg"));
    assert(report.functions.size() == 1u);
    const auto& owner = report.functions.front();
    assert(owner.guest_start == base && owner.compiled);
    assert(std::any_of(owner.ir.blocks.begin(), owner.ir.blocks.end(), [&](const auto& block) {
      return block.guest_address == target;
    }));
    assert(std::none_of(report.unresolved.begin(), report.unresolved.end(), [&](const auto& item) {
      return item.target == target && item.kind == "branch-into-unknown-code";
    }));
  }
  std::cout << "  [PASS] Forward terminal branches extend CFG ownership before tail-call classification\n";

  // Test 23 (generic switch-tail repair): module metadata proving a switch
  // edge does NOT prove each case body is a function. The case target is
  // absorbed into the canonical owner and remains externally dispatchable as
  // an AlternateBlock entry. This replaces title-specific function-bound
  // widening tables used by older recomp projects.
  {
    const auto target = base + 3u * 4u;
    const std::vector<std::uint32_t> words = {
        kBctr,
        0u,
        0u,
        addi_word(3u, 3u, 7u),
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    SwitchTableHint table{};
    table.site = base;
    table.explicit_targets.push_back(target);
    hints.switches.push_back(table);

    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "switch_tail_region_entry"));
    assert(find_function(report, target) == nullptr &&
           "switch case targets are entry blocks, not automatic semantic functions");
    const auto* entry = find_entry(report, target);
    assert(entry != nullptr && entry->kind == GuestEntryKind::AlternateBlock &&
           entry->owner_function == base);
    assert(std::find(entry->sources.begin(), entry->sources.end(), DiscoverySource::ControlFlowHint) !=
           entry->sources.end());
  }
  std::cout << "  [PASS] Switch-tail case bodies become alternate entries in the canonical compiled region\n";

  // Test 24 (evidence accumulation): a strong seed must retain its identity
  // when weaker adaptive/static evidence names the same address. This prevents
  // seed insertion order from changing function authority.
  {
    const auto target_b = base + 4u * 4u;
    const auto target_c = base + 8u * 4u;
    const std::vector<std::uint32_t> words = {
        mflr_word(0u), kBlr, 0u, 0u,
        mflr_word(0u), kBlr, 0u, 0u,
        stwu_word(1u, 1u, -32), kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    xbox::XexSection pointer_section{};
    pointer_section.name = ".rdata";
    pointer_section.virtual_address = kLoadAddress + 0x31000u;
    pointer_section.virtual_size = 24u;
    pointer_section.raw_size = 24u;
    pointer_section.readable = true;
    pointer_section.executable = false;
    pointer_section.bytes.resize(24u);
    const std::array<std::uint32_t, 6> table = {
        base, target_b, target_c, base, target_b, target_c};
    for (std::size_t i = 0; i < table.size(); ++i)
      be32(pointer_section.bytes, i * 4u, table[i]);
    image.sections.push_back(pointer_section);

    DriverOptions options{};
    options.pre_parsed_image = image;
    options.output = root / "seed_evidence_accumulation";
    options.analysis_jobs = 1u;
    AdaptiveObservation observation{};
    observation.address = base;
    observation.site = base + 4u;
    observation.kind = AdaptiveObservationKind::ExecutedEntry;
    options.adaptive_observations.push_back(observation);
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    const auto* entry_function = find_function(report, base);
    assert(entry_function != nullptr);
    assert(has_source(*entry_function, DiscoverySource::EntryPoint));
    assert(has_source(*entry_function, DiscoverySource::PointerTable));
    assert(has_source(*entry_function, DiscoverySource::RuntimeObservation));
    assert(entry_function->authority == FunctionAuthority::EntryPoint);
    std::filesystem::remove_all(options.output);
  }
  std::cout << "  [PASS] Initial seeds accumulate evidence without weak-source downgrades\n";

  // Test 25 (runtime call evidence): an observed indirect CALL target carries
  // both runtime-observation and callable-boundary provenance. Executed/branch
  // observations remain weaker entry facts.
  {
    const auto target = base + 4u * 4u;
    const std::vector<std::uint32_t> words = {
        kBlr, 0u, 0u, 0u, mflr_word(0u), kBlr,
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    DriverOptions options{};
    options.pre_parsed_image = image;
    options.output = root / "runtime_call_evidence";
    options.analysis_jobs = 1u;
    AdaptiveObservation observation{};
    observation.address = target;
    observation.site = base;
    observation.kind = AdaptiveObservationKind::IndirectCallTarget;
    options.adaptive_observations.push_back(observation);
    AnalysisReport report;
    std::string error;
    assert(load_and_analyze(options, report, error) && error.empty());
    const auto* recovered = find_function(report, target);
    assert(recovered != nullptr && recovered->compiled);
    assert(has_source(*recovered, DiscoverySource::RuntimeObservation));
    assert(has_source(*recovered, DiscoverySource::ResolvedIndirectCall));
    assert(recovered->authority == FunctionAuthority::DirectCall);
    std::filesystem::remove_all(options.output);
  }
  std::cout << "  [PASS] Runtime indirect-call observations become strong callable evidence\n";

  // Test 26 (stable adaptive configuration identity): ordering and duplicate
  // hit counts are runtime-history details, not different recompilation facts.
  {
    const auto xex_bytes = make_xex({kBlr});
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
    AdaptiveObservation a{};
    a.address = base;
    a.site = base + 4u;
    a.kind = AdaptiveObservationKind::ExecutedEntry;
    a.hits = 1u;
    AdaptiveObservation b{};
    b.address = base;
    b.site = base + 8u;
    b.kind = AdaptiveObservationKind::IndirectBranchTarget;
    b.hits = 2u;

    DriverOptions first{};
    first.pre_parsed_image = image;
    first.output = root / "adaptive_hash_first";
    first.analysis_jobs = 1u;
    first.adaptive_observations = {a, b};
    AnalysisReport first_report;
    std::string error;
    assert(load_and_analyze(first, first_report, error) && error.empty());

    a.hits = 999u;
    b.hits = 123u;
    DriverOptions second = first;
    second.output = root / "adaptive_hash_second";
    second.adaptive_observations = {b, a};
    AnalysisReport second_report;
    assert(load_and_analyze(second, second_report, error) && error.empty());
    assert(first_report.configuration_hash == second_report.configuration_hash);
    std::filesystem::remove_all(first.output);
    std::filesystem::remove_all(second.output);
  }
  std::cout << "  [PASS] Adaptive configuration hashing is order- and hit-count-independent\n";

  // Test 27 (conservative gap fill): an otherwise unreachable executable code
  // island may be recovered without a title-specific function list, but only
  // when a prologue begins on a structural boundary and the island reaches a
  // real terminator.
  {
    const auto hidden = base + 4u * 4u;
    const std::vector<std::uint32_t> words = {
        kBlr, 0u, 0u, 0u,
        mflr_word(0u), addi_word(3u, 3u, 1u), kBlr,
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "conservative_gap_fill"));
    const auto* recovered = find_function(report, hidden);
    assert(recovered != nullptr && recovered->compiled);
    assert(has_source(*recovered, DiscoverySource::GapRecovery));
    assert(has_source(*recovered, DiscoverySource::PrologueHeuristic));
    assert(report.diagnostics.gap_functions_recovered == 1u);
  }
  std::cout << "  [PASS] Conservative gap fill recovers bounded unowned code islands\n";

  // Test 28 (Gen 6 return classification): a real BLR is direct evidence that
  // the function may return normally.
  {
    const auto xex_bytes = make_xex({kBlr});
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "gen6_may_return"));
    const auto* function = find_function(report, base);
    assert(function != nullptr);
    assert(function->has_explicit_return);
    assert(function->return_behavior == ReturnBehavior::MayReturn);
    assert(report.diagnostics.inferred_may_return_functions >= 1u);
  }
  std::cout << "  [PASS] Gen 6 classifies explicit return paths as MayReturn\n";

  // Test 29 (Gen 6 fixed point): explicit NoReturn metadata on a terminal
  // callee propagates through a tail-call chain without treating ordinary
  // linked calls as non-returning.
  {
    const auto sink = base + 8u;
    const std::vector<std::uint32_t> words = {
        b_word(base, sink), 0u, b_word(sink, sink),
    };
    const auto xex_bytes = make_xex(words);
    xbox::XexImage image{};
    std::string parse_error;
    assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));

    AnalysisHintSetV2 hints{};
    hints.identity = xbox::compute_effective_identity(image);
    FunctionHint sink_hint{};
    sink_hint.address = sink;
    sink_hint.end = sink + 4u;
    sink_hint.name = "gen6_noreturn_sink";
    sink_hint.flags = FunctionFlags::NoReturn;
    hints.functions.push_back(sink_hint);

    AnalysisReport report;
    assert(run(xex_bytes, hints, report, root / "gen6_noreturn_fixed_point"));
    const auto* entry = find_function(report, base);
    const auto* target = find_function(report, sink);
    assert(entry != nullptr && target != nullptr);
    assert(target->return_behavior_explicit);
    assert(target->return_behavior == ReturnBehavior::NoReturn);
    assert(entry->return_behavior == ReturnBehavior::NoReturn);
    assert(report.diagnostics.inferred_no_return_functions >= 2u);
    assert(report.diagnostics.return_fixed_point_iterations >= 1u);
  }
  std::cout << "  [PASS] Gen 6 propagates NoReturn through terminal tail-call chains\n";

  // Test 30 (Gen 6 static pointer-table integration): load a call target from
  // immutable image bytes, feed it to CTR, and verify the driver carries the
  // dedicated resolver diagnostic/provenance into the discovered function.
  {
    const auto table = base + 8u * 4u;
    const auto target = base + 10u * 4u;
    const std::vector<std::uint32_t> words = {
        lis_word(10u, static_cast<std::uint16_t>(table >> 16u)),
        ori_word(10u, 10u, static_cast<std::uint16_t>(table)),
        lwz_word(11u, 10u, 0),
        mtctr_word(11u),
        kBctrl,
        kBlr,
        0u, 0u,
        target,
        0u,
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    AnalysisReport report;
    assert(run(xex_bytes, std::nullopt, report, root / "gen6_readonly_pointer_call"));
    const auto* recovered = find_function(report, target);
    assert(recovered != nullptr);
    assert(has_source(*recovered, DiscoverySource::ResolvedIndirectCall));
    assert(report.diagnostics.resolved_indirect_via_readonly_table >= 1u);
    assert(report.diagnostics.resolved_indirect_via_dataflow >= 1u);
  }
  std::cout << "  [PASS] Gen 6 resolves immutable pointer-table calls end-to-end\n";

  std::cout << "All Recomp Analysis V3 + Gen 6 discovery quality tests passed!\n";
  return 0;
}
