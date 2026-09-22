// Recomp Analysis V3 (discovery-quality pass) tests: provenance/evidence
// combination, confidence, generic XEX metadata seeding (TLS callbacks),
// bounded CTR dataflow resolution, invalid-vs-unsupported PPC
// classification, the conservative prologue heuristic, validated tail
// calls, and switch/jump-table bounds-check gating. Same small hand-built
// synthetic XEX2 fixture technique as tests/recomp/analysis_v2_consumption_tests.cpp.

#include <algorithm>
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

bool has_source(const DiscoveredFunction& function, DiscoverySource source) {
  return std::find(function.sources.begin(), function.sources.end(), source) != function.sources.end();
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
    assert(has_source(*callee, DiscoverySource::ResolvedIndirect));
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
    assert(entry != nullptr && entry->compiled && entry->guest_end == base + 5u * 4u);
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

  // Test 13 (recursive-explosion guard): a candidate reached ONLY via a
  // plain direct branch (no seed, no prologue match - "weak" provenance)
  // must still be preserved as a real function (never silently dropped),
  // but must NOT itself get full discovery authority: its own terminal
  // branch to a further, otherwise-unreachable address must not spawn yet
  // another function. The rejected target still surfaces via the existing
  // branch-into-unknown-code diagnostic, never silently lost. Also proves
  // this is deterministic: jobs=1 and jobs=8 must agree exactly.
  {
    // word0: entry, terminal branch -> word1 (B: reached ONLY this way)
    // word1: addi r3,r3,5           (B's body - deliberately not a
    //                                 recognized prologue opening)
    // word2: terminal branch -> word4 (C - must NOT be promoted: B has no
    //                                 independent evidence of its own)
    // word3: blr                     (padding, never reached)
    // word4: blr                     (C - must remain undiscovered)
    const std::vector<std::uint32_t> words = {
        b_word(base + 0u * 4u, base + 1u * 4u),
        addi_word(3u, 3u, 5u),
        b_word(base + 2u * 4u, base + 4u * 4u),
        kBlr,
        kBlr,
    };
    const auto xex_bytes = make_xex(words);
    const auto weak_b = base + 1u * 4u;
    const auto rejected_c = base + 4u * 4u;

    AnalysisReport report_serial;
    assert(run(xex_bytes, std::nullopt, report_serial, root / "weak_chain_serial", std::nullopt, 1u));
    AnalysisReport report_parallel;
    assert(run(xex_bytes, std::nullopt, report_parallel, root / "weak_chain_parallel", std::nullopt, 8u));

    for (const auto* report : {&report_serial, &report_parallel}) {
      assert(report->functions.size() == 2u &&
             "the weak candidate must be preserved, but must not seed a third function");
      const auto* entry = find_function(*report, base);
      assert(entry != nullptr && entry->compiled);
      assert(has_source(*entry, DiscoverySource::EntryPoint));
      const auto* weak = find_function(*report, weak_b);
      assert(weak != nullptr && weak->compiled &&
             "a weak (direct-branch-only) candidate must still be compiled and reported");
      // The post-wave cross-reference pass retroactively adds
      // ValidatedTailCall too (A's branch_references still records this
      // target regardless of the promotion gate below), so the FINAL
      // reported evidence is {DirectBranch, ValidatedTailCall} - but both
      // are exactly the "weak" sources this guard exists for (never
      // EntryPoint/Export/ModuleHint/UnwindMetadata/TlsCallback/DirectCall/
      // ResolvedIndirect/PrologueHeuristic). The gating decision itself
      // (see self_confirmed in analyze_function_candidate()) correctly used
      // only the evidence available AT CLAIM TIME, before this retroactive
      // pass ever runs.
      assert(has_source(*weak, DiscoverySource::DirectBranch) &&
             std::none_of(weak->sources.begin(), weak->sources.end(),
                         [](DiscoverySource source) {
                           return source != DiscoverySource::DirectBranch &&
                                  source != DiscoverySource::ValidatedTailCall;
                         }) &&
             "this candidate's only evidence must be the plain direct branch that found it "
             "(plus the retroactive validated-tail-call cross-reference)");
      assert(find_function(*report, rejected_c) == nullptr &&
             "a weak candidate's own branch target must not gain full discovery authority");
      assert(std::any_of(report->unresolved.begin(), report->unresolved.end(), [&](const auto& item) {
        return item.kind == "branch-into-unknown-code" && item.address == weak_b && item.target == rejected_c;
      }) && "the denied target must still surface as an explicit diagnostic, never silently dropped");
    }
    assert(report_serial.functions.size() == report_parallel.functions.size());
    assert(report_serial.diagnostics.analysis_waves == report_parallel.diagnostics.analysis_waves &&
           "discovery must be deterministic regardless of worker count");
  }
  std::cout << "  [PASS] A weak candidate is preserved but denied further discovery authority (deterministic)\n";

  // Test 14 (diagnostics deduplication): the exact same unresolved
  // branch-into-unknown-code site, referenced by two different branch
  // instructions within one function, must collapse into a single
  // unresolved entry, not two.
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
    assert(count == 1u &&
           "the same (kind, address, target, detail) unresolved site must be deduplicated to one entry");
  }
  std::cout << "  [PASS] Duplicate unresolved diagnostics for the same site collapse to one entry\n";

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
    assert(callee != nullptr && callee->compiled && has_source(*callee, DiscoverySource::ResolvedIndirect));
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
    assert(callee != nullptr && callee->compiled && has_source(*callee, DiscoverySource::ResolvedIndirect));
    assert(report.diagnostics.resolved_indirect_via_jump_table >= 1u);
    assert(report.diagnostics.resolved_indirect_via_dataflow == 0u);
    assert(report.diagnostics.functions_with_resolved_indirect_provenance == 1u);
    std::filesystem::remove_all(root / "resolved_indirect_jump_table");
  }
  std::cout << "  [PASS] Jump-table-resolved-indirect provenance and diagnostics agree, distinctly from dataflow\n";

  std::cout << "All Recomp Analysis V3 discovery quality tests passed!\n";
  return 0;
}
