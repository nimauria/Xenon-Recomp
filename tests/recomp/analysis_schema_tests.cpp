// Unit tests for Analysis Hint Schema V2 (Part 1 / Part 7 "Analysis Schema"
// of the Gracemeria readiness pass): validate_hint_set()'s structural
// checks, hint_set_matches_identity()'s revision scoping, and the JSON
// (de)serialization round trip - all independent of any XEX/driver
// pipeline. See tests/recomp/analysis_v2_consumption_tests.cpp for proof the
// Recomp Driver actually *uses* this metadata during analysis.

#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/analysis_schema_json.hpp"

#include <cassert>
#include <iostream>

using namespace xenon::recomp::analysis;
namespace xbox = xenon::xbox;
namespace core = xenon::core;

namespace {

xbox::XexEffectiveIdentity make_identity(std::uint32_t title_id, std::byte hash_seed) {
  xbox::XexEffectiveIdentity identity{};
  identity.title_id = title_id;
  identity.media_id = 0x1234u;
  identity.base_version.value = 1;
  identity.effective_version.value = 1;
  identity.effective_image_hash.fill(hash_seed);
  return identity;
}

}  // namespace

int main() {
  std::cout << "Testing Analysis Hint Schema V2...\n";

  // Test 1: a minimal, well-formed hint set validates cleanly.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(0x41560001u, std::byte{0xAA});
    FunctionHint fn{};
    fn.address = 0x1000;
    fn.end = 0x1010;
    fn.name = "fn_1000";
    hint_set.functions.push_back(fn);
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] Minimal valid hint set passes validation\n";

  // Test 2: function end <= address is rejected.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    FunctionHint fn{};
    fn.address = 0x2000;
    fn.end = 0x1000;  // before address
    hint_set.functions.push_back(fn);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
    assert(!errors.empty());
  }
  std::cout << "  [PASS] Function with end <= address is rejected\n";

  // Test 3: function size 0 is rejected.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    FunctionHint fn{};
    fn.address = 0x3000;
    fn.size = 0;
    hint_set.functions.push_back(fn);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] Function with size 0 is rejected\n";

  // Test 4: chunks + valid parent relationship.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    FunctionHint parent{};
    parent.address = 0x4000;
    hint_set.functions.push_back(parent);
    FunctionChunk chunk{};
    chunk.start = 0x5000;
    chunk.end = 0x5040;
    chunk.parent_function = 0x4000;
    hint_set.chunks.push_back(chunk);
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] Function chunk with a valid parent relationship validates\n";

  // Test 5: chunk referencing a non-existent parent function is rejected.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    FunctionChunk chunk{};
    chunk.start = 0x5000;
    chunk.end = 0x5040;
    chunk.parent_function = 0x9999;  // no such function hint
    hint_set.chunks.push_back(chunk);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] Chunk with an unresolvable parent function is rejected\n";

  // Test 6: two overlapping chunks with DIFFERENT parents is a contradiction.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.functions.push_back(FunctionHint{0x4000, {}, {}, "", {}, {}});
    hint_set.functions.push_back(FunctionHint{0x4100, {}, {}, "", {}, {}});
    FunctionChunk a{0x5000, 0x5100, 0x4000};
    FunctionChunk b{0x5050, 0x5150, 0x4100};  // overlaps a, different parent
    hint_set.chunks.push_back(a);
    hint_set.chunks.push_back(b);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] Overlapping chunks with contradictory parents are rejected\n";

  // Test 7: a parent-function cycle (A's parent is B, B's parent is A) is rejected.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    FunctionHint a{}; a.address = 0x1000; a.parent_function = 0x2000;
    FunctionHint b{}; b.address = 0x2000; b.parent_function = 0x1000;
    hint_set.functions.push_back(a);
    hint_set.functions.push_back(b);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
    bool found_cycle_message = false;
    for (const auto& message : errors)
      if (message.find("cycle") != std::string::npos) found_cycle_message = true;
    assert(found_cycle_message);
  }
  std::cout << "  [PASS] A parent-function cycle is detected and rejected\n";

  // Test 8: a function declaring itself as its own parent is rejected.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    FunctionHint a{}; a.address = 0x1000; a.parent_function = 0x1000;
    hint_set.functions.push_back(a);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] A function that is its own parent is rejected\n";

  // Test 9: explicit switch targets are enough on their own (no table shape needed).
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    SwitchTableHint table{};
    table.site = 0x1000;
    table.explicit_targets = {0x2000, 0x2010, 0x2020};
    hint_set.switches.push_back(table);
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] Switch table hint with only explicit targets validates\n";

  // Test 10: a switch table with neither a decodable shape nor explicit
  // targets is malformed.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    SwitchTableHint table{};
    table.site = 0x1000;
    hint_set.switches.push_back(table);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] Switch table hint with no targets and no table shape is rejected\n";

  // Test 11: a switch table giving only table_address (no entry_count, no
  // explicit_targets) is malformed - the analyzer cannot act on it.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    SwitchTableHint table{};
    table.site = 0x1000;
    table.table_address = 0x3000;
    hint_set.switches.push_back(table);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] Switch table hint with only table_address (no entry_count) is rejected\n";

  // Test 12: known indirect calls/branches with explicit or empty target
  // lists both validate (an empty list is a real, meaningful
  // "acknowledged but unresolved" state, not malformed).
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.indirect_calls.push_back(KnownIndirectCall{0x1000, {0x2000, 0x2010}});
    hint_set.indirect_calls.push_back(KnownIndirectCall{0x1010, {}});
    hint_set.indirect_branches.push_back(KnownIndirectBranch{0x1020, {0x3000}});
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] Known indirect call/branch hints (with or without targets) validate\n";

  // Test 13: data/ignored/invalid regions - valid ranges pass, end <= start fails.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.regions.push_back(RegionHint{0x1000, 0x1100, RegionKind::Data, "embedded table"});
    hint_set.regions.push_back(RegionHint{0x2000, 0x2010, RegionKind::Ignored, ""});
    hint_set.regions.push_back(RegionHint{0x3000, 0x3004, RegionKind::InvalidInstruction, "obfuscated"});
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] Data/ignored/invalid-instruction regions validate\n";

  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.regions.push_back(RegionHint{0x1100, 0x1000, RegionKind::Data, ""});  // end < start
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] A region with end <= start is rejected\n";

  // Test 14: overlapping regions of DIFFERENT kinds is a contradiction;
  // overlapping regions of the SAME kind is redundant but not an error.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.regions.push_back(RegionHint{0x1000, 0x1100, RegionKind::Data, ""});
    hint_set.regions.push_back(RegionHint{0x1050, 0x1150, RegionKind::InvalidInstruction, ""});
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.regions.push_back(RegionHint{0x1000, 0x1100, RegionKind::Data, ""});
    hint_set.regions.push_back(RegionHint{0x1050, 0x1150, RegionKind::Data, ""});
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] Overlapping regions: contradictory kinds rejected, same kind allowed\n";

  // Test 15: native replacement identities round-trip through name lookup.
  {
    assert(native_replacement_kind_from_name("memcpy") == NativeReplacementKind::Memcpy);
    assert(native_replacement_kind_from_name("RtlAllocateHeap") == NativeReplacementKind::HeapAllocate);
    assert(native_replacement_kind_from_name("XMemCpy") == NativeReplacementKind::Unsupported &&
           "an unverified/unrecognized rexcrt name must report Unsupported, never guess an "
           "equivalent implementation");
    assert(std::string(native_replacement_kind_name(NativeReplacementKind::Memcpy)) == "Memcpy");
  }
  std::cout << "  [PASS] Native replacement name<->kind mapping is correct and conservative\n";

  // Test 16: setjmp/longjmp RuntimeHelper - at most one of each kind.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000, RuntimeHelperKind::SetJmp});
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1010, RuntimeHelperKind::LongJmp});
    std::vector<std::string> errors;
    assert(validate_hint_set(hint_set, errors) && errors.empty());
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1020, RuntimeHelperKind::SetJmp});  // duplicate
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] setjmp/longjmp RuntimeHelper metadata validates; duplicates rejected\n";

  // Test 17: an unsupported schema version is rejected outright.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.schema_version = 999;
    hint_set.identity = make_identity(1, std::byte{1});
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
    assert(!errors.empty());
  }
  std::cout << "  [PASS] An unsupported schema version is rejected\n";

  // Test 18: revision scoping - hint_set_matches_identity() requires an
  // EXACT match (title_id, media_id, effective_image_hash), never the
  // closest-looking revision.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(0x41560001u, std::byte{0xAA});
    const auto matching = make_identity(0x41560001u, std::byte{0xAA});
    auto wrong_hash = matching;
    wrong_hash.effective_image_hash.fill(std::byte{0xBB});
    auto wrong_title = matching;
    wrong_title.title_id = 0x41560002u;

    assert(hint_set_matches_identity(hint_set, matching));
    assert(!hint_set_matches_identity(hint_set, wrong_hash) &&
           "a hint set must never match an executable with a different effective image hash "
           "(e.g. base XEX vs a title-update-patched revision)");
    assert(!hint_set_matches_identity(hint_set, wrong_title));
  }
  std::cout << "  [PASS] hint_set_matches_identity() requires an exact revision match\n";

  // Test 19: JSON round trip preserves every field category.
  {
    AnalysisHintSetV2 original{};
    original.module_name = "test_module";
    original.identity = make_identity(0x41560001u, std::byte{0x42});
    original.identity.title_update_applied = true;
    FunctionHint fn{};
    fn.address = 0x1000;
    fn.end = 0x1040;
    fn.name = "fn_1000";
    fn.flags = FunctionFlags::Leaf | FunctionFlags::NoReturn;
    original.functions.push_back(fn);
    original.chunks.push_back(FunctionChunk{0x2000, 0x2040, 0x1000});
    SwitchTableHint table{};
    table.site = 0x3000;
    table.table_address = 0x4000;
    table.entry_count = 4;
    table.entry_format = SwitchEntryFormat::RelativeWord32;
    table.index_register = 5;
    original.switches.push_back(table);
    original.indirect_calls.push_back(KnownIndirectCall{0x5000, {0x6000}});
    original.indirect_branches.push_back(KnownIndirectBranch{0x5010, {}});
    original.native_replacements.push_back(NativeReplacement{0x7000, NativeReplacementKind::Memcpy, "memcpy"});
    original.runtime_helpers.push_back(RuntimeHelper{0x8000, RuntimeHelperKind::SetJmp});
    original.regions.push_back(RegionHint{0x9000, 0x9010, RegionKind::Data, "table"});
    original.symbols.push_back(SymbolHint{0xA000, "g_symbol"});
    original.patches.push_back(PatchDeclaration{0xB000, "DEADBEEF", "nop out a check"});
    original.hooks.push_back(HookDeclaration{0xC000, "custom_hook", "fix a rendering glitch"});

    const auto json = to_json(original);
    const auto text = json.dump();
    core::JsonValue reparsed;
    std::string parse_error;
    assert(core::JsonValue::parse(text, reparsed, &parse_error));

    AnalysisHintSetV2 restored{};
    std::string from_json_error;
    assert(from_json(reparsed, restored, from_json_error));

    assert(restored.module_name == original.module_name);
    assert(restored.identity.title_id == original.identity.title_id);
    assert(restored.identity.effective_image_hash == original.identity.effective_image_hash);
    assert(restored.identity.title_update_applied == original.identity.title_update_applied);
    assert(restored.functions.size() == 1 && restored.functions[0].address == 0x1000);
    assert(restored.functions[0].end == 0x1040);
    assert(has_flag(restored.functions[0].flags, FunctionFlags::Leaf));
    assert(has_flag(restored.functions[0].flags, FunctionFlags::NoReturn));
    assert(restored.chunks.size() == 1 && restored.chunks[0].parent_function == 0x1000);
    assert(restored.switches.size() == 1 && restored.switches[0].entry_format == SwitchEntryFormat::RelativeWord32);
    assert(restored.switches[0].entry_count == 4);
    assert(restored.indirect_calls.size() == 1 && restored.indirect_calls[0].targets.size() == 1);
    assert(restored.native_replacements.size() == 1 &&
           restored.native_replacements[0].kind == NativeReplacementKind::Memcpy);
    assert(restored.runtime_helpers.size() == 1 &&
           restored.runtime_helpers[0].kind == RuntimeHelperKind::SetJmp);
    assert(restored.regions.size() == 1 && restored.regions[0].kind == RegionKind::Data);
    assert(restored.symbols.size() == 1 && restored.symbols[0].name == "g_symbol");
    assert(restored.patches.size() == 1 && restored.patches[0].patch_bytes_hex == "DEADBEEF");
    assert(restored.hooks.size() == 1 && restored.hooks[0].native_replacement_identity == "custom_hook");

    std::vector<std::string> errors;
    assert(validate_hint_set(restored, errors) && errors.empty());
  }
  std::cout << "  [PASS] JSON round trip preserves every schema category and stays valid\n";

  // Test 20: malformed JSON input (not an object) is rejected cleanly.
  {
    core::JsonValue array = core::JsonValue::make_array();
    AnalysisHintSetV2 out{};
    std::string error;
    assert(!from_json(array, out, error));
    assert(!error.empty());
  }
  std::cout << "  [PASS] Malformed (non-object) analysis JSON is rejected\n";

  // Test 21: expand_runtime_helpers() reproduces XenonRecomp's own
  // Analyse() address/register-count formula exactly (Part 1.5/1.6) for
  // every register-range kind - a compact single-address declaration must
  // expand into one entry per callable register-count variant.
  {
    RuntimeHelper gpr_restore{0x831B0B40u, RuntimeHelperKind::RestoreGprLr, 14u};
    const auto expanded = expand_runtime_helpers({gpr_restore});
    assert(expanded.size() == 18);  // registers 14..31 inclusive
    assert(expanded.front().address == 0x831B0B40u && expanded.front().register_start == 14u);
    assert(expanded.back().register_start == 31u);
    assert(expanded.back().address == 0x831B0B40u + (31u - 14u) * 4u);

    RuntimeHelper vmx_save{0x831B3450u, RuntimeHelperKind::SaveVmx, 14u};
    const auto vmx_expanded = expand_runtime_helpers({vmx_save});
    assert(vmx_expanded.size() == 18);
    // Code stride (address spacing between register-count entry points) is
    // 8 bytes for VMX (an index-register-setup instruction plus one indexed
    // vector store/load) - distinct from the 16-byte STACK DATA spacing
    // between register slots (128-bit vectors), which is a different axis.
    assert(vmx_expanded[1].address == 0x831B3450u + 8u);

    RuntimeHelper vmx128_restore{0x831B377Cu, RuntimeHelperKind::RestoreVmx128, 64u};
    const auto vmx128_expanded = expand_runtime_helpers({vmx128_restore});
    assert(vmx128_expanded.size() == 64);  // registers 64..127 inclusive
    assert(vmx128_expanded.front().register_start == 64u);
    assert(vmx128_expanded.back().register_start == 127u);

    // SetJmp/LongJmp pass through unchanged (exactly one address each).
    RuntimeHelper setjmp{0x9000u, RuntimeHelperKind::SetJmp};
    const auto setjmp_expanded = expand_runtime_helpers({setjmp});
    assert(setjmp_expanded.size() == 1 && setjmp_expanded[0].address == 0x9000u &&
           !setjmp_expanded[0].register_start.has_value());
  }
  std::cout << "  [PASS] expand_runtime_helpers() derives every register-range variant address\n";

  // Test 22: register_start out of the family's valid range is rejected;
  // a register-range kind missing register_start entirely is rejected;
  // SetJmp/LongJmp declaring one is rejected.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveGprLr, 13u});  // too low
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));

    hint_set.runtime_helpers.clear();
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveVmx128, 63u});  // too low
    assert(!validate_hint_set(hint_set, errors));

    hint_set.runtime_helpers.clear();
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveGprLr});  // missing register_start
    assert(!validate_hint_set(hint_set, errors));

    hint_set.runtime_helpers.clear();
    RuntimeHelper bad_setjmp{0x1000u, RuntimeHelperKind::SetJmp};
    bad_setjmp.register_start = 14u;
    hint_set.runtime_helpers.push_back(bad_setjmp);
    assert(!validate_hint_set(hint_set, errors));

    hint_set.runtime_helpers.clear();
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveGprLr, 14u});  // valid
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] register_start range/presence is validated per RuntimeHelperKind\n";

  // Test 23: at most one RuntimeHelper per kind now covers all 10 kinds, not
  // just SetJmp/LongJmp.
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveGprLr, 14u});
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x2000u, RuntimeHelperKind::SaveGprLr, 14u});  // duplicate kind
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] More than one RuntimeHelper of the same register-range kind is rejected\n";

  // Test 24: a post-expansion address collision between two different
  // families is rejected (would otherwise silently break codegen's
  // address->case dispatch).
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveGprLr, 14u});
    // SaveFpr's family expands with the same base address and stride 4,
    // so its variant addresses collide exactly with SaveGprLr's above.
    hint_set.runtime_helpers.push_back(RuntimeHelper{0x1000u, RuntimeHelperKind::SaveFpr, 14u});
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));
  }
  std::cout << "  [PASS] Post-expansion RuntimeHelper address collisions are rejected\n";

  // Test 25: InstructionPatternHint structural validation (Part 2).
  {
    AnalysisHintSetV2 hint_set{};
    hint_set.identity = make_identity(1, std::byte{1});
    InstructionPatternHint pattern{};
    pattern.value = 0x00000000u;
    pattern.skip_bytes = 3u;  // not a multiple of 4
    hint_set.instruction_patterns.push_back(pattern);
    std::vector<std::string> errors;
    assert(!validate_hint_set(hint_set, errors));

    hint_set.instruction_patterns.clear();
    pattern.skip_bytes = 4u;
    pattern.mask = 0u;  // matches everything
    hint_set.instruction_patterns.push_back(pattern);
    assert(!validate_hint_set(hint_set, errors));

    hint_set.instruction_patterns.clear();
    pattern.mask = 0xFFFFFFFFu;
    pattern.scope_start = 0x2000u;
    pattern.scope_end = 0x1000u;  // end <= start
    hint_set.instruction_patterns.push_back(pattern);
    assert(!validate_hint_set(hint_set, errors));

    hint_set.instruction_patterns.clear();
    pattern.scope_start.reset();
    pattern.scope_end.reset();
    hint_set.instruction_patterns.push_back(pattern);
    assert(validate_hint_set(hint_set, errors) && errors.empty());
  }
  std::cout << "  [PASS] InstructionPatternHint structural validation catches malformed rules\n";

  // Test 26: RuntimeHelper.register_start and instruction_patterns round trip
  // through JSON, and old JSON lacking these newer fields entirely still
  // parses (backward compatibility, Part 3).
  {
    AnalysisHintSetV2 original{};
    original.identity = make_identity(1, std::byte{2});
    original.runtime_helpers.push_back(RuntimeHelper{0x831B0AF0u, RuntimeHelperKind::SaveGprLr, 14u});
    InstructionPatternHint pattern{};
    pattern.value = 0x00485645u;
    pattern.mask = 0xFFFFFFFFu;
    pattern.skip_bytes = 4u;
    pattern.reason = "End of .text";
    original.instruction_patterns.push_back(pattern);

    const auto json = to_json(original);
    const auto text = json.dump();
    core::JsonValue reparsed;
    std::string parse_error;
    assert(core::JsonValue::parse(text, reparsed, &parse_error));

    AnalysisHintSetV2 restored{};
    std::string from_json_error;
    assert(from_json(reparsed, restored, from_json_error));
    assert(restored.runtime_helpers.size() == 1 &&
           restored.runtime_helpers[0].kind == RuntimeHelperKind::SaveGprLr &&
           restored.runtime_helpers[0].register_start == 14u);
    assert(restored.instruction_patterns.size() == 1 &&
           restored.instruction_patterns[0].value == 0x00485645u &&
           restored.instruction_patterns[0].reason == "End of .text");
    std::vector<std::string> errors;
    assert(validate_hint_set(restored, errors) && errors.empty());

    // Simulate a pre-existing hints.v2.json written before this pass: no
    // "registerStart" field on a runtimeHelpers entry, no
    // "instructionPatterns" key at all.
    const std::string legacy_text =
        "{\"schemaVersion\":2,\"moduleName\":\"m\",\"identity\":{\"titleId\":1,\"mediaId\":1,"
        "\"baseVersion\":1,\"effectiveVersion\":1,"
        "\"effectiveImageHash\":\"0202020202020202020202020202020202020202\","
        "\"titleUpdateApplied\":false},"
        "\"functions\":[],\"chunks\":[],\"switches\":[],\"indirectCalls\":[],\"indirectBranches\":[],"
        "\"nativeReplacements\":[],"
        "\"runtimeHelpers\":[{\"address\":36864,\"kind\":\"SetJmp\"}],"
        "\"regions\":[],\"symbols\":[],\"patches\":[],\"hooks\":[]}";
    core::JsonValue legacy_json;
    assert(core::JsonValue::parse(legacy_text, legacy_json, &parse_error));
    AnalysisHintSetV2 legacy_restored{};
    assert(from_json(legacy_json, legacy_restored, from_json_error));
    assert(legacy_restored.runtime_helpers.size() == 1 &&
           legacy_restored.runtime_helpers[0].kind == RuntimeHelperKind::SetJmp &&
           !legacy_restored.runtime_helpers[0].register_start.has_value());
    assert(legacy_restored.instruction_patterns.empty());
    assert(validate_hint_set(legacy_restored, errors) && errors.empty());
  }
  std::cout << "  [PASS] RuntimeHelper.register_start/instruction_patterns round-trip through JSON; "
               "legacy JSON without either still loads\n";

  // Test 27: UTF-8 BOM handling is centralized in JsonValue::parse so
  // PowerShell 5.1-generated JSON is accepted by every Xenon consumer.
  {
    const std::string bom_json = std::string("\xEF\xBB\xBF") +
                                 "{\"schemaVersion\":2,\"moduleName\":\"bom\",\"identity\":{"
                                 "\"titleId\":1,\"mediaId\":1,\"baseVersion\":1,\"effectiveVersion\":1,"
                                 "\"effectiveImageHash\":\"0202020202020202020202020202020202020202\","
                                 "\"titleUpdateApplied\":false},\"functions\":[],\"chunks\":[],\"switches\":[],"
                                 "\"indirectCalls\":[],\"indirectBranches\":[],\"nativeReplacements\":[],"
                                 "\"runtimeHelpers\":[],\"regions\":[],\"symbols\":[],\"patches\":[],\"hooks\":[]}";
    core::JsonValue parsed;
    std::string parse_error;
    assert(core::JsonValue::parse(bom_json, parsed, &parse_error));
    AnalysisHintSetV2 restored{};
    std::string from_json_error;
    assert(from_json(parsed, restored, from_json_error));
    assert(restored.module_name == "bom");
  }
  std::cout << "  [PASS] UTF-8 BOM-prefixed JSON parses normally\n";

  std::cout << "All Analysis Hint Schema V2 tests passed!\n";
  return 0;
}
