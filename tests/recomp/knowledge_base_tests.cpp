#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/core/json.hpp"
#include "xenon/recomp/driver.hpp"
#include "xenon/recomp/knowledge_base.hpp"

namespace {

void append_be32(std::vector<std::byte>& bytes, std::uint32_t word) {
  bytes.push_back(static_cast<std::byte>((word >> 24u) & 0xFFu));
  bytes.push_back(static_cast<std::byte>((word >> 16u) & 0xFFu));
  bytes.push_back(static_cast<std::byte>((word >> 8u) & 0xFFu));
  bytes.push_back(static_cast<std::byte>(word & 0xFFu));
}

std::uint32_t d_form(std::uint32_t opcode, std::uint32_t rt, std::uint32_t ra,
                     std::uint16_t imm) {
  return (opcode << 26u) | (rt << 21u) | (ra << 16u) | imm;
}

std::uint32_t branch_i(std::int32_t displacement, bool link) {
  return (18u << 26u) | (static_cast<std::uint32_t>(displacement) & 0x03FFFFFCu) |
         (link ? 1u : 0u);
}

struct Fixture {
  xenon::xbox::XexImage image;
  xenon::recomp::DiscoveredFunction function;
};

Fixture make_fixture(std::uint32_t base, std::uint16_t hi, std::uint16_t lo,
                     std::uint32_t external_target) {
  Fixture fixture;
  xenon::xbox::XexSection section{};
  section.name = ".text";
  section.virtual_address = base;
  section.executable = true;
  section.readable = true;
  // lis r3, hi ; ori r3,r3,lo ; addi r4,r3,1 ; bl external ; blr
  append_be32(section.bytes, d_form(15u, 3u, 0u, hi));
  append_be32(section.bytes, d_form(24u, 3u, 3u, lo));
  append_be32(section.bytes, d_form(14u, 4u, 3u, 1u));
  const auto call_site = base + 12u;
  append_be32(section.bytes, branch_i(static_cast<std::int32_t>(external_target - call_site), true));
  append_be32(section.bytes, 0x4E800020u);
  section.virtual_size = static_cast<std::uint32_t>(section.bytes.size());
  section.raw_size = section.virtual_size;
  fixture.image.sections.push_back(section);

  auto& function = fixture.function;
  function.guest_start = base;
  function.guest_end = base + 20u;
  function.ranges = {base, base + 20u};
  function.compiled = true;
  function.confidence = 90u;
  function.name = "xenon_fn_test";
  function.calls = {external_target};
  function.branches.push_back({call_site, external_target, false, false, true, false, true});

  xenon::cpu::ir::Block first{};
  first.guest_address = base;
  first.end_address = base + 16u;
  first.has_external_exit = true;
  first.successors.push_back({base + 16u, xenon::cpu::ir::EdgeKind::Fallthrough, true});
  xenon::cpu::ir::Block second{};
  second.guest_address = base + 16u;
  second.end_address = base + 20u;
  second.predecessors.push_back(base);
  second.has_external_exit = true;
  function.ir.guest_address = base;
  function.ir.blocks = {first, second};
  return fixture;
}

}  // namespace

