// Revision mapping: carrying a module's hints from one executable revision of
// a title to another (AC6 US retail -> Europe Rev 1 is the real case).
//
// Synthetic revisions: the target inserts code before and between functions
// (so every branch displacement changes), swaps the order of two look-alike
// thunks and changes their `this` adjustment, as a later build with a grown
// class layout does.

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/recomp/revision_mapping.hpp"

using namespace xenon::recomp;

namespace {

constexpr std::uint32_t kBase = 0x82000000u;

// Distinct immediate-free words (or rA,rS,rB), so anchors are unique.
std::uint32_t distinct(std::uint32_t i) { return 0x7C000378u | ((i >> 5u) & 31u) << 16u | (i & 31u) << 11u; }
std::uint32_t branch(std::uint32_t from, std::uint32_t to, bool link) {
  return 0x48000000u | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}
std::uint32_t addi_r3(std::int16_t value) { return 0x38630000u | static_cast<std::uint16_t>(value); }

xenon::xbox::XexImage image(const std::vector<std::uint32_t>& words) {
  xenon::xbox::XexImage image{};
  xenon::xbox::XexSection text{};
  text.name = ".text";
  text.virtual_address = kBase;
  text.virtual_size = static_cast<std::uint32_t>(words.size() * 4u);
  text.executable = true;
  for (const auto word : words)
    for (int shift = 24; shift >= 0; shift -= 8)
      text.bytes.push_back(static_cast<std::byte>(word >> shift));
  image.sections.push_back(std::move(text));
  return image;
}

std::uint32_t at(std::size_t index) { return kBase + static_cast<std::uint32_t>(index * 4u); }

}  // namespace

int main() {
  // The relocation-insensitive form keeps opcodes and registers, drops
  // displacements and address immediates.
  assert(relocation_insensitive_word(0x48001234u) == relocation_insensitive_word(0x48005678u));
  assert(relocation_insensitive_word(0x48001235u) != relocation_insensitive_word(0x48001234u));  // bl vs b
  assert(relocation_insensitive_word(0x3D608201u) == relocation_insensitive_word(0x3D608209u));  // lis
  assert(relocation_insensitive_word(0x7C000378u) == 0x7C000378u);

  // Source: function A (40 words, calls thunk1 at A[10]), thunk1 (b A[0]),
  // thunk2 (b A[5], called by nothing), function B (40 words).
  std::vector<std::uint32_t> source;
  for (std::uint32_t i = 0; i < 40; ++i) source.push_back(distinct(1 + i));
  const std::size_t s_thunk1 = 40, s_thunk2 = 42, s_b = 44;
  source[10] = branch(at(10), at(s_thunk1), true);
  source[39] = 0x4E800020u;  // blr
  source.push_back(addi_r3(-832));
  source.push_back(branch(at(41), at(0), false));
  source.push_back(addi_r3(-832));
  source.push_back(branch(at(43), at(5), false));
  for (std::uint32_t i = 0; i < 40; ++i) source.push_back(distinct(101 + i));

  // Target: two inserted words, A, the thunks swapped with -840, three
  // inserted words, B.
  std::vector<std::uint32_t> target = {distinct(500), distinct(501)};
  const std::size_t t_a = 2, t_thunk2 = 42, t_thunk1 = 44, t_b = 49;
  for (std::uint32_t i = 0; i < 40; ++i) target.push_back(distinct(1 + i));
  target[t_a + 10] = branch(at(t_a + 10), at(t_thunk1), true);
  target[t_a + 39] = 0x4E800020u;
  target.push_back(addi_r3(-840));
  target.push_back(branch(at(43), at(t_a + 5), false));
  target.push_back(addi_r3(-840));
  target.push_back(branch(at(45), at(t_a), false));
  for (std::uint32_t i = 0; i < 3; ++i) target.push_back(distinct(600 + i));
  for (std::uint32_t i = 0; i < 40; ++i) target.push_back(distinct(101 + i));

  const auto map = RevisionAddressMap::build(image(source), image(target));
  assert(map.source_words() == source.size());

  // Aligned code translates despite the shifted branches.
  assert(map.translate(at(0)) == at(t_a));
  assert(map.translate(at(10)) == at(t_a + 10));
  assert(map.translate(at(s_b)) == at(t_b));
  assert(map.translate(at(s_b + 39)) == at(t_b + 39));
  assert(!map.translate(at(s_b + 40)));  // past the end
  assert(!map.translate(at(1) + 2u));    // not an instruction

  // Extension past A must not pair thunk1 with thunk2, which now sits in its
  // place: thunk1 branches to A[0], the word there branches to A[5].
  assert(!map.translate(at(s_thunk1)));
  assert(!map.translate(at(s_thunk2)));

  // thunk1 is placed by its caller, thunk2 (no callers) by its body: the
  // one target thunk whose branch lands where A[5] maps. Neither matches
  // exactly (-832 vs -840), as after a class layout change.
  assert(map.translate_entry(at(s_thunk1)) == at(t_thunk1));
  assert(map.translate_entry(at(s_thunk2)) == at(t_thunk2));
  assert(!map.translate_entry(kBase + 0x10000u));  // outside the code

  // Porting a hint set: code addresses translate, data does not, and every
  // hint left behind is reported.
  xenon::recomp::analysis::AnalysisHintSetV2 hints{};
  hints.functions = {{.address = at(0), .end = at(40), .name = "a"},
                     {.address = at(s_thunk1), .name = "thunk1"},
                     {.address = at(s_thunk2), .name = "thunk2"},
                     {.address = at(s_b), .name = "b"}};
  xenon::recomp::analysis::SwitchTableHint table{};
  table.site = at(20);
  table.table_address = 0x82100000u;
  table.entry_count = 4;
  hints.switches.push_back(table);
  xenon::recomp::analysis::HookDeclaration hook{};
  hook.address = at(3);
  hook.kind = xenon::recomp::analysis::HookKind::Instruction;
  hook.symbol = "probe";
  hook.on_true = xenon::recomp::analysis::HookAction::Jump;
  hook.jump_on_true = at(7);
  xenon::recomp::analysis::HookDeclaration lost = hook;
  lost.address = kBase + 0x10000u;
  hints.hooks = {hook, lost};
  xenon::xbox::XexEffectiveIdentity identity{};
  identity.media_id = 0x0379EFB3u;

  HintPortReport report;
  const auto ported = port_hint_set(hints, map, identity, report);
  assert(ported.identity.media_id == 0x0379EFB3u);
  assert(ported.functions.size() == 4);
  assert(ported.functions[0].address == at(t_a) && ported.functions[0].end == at(t_a + 40));
  assert(ported.functions[1].address == at(t_thunk1));
  assert(ported.functions[2].address == at(t_thunk2));
  assert(ported.functions[3].address == at(t_b));
  assert(report.by_reference == 2);
  assert(ported.switches.empty());  // the table is data
  assert(ported.hooks.size() == 1);
  assert(ported.hooks[0].address == at(t_a + 3) && ported.hooks[0].jump_on_true == at(t_a + 7));
  assert(report.mapped == 5);
  assert(report.unmapped.size() == 2);
  std::cout << "revision mapping tests passed\n";
  return 0;
}