int main() {
  using namespace xenon::recomp;

  // Same function at a different guest address, with a relocated immediate
  // address and a different direct-call displacement. Core normalized shape,
  // entry anchor and CFG remain stable; constants are intentionally separate.
  auto old_revision = make_fixture(0x82001000u, 0x8234u, 0x1000u, 0x82002000u);
  auto new_revision = make_fixture(0x83005000u, 0x8334u, 0x9000u, 0x83008000u);
  const auto old_fp = fingerprint_function(old_revision.image, old_revision.function);
  const auto new_fp = fingerprint_function(new_revision.image, new_revision.function);
  assert(old_fp.valid() && new_fp.valid());
  assert(old_fp.instruction_shape_hash == new_fp.instruction_shape_hash);
  assert(old_fp.entry_anchor_hash == new_fp.entry_anchor_hash);
  assert(old_fp.cfg_shape_hash == new_fp.cfg_shape_hash);
  assert(old_fp.constant_shape_hash != new_fp.constant_shape_hash);
  assert(old_fp.call_neighborhood_hash == new_fp.call_neighborhood_hash);

  KnowledgeRecord record{};
  record.id = "crt.memcpy.test";
  record.kind = KnowledgeKind::Crt;
  record.family = "msvcrt";
  record.label = "memcpy";
  record.fingerprint = old_fp;
  record.source_image_hash = "1111111111111111111111111111111111111111";
  record.source_address = old_revision.function.guest_start;
  record.confidence = 90u;
  record.observations = 4u;

  const std::vector<KnowledgeRecord> records{record};
  const auto matches = match_knowledge(
      new_fp, records, "2222222222222222222222222222222222222222", 70u);
  assert(matches.size() == 1u);
  assert(matches.front().cross_revision);
  assert(matches.front().instruction_shape);
  assert(matches.front().entry_anchor);
  assert(matches.front().cfg_shape);
  assert(!matches.front().constants);
  assert(matches.front().score >= 70u);

  auto low_confidence = record;
  low_confidence.confidence = 50u;
  const std::vector<KnowledgeRecord> low_records{low_confidence};
  assert(match_knowledge(new_fp, low_records,
                         "2222222222222222222222222222222222222222", 70u).empty());

  // CFG disagreement must reduce the evidence score; instruction bytes alone
  // are not treated as proof of function identity.
  auto changed_cfg = new_revision;
  changed_cfg.function.ir.blocks.front().successors.clear();
  const auto changed_cfg_fp = fingerprint_function(changed_cfg.image, changed_cfg.function);
  assert(changed_cfg_fp.instruction_shape_hash == old_fp.instruction_shape_hash);
  assert(changed_cfg_fp.cfg_shape_hash != old_fp.cfg_shape_hash);
  const auto weaker = match_knowledge(
      changed_cfg_fp, records, "2222222222222222222222222222222222222222", 0u);
  assert(!weaker.empty());
  assert(weaker.front().score < matches.front().score);

  // The cheap anchor is relocation-independent but is not a full fingerprint.
  const auto old_anchor = fingerprint_entry_anchor(old_revision.image, 0x82001000u, 4u);
  const auto new_anchor = fingerprint_entry_anchor(new_revision.image, 0x83005000u, 4u);
  assert(old_anchor.entry_anchor_hash == new_anchor.entry_anchor_hash);
  assert(old_anchor.instruction_count == 4u);

  // JSONL round trip preserves 64-bit hashes exactly (stored as hex strings).
  const auto path = std::filesystem::temp_directory_path() / "xenon-gen9-knowledge-test.jsonl";
  std::string error;
  assert(save_knowledge_base(path, records, error) && error.empty());
  std::vector<KnowledgeRecord> loaded;
  assert(load_knowledge_base(path, loaded, error) && error.empty());
  assert(loaded.size() == 1u);
  assert(loaded.front().id == record.id);
  assert(loaded.front().kind == KnowledgeKind::Crt);
  assert(loaded.front().fingerprint.instruction_shape_hash == old_fp.instruction_shape_hash);
  assert(loaded.front().fingerprint.cfg_shape_hash == old_fp.cfg_shape_hash);

  // Repeated observations from the same revision compact on persistence;
  // this is what lets --knowledge + --knowledge-export accumulate a DB
  // without appending an identical row on every analysis run.
  auto repeated_record = record;
  repeated_record.observations = 7u;
  repeated_record.confidence = 95u;
  std::vector<KnowledgeRecord> repeated_persist{record, repeated_record};
  assert(save_knowledge_base(path, repeated_persist, error) && error.empty());
  loaded.clear();
  assert(load_knowledge_base(path, loaded, error) && error.empty());
  assert(loaded.size() == 1u);
  assert(loaded.front().observations == 11u);
  assert(loaded.front().confidence == 95u);
  std::filesystem::remove(path);

  // Canonical DB identity ignores ordering/duplicates/observation hit counts.
  auto duplicate = record;
  duplicate.observations = 999u;
  std::vector<KnowledgeRecord> repeated{duplicate, record};
  assert(knowledge_base_fingerprint(repeated) == knowledge_base_fingerprint(records));
  duplicate.label = "different-semantic-label";
  std::vector<KnowledgeRecord> changed{duplicate};
  assert(knowledge_base_fingerprint(changed) != knowledge_base_fingerprint(records));

  // Analysis export creates reusable records without inventing a CRT/engine
  // identity for ordinary anonymous functions.
  AnalysisReport report{};
  report.image = old_revision.image;
  report.functions.push_back(old_revision.function);
  report.functions.front().fingerprint = old_fp;
  const auto exported = export_analysis_knowledge(
      report, "1111111111111111111111111111111111111111");
  assert(exported.size() == 1u);
  assert(exported.front().kind == KnowledgeKind::Function);
  assert(exported.front().fingerprint.instruction_shape_hash != 0u);


  // End-to-end cross-revision recovery: learn a function from one image, move
  // the exact code in a second image where nothing statically calls it, and
  // verify the cheap anchor nominates it while the full fingerprint is what
  // ultimately accepts it.
  {
    const auto make_analysis_image = [](std::uint32_t target, bool target_is_entry) {
      xenon::xbox::XexImage image{};
      xenon::xbox::XexSection section{};
      section.name = ".text";
      section.virtual_address = target;
      section.executable = true;
      section.readable = true;
      append_be32(section.bytes, d_form(14u, 3u, 3u, 1u));
      append_be32(section.bytes, d_form(24u, 4u, 4u, 2u));
      append_be32(section.bytes, d_form(14u, 5u, 5u, 3u));
      append_be32(section.bytes, 0x4E800020u);
      if (!target_is_entry) {
        for (int i = 0; i < 4; ++i) append_be32(section.bytes, 0u);
        append_be32(section.bytes, 0x4E800020u);
      }
      section.virtual_size = static_cast<std::uint32_t>(section.bytes.size());
      section.raw_size = section.virtual_size;
      image.sections.push_back(section);
      image.effective_image = section.bytes;
      image.image_base = target;
      image.entry_point = target_is_entry ? target : target + 32u;
      return image;
    };

    DriverOptions learn_options{};
    learn_options.pre_parsed_image = make_analysis_image(0x84001000u, true);
    learn_options.scan_static_pointer_tables = false;
    learn_options.recover_multi_source_orphans = false;
    learn_options.recover_unowned_gaps = false;
    AnalysisReport learned_report{};
    std::string analysis_error;
    assert(load_and_analyze(learn_options, learned_report, analysis_error));
    assert(analysis_error.empty());
    const auto old_hash = xenon::xbox::format_effective_image_hash(
        xenon::xbox::compute_effective_image_hash(learned_report.image));
    const auto learned_records = export_analysis_knowledge(learned_report, old_hash);
    assert(!learned_records.empty());

    DriverOptions reuse_options{};
    reuse_options.pre_parsed_image = make_analysis_image(0x85005000u, false);
    reuse_options.scan_static_pointer_tables = false;
    reuse_options.recover_multi_source_orphans = false;
    reuse_options.recover_unowned_gaps = false;
    reuse_options.knowledge_records = learned_records;
    AnalysisReport reused_report{};
    assert(load_and_analyze(reuse_options, reused_report, analysis_error));
    const auto moved = std::find_if(reused_report.functions.begin(), reused_report.functions.end(),
                                    [](const auto& fn) { return fn.guest_start == 0x85005000u; });
    assert(moved != reused_report.functions.end());
    assert(std::find(moved->sources.begin(), moved->sources.end(), DiscoverySource::KnowledgeMatch) !=
           moved->sources.end());
    assert(!moved->knowledge_matches.empty());
    assert(moved->knowledge_matches.front().cross_revision);
    assert(reused_report.diagnostics.knowledge_seed_candidates >= 1u);
    assert(reused_report.diagnostics.knowledge_cross_revision_matches >= 1u);
    xenon::core::JsonValue parsed_report;
    std::string json_error;
    assert(xenon::core::JsonValue::parse(format_report_json(reused_report), parsed_report, &json_error));
    assert(json_error.empty());
    assert(parsed_report.get_number("analysis_engine_revision") == 9.0);
  }

  std::cout << "Gen 9 knowledge-base tests passed\n";
  return 0;
}
