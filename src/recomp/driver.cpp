#include "xenon/recomp/driver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/function_compiler.hpp"
#include "xenon/recomp/native_replacements.hpp"
#include "xenon/recomp/worker_pool.hpp"

namespace xenon::recomp {
namespace {
using cpu::GuestAddress;

std::uint32_t be32(const std::vector<std::byte>& data, std::size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return (std::to_integer<std::uint32_t>(data[offset]) << 24) |
         (std::to_integer<std::uint32_t>(data[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(data[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(data[offset + 3]);
}

std::uint64_t hash_bytes(std::span<const std::byte> bytes, std::uint64_t seed = 1469598103934665603ull) {
  auto hash = seed;
  for (const auto byte : bytes) {
    hash ^= std::to_integer<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t hash_config(const DriverOptions& options) {
  std::uint64_t hash = hash_bytes({});
  for (const auto& hint : options.hints) {
    hash = hash_bytes(std::as_bytes(std::span(hint.name.data(), hint.name.size())), hash);
    for (const auto address : hint.function_boundaries)
      hash = hash_bytes(std::as_bytes(std::span(&address, 1)), hash);
    for (const auto address : hint.data_regions)
      hash = hash_bytes(std::as_bytes(std::span(&address, 1)), hash);
    for (const auto& symbol : hint.known_symbols) {
      hash = hash_bytes(std::as_bytes(std::span(&symbol.address, 1)), hash);
      hash = hash_bytes(std::as_bytes(std::span(symbol.name.data(), symbol.name.size())), hash);
    }
    for (const auto& hook : hint.special_hooks)
      hash = hash_bytes(std::as_bytes(std::span(hook.data(), hook.size())), hash);
    for (const auto& patch : hint.patches)
      hash = hash_bytes(std::as_bytes(std::span(patch.data(), patch.size())), hash);
  }
  return hash;
}

// Text of the xenon::recomp::runtime_helpers:: symbol a given (already
// expanded, i.e. concrete-address) RuntimeHelper dispatches to.
// Register-range kinds are function templates parameterized on the
// concrete register_start (Part 1.5/1.6) - the driver bakes that value in
// as a literal template argument in the generated source text rather than
// emitting one hand-written function per possible register count.
std::string runtime_helper_native_symbol(const analysis::RuntimeHelper& helper) {
  switch (helper.kind) {
    case analysis::RuntimeHelperKind::SetJmp: return "setjmp_v2";
    case analysis::RuntimeHelperKind::LongJmp: return "longjmp_v2";
    case analysis::RuntimeHelperKind::SaveGprLr:
      return "save_gpr_lr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::RestoreGprLr:
      return "restore_gpr_lr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::SaveFpr:
      return "save_fpr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::RestoreFpr:
      return "restore_fpr_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::SaveVmx:
      return "save_vmx_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::RestoreVmx:
      return "restore_vmx_v2<" + std::to_string(helper.register_start.value_or(14u)) + ">";
    case analysis::RuntimeHelperKind::SaveVmx128:
      return "save_vmx128_v2<" + std::to_string(helper.register_start.value_or(64u)) + ">";
    case analysis::RuntimeHelperKind::RestoreVmx128:
      return "restore_vmx128_v2<" + std::to_string(helper.register_start.value_or(64u)) + ">";
  }
  return "setjmp_v2";
}

const xbox::XexSection* executable_section(const ExecutableRangeIndex& index, GuestAddress address) {
  const auto* section = index.containing_section(address);
  return (section != nullptr && section->executable) ? section : nullptr;
}

// Resolves a SwitchTableHint's target addresses (Part 1.6/1.11): explicit
// targets always win when supplied; otherwise decodes `entry_count` entries
// of `entry_format` starting at `table_address` directly from the image's
// bytes, rather than relying solely on the nearby-aligned-word heuristic
// difficult commercial binaries defeat. Skips (with a warning, not silently)
// any decoded entry that does not land in an executable section - a
// malformed/misdescribed hint must be visible, never swallowed.
std::vector<std::uint32_t> resolve_switch_targets(const analysis::SwitchTableHint& table,
                                                   const ExecutableRangeIndex& index,
                                                   std::vector<std::string>& warnings) {
  if (!table.explicit_targets.empty()) return table.explicit_targets;
  std::vector<std::uint32_t> targets;
  if (!table.table_address || !table.entry_count) return targets;
  // Jump tables normally live in a read-only data section rather than the
  // executable one - look up the table's containing section directly
  // (Part 7's indexed lookup already covers both executable and
  // non-executable sections in one query, so no separate fallback scan is
  // needed here the way the old image.sections linear scan required).
  const auto* data_section = index.containing_section(*table.table_address);
  if (!data_section) {
    warnings.push_back("switch table hint at 0x" +
                       [&] { std::ostringstream s; s << std::hex << table.site; return s.str(); }() +
                       ": table_address is not inside any known section");
    return targets;
  }
  const std::size_t entry_size = table.entry_format == analysis::SwitchEntryFormat::RelativeInt16 ? 2u : 4u;
  const auto base_offset =
      static_cast<std::size_t>(*table.table_address - data_section->virtual_address);
  for (std::uint32_t i = 0; i < *table.entry_count; ++i) {
    const auto offset = base_offset + static_cast<std::size_t>(i) * entry_size;
    if (offset + entry_size > data_section->bytes.size()) break;
    std::uint32_t target = 0;
    switch (table.entry_format) {
      case analysis::SwitchEntryFormat::AbsoluteWord32:
        target = be32(data_section->bytes, offset);
        break;
      case analysis::SwitchEntryFormat::RelativeWord32:
        target = *table.table_address +
                 static_cast<std::int32_t>(be32(data_section->bytes, offset));
        break;
      case analysis::SwitchEntryFormat::RelativeInt16: {
        const auto raw = static_cast<std::uint16_t>(
            (std::to_integer<std::uint32_t>(data_section->bytes[offset]) << 8) |
            std::to_integer<std::uint32_t>(data_section->bytes[offset + 1]));
        target = *table.table_address + static_cast<std::int16_t>(raw);
        break;
      }
    }
    if (!index.is_executable_address(target)) {
      warnings.push_back("switch table hint at 0x" +
                         [&] { std::ostringstream s; s << std::hex << table.site; return s.str(); }() +
                         ": decoded entry 0x" +
                         [&] { std::ostringstream s; s << std::hex << target; return s.str(); }() +
                         " is not executable, skipped");
      continue;
    }
    targets.push_back(target);
  }
  return targets;
}

bool source_contains(const DiscoveredFunction& function, DiscoverySource source) {
  return std::find(function.sources.begin(), function.sources.end(), source) != function.sources.end();
}

void add_source(DiscoveredFunction& function, DiscoverySource source) {
  if (!source_contains(function, source)) function.sources.push_back(source);
}

std::string cpp_name(std::uint32_t address) {
  std::ostringstream out;
  out << "xenon_fn_" << std::hex << std::uppercase << address;
  return out.str();
}

std::string sanitize_cpp_name(std::string name, std::uint32_t address) {
  for (auto& character : name)
    if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
      character = '_';
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
    name = "xenon_" + name;
  return name.empty() ? cpp_name(address) : name;
}

bool parse_address_list(const std::string& value, std::vector<std::uint32_t>& output) {
  std::istringstream input(value);
  std::string token;
  while (std::getline(input, token, ',')) {
    try {
      output.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 0)));
    } catch (const std::exception&) {
      return false;
    }
  }
  return true;
}

bool load_hints(const DriverOptions& options, std::vector<ModuleHint>& hints, std::string& error) {
  hints = options.hints;
  if (options.hints_file.empty()) return true;
  std::ifstream input(options.hints_file);
  if (!input) {
    error = "unable to open module hints: " + options.hints_file.string();
    return false;
  }
  ModuleHint hint{};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      error = "invalid module hint line: " + line;
      return false;
    }
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "name") hint.name = value;
    else if (key == "function_boundaries" && !parse_address_list(value, hint.function_boundaries))
      error = "invalid function_boundaries hint";
    else if (key == "data_regions" && !parse_address_list(value, hint.data_regions))
      error = "invalid data_regions hint";
    else if (key == "ignored_regions" && !parse_address_list(value, hint.ignored_regions))
      error = "invalid ignored_regions hint";
    else if (key == "known_symbols") {
      const auto at = value.find('@');
      if (at == std::string::npos) {
        error = "known_symbols must use name@address";
      } else {
        try {
          hint.known_symbols.push_back({static_cast<std::uint32_t>(
              std::stoul(value.substr(at + 1), nullptr, 0)), value.substr(0, at)});
        } catch (const std::exception&) {
          error = "invalid known_symbols hint";
        }
      }
    }
    else if (key == "special_hooks") hint.special_hooks.push_back(value);
    else if (key == "patches") hint.patches.push_back(value);
    else {
      error = "unknown module hint key: " + key;
    }
    if (!error.empty()) return false;
  }
  if (!hint.name.empty() || !hint.function_boundaries.empty() || !hint.known_symbols.empty())
    hints.push_back(std::move(hint));
  return true;
}

bool is_terminal(const cpu::DecodedInstruction& instruction) {
  if (!instruction.info ||
      instruction.info->group != cpu::InstructionGroup::Branch ||
      instruction.lk()) {
    return false;
  }
  if (instruction.mnemonic() == "bx") return true;

  // Conditional branches end a basic block, not necessarily the containing
  // function's primary range. Only stop linear discovery for XL/B-form
  // branches whose BO encoding ignores both CR and CTR (architecturally
  // unconditional). This lets a parent primary range retain its fallthrough
  // block while a declared FunctionChunk supplies the non-contiguous target.
  if (instruction.mnemonic() == "bcx" || instruction.mnemonic() == "bclrx" ||
      instruction.mnemonic() == "bcctrx") {
    return (instruction.bo() & 0x14u) == 0x14u;
  }
  return false;
}

// Part 5 (Recomp Analysis V3): classifies a decode failure using Xenon's own
// opcode catalog rather than collapsing every unrecognized word into one
// generic "invalid-ppc" bucket. A primary (6-bit) opcode with zero cataloged
// entries at all is almost certainly not PPC code (embedded data, padding,
// garbage) - "invalid-ppc". A primary opcode Xenon DOES recognize (real
// cataloged instructions exist under it) whose specific bit pattern still
// doesn't match anything is a genuinely different situation: a real PPC
// instruction family Xenon's decoder does not yet implement this exact
// encoding of - "unsupported-ppc", further narrowed to "unsupported-vmx"
// when that primary opcode's cataloged entries are Vector-group (VMX/
// VMX128's Xbox-specific encoding space is the largest known decoder gap -
// see docs/recomp/RECOMP_ANALYSIS_V3.md). Runs only on the (rare) decode-failure
// path, never in the hot per-instruction loop.
const char* classify_decode_failure(std::uint32_t word) noexcept {
  const auto primary = word >> 26u;
  bool primary_known = false;
  bool primary_is_vector = false;
  for (const auto& opcode : cpu::Decoder::opcode_catalog()) {
    if ((opcode.pattern >> 26u) != primary) continue;
    primary_known = true;
    if (opcode.group == cpu::InstructionGroup::Vector) primary_is_vector = true;
  }
  if (!primary_known) return "invalid-ppc";
  return primary_is_vector ? "unsupported-vmx" : "unsupported-ppc";
}

// Part 4 (Recomp Analysis V3): recognizes a common Xbox 360/PowerPC compiler
// function-prologue opening instruction. Deliberately narrow and
// deliberately never a discovery source on its own - see
// DiscoverySource::PrologueHeuristic's doc comment and
// docs/recomp/RECOMP_ANALYSIS_V3.md's "false-positive prevention" section. Only
// ever consulted for a candidate ALREADY being analyzed via some other
// discovery path (entry/export/unwind/hint/direct call/resolved indirect/
// TLS callback); this never independently scans a data section and
// classifies it as code.
//  - `stwu r1, -N(r1)` / `stwux r1, r1, rB` - grows the stack by adjusting
//    r1 itself, the single most common PPC function-prologue opening
//    instruction.
//  - `mflr r0` (mfspr with SPR 8) - saving the link register, almost always
//    among a non-leaf function's first few instructions.
[[nodiscard]] bool looks_like_function_prologue(const cpu::DecodedInstruction& instruction) noexcept {
  if (!instruction.valid()) return false;
  const auto mnemonic = instruction.mnemonic();
  if ((mnemonic == "stwu" || mnemonic == "stwux") && instruction.rt() == 1u && instruction.ra() == 1u)
    return true;
  if (mnemonic == "mfspr" && instruction.spr() == 8u) return true;
  return false;
}

std::string hash_name(std::uint64_t hash) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::string hex_string(std::uint32_t value) {
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}

// Qualified xenon::recomp::native_replacements function name for a kind that
// native_replacements::entry_for() actually implements - used to emit a
// direct call in the generated lookup_compiled() switch. Must be kept in
// sync with native_replacements.cpp's entry_for(); only ever invoked for a
// kind entry_for() returns non-null for (callers check that first).
const char* native_replacement_function_name(analysis::NativeReplacementKind kind) {
  using analysis::NativeReplacementKind;
  switch (kind) {
    case NativeReplacementKind::Memcpy: return "memcpy_v2";
    case NativeReplacementKind::Memmove: return "memmove_v2";
    case NativeReplacementKind::Memset: return "memset_v2";
    case NativeReplacementKind::MemcpyChecked: return "memcpy_checked_v2";
    case NativeReplacementKind::MemmoveChecked: return "memmove_checked_v2";
    case NativeReplacementKind::Memcmp: return "memcmp_v2";
    case NativeReplacementKind::Strlen: return "strlen_v2";
    case NativeReplacementKind::Strncmp: return "strncmp_v2";
    case NativeReplacementKind::Strncpy: return "strncpy_v2";
    case NativeReplacementKind::Strchr: return "strchr_v2";
    case NativeReplacementKind::Strstr: return "strstr_v2";
    case NativeReplacementKind::Strrchr: return "strrchr_v2";
    case NativeReplacementKind::StrcpyChecked: return "strcpy_checked_v2";
    case NativeReplacementKind::HeapAllocate: return "heap_allocate_v2";
    case NativeReplacementKind::HeapFree: return "heap_free_v2";
    case NativeReplacementKind::HeapSize: return "heap_size_v2";
    case NativeReplacementKind::HeapReAllocate: return "heap_reallocate_v2";
    default: return nullptr;
  }
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  for (const char character : value) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    escaped.push_back(character);
  }
  return escaped;
}

// Immutable, read-only-shared inputs every per-candidate analysis worker
// needs (Part 2/4 of the Recomp Analysis V2 pass). Built once by
// load_and_analyze() before any wave runs; never mutated afterward, so
// sharing a single instance (by const reference) across worker threads is
// safe.
struct AnalysisContext {
  const xbox::XexImage& image;
  const ExecutableRangeIndex& range_index;
  const std::vector<ModuleHint>& hints;
  const analysis::AnalysisHintSetV2* hint_set_v2;  // may be null
  const std::map<GuestAddress, DiscoverySource>& seeds;
  const std::map<GuestAddress, std::string>& known_names;
  const std::vector<analysis::RuntimeHelper>& expanded_runtime_helpers;
  bool allow_partial;
  std::uint64_t configuration_hash;
  // Part 1 (Recomp Analysis V3): every DiscoverySource ever attached to a
  // runtime-discovered (non-seed) candidate address, accumulated by the
  // single calling thread between waves (see load_and_analyze()) and only
  // ever read - never written - by workers during a wave, exactly like
  // `seeds` above. Lets a candidate discovered via, say, both a direct call
  // from one function and a validated tail call from another carry BOTH
  // pieces of evidence once it is finally claimed and analyzed.
  const std::map<GuestAddress, std::vector<DiscoverySource>>& discovered_evidence;
};

bool hinted_non_code(const AnalysisContext& ctx, GuestAddress address) {
  for (const auto& hint : ctx.hints)
    if (std::find(hint.data_regions.begin(), hint.data_regions.end(), address) != hint.data_regions.end() ||
        std::find(hint.ignored_regions.begin(), hint.ignored_regions.end(), address) !=
            hint.ignored_regions.end())
      return true;
  if (ctx.hint_set_v2)
    for (const auto& region : ctx.hint_set_v2->regions)
      if (region.kind != analysis::RegionKind::CodeOverride && address >= region.start &&
          address < region.end)
        return true;
  return false;
}

const analysis::InstructionPatternHint* instruction_pattern_match(const AnalysisContext& ctx,
                                                                   GuestAddress address,
                                                                   std::uint32_t word) {
  if (!ctx.hint_set_v2) return nullptr;
  for (const auto& pattern : ctx.hint_set_v2->instruction_patterns) {
    if ((word & pattern.mask) != (pattern.value & pattern.mask)) continue;
    if (pattern.scope_start && pattern.scope_end &&
        (address < *pattern.scope_start || address >= *pattern.scope_end))
      continue;
    return &pattern;
  }
  return nullptr;
}

// Resolves a raw candidate address to the address that should actually be
// claimed/analyzed (Part 2 Phase D / Part 14): a FunctionChunk's child range
// is owned by its declared parent (unless that exact address is ALSO an
// independent top-level FunctionHint), so any discovery that lands inside
// one redirects to the parent instead of creating a second, unrelated host
// function - exactly the behavior the previous serial algorithm applied
// each time it popped an address off its worklist, just performed here
// before an address is ever claimed for a wave rather than inside the
// per-function analysis itself. Bounded against a cyclic/self-referential
// hint set (Part 5 "avoid infinite discovery loops"); `cyclic` is set true
// if the bound is hit, so the caller can surface it as a visible warning
// rather than silently truncating the redirect.
// Part 2 (generated-code deduplication / shard ownership fix): true iff
// `address` has its own explicit, parentless FunctionHint - i.e. hint data
// itself declares this address an independent top-level function, regardless
// of any FunctionChunk that also happens to claim it as a child range.
// Shared by canonicalize_candidate() below (decides which address a raw
// discovery actually gets analyzed/compiled under) and
// analyze_function_candidate()'s FunctionChunk-ingestion loop (decides which
// chunks a parent may stitch into its own compiled body) so the two
// decisions can never disagree with each other - disagreement between them
// is exactly what used to let the same guest bytes be compiled twice, once
// under the parent's canonical identity and once under the independent
// function's own.
bool has_independent_function_hint(const analysis::AnalysisHintSetV2* hint_set_v2, GuestAddress address) {
  if (!hint_set_v2) return false;
  return std::any_of(hint_set_v2->functions.begin(), hint_set_v2->functions.end(),
                     [address](const auto& hint) {
                       return hint.address == address && !hint.parent_function.has_value();
                     });
}

GuestAddress canonicalize_candidate(GuestAddress start, const analysis::AnalysisHintSetV2* hint_set_v2,
                                    bool& cyclic) {
  cyclic = false;
  if (!hint_set_v2) return start;
  for (int guard = 0; guard < 256; ++guard) {
    const auto owning_chunk =
        std::find_if(hint_set_v2->chunks.begin(), hint_set_v2->chunks.end(),
                     [start](const auto& chunk) { return start >= chunk.start && start < chunk.end; });
    if (owning_chunk == hint_set_v2->chunks.end()) return start;
    if (has_independent_function_hint(hint_set_v2, start)) return start;
    if (owning_chunk->parent_function == start) return start;
    start = owning_chunk->parent_function;
  }
  cyclic = true;
  return start;
}

// Worker-local output of analyzing exactly one candidate address (Part 2's
// FunctionAnalysisResult). Contains no reference to shared state - a wave's
// results are merged into AnalysisReport by the single calling thread only,
// in deterministic address order (see load_and_analyze()).
struct FunctionAnalysisResult {
  enum class Outcome {
    Rejected,  // never becomes a DiscoveredFunction (matches the old algorithm's `continue`)
    Skipped,   // a runtime-helper address; deliberately not analyzed at all
    Accepted,  // becomes exactly one DiscoveredFunction entry (compiled or not)
  };
  Outcome outcome{Outcome::Rejected};
  DiscoveredFunction function;
  // Raw (not yet canonicalized/deduped) new candidates, each tagged with WHY
  // this analysis pass believes it is a function start (Part 1 of the
  // Recomp Analysis V3 pass) - carried through so the eventual candidate's
  // own `sources`/confidence reflect real provenance instead of a generic
  // fallback the moment it is claimed in a later wave.
  std::vector<std::pair<GuestAddress, DiscoverySource>> discovered;
  std::vector<UnresolvedReference> unresolved;
  std::vector<std::string> warnings;
  std::size_t instruction_pattern_matches{};
  std::size_t resolved_indirect_via_dataflow{};    // Part 7: bounded CTR constant propagation hits
  std::size_t resolved_indirect_via_jump_table{};  // Part 8: switch/jump-table recovery hits
};

// Analyzes exactly one already-canonicalized, already-claimed candidate
// address: decode scan, FunctionChunk (Part 14) integration, and static
// compilation. Reads only `ctx` (immutable/shared) and `start`; every output
// is local to the returned result. Safe to call concurrently for different
// `start` values from multiple threads (cpu::Decoder and
// cpu::StaticFunctionCompiler are both stateless - see worker_pool.hpp's
// design note and docs/recomp/RECOMP_ANALYSIS_V2.md's reentrancy audit).
//
// This is a faithful extraction of the previous serial algorithm's per-
// address loop body: the chunk-owner redirect and "already discovered"
// checks that used to sit at the top of that loop are deliberately NOT
// here - both are now handled by the caller before a candidate is ever
// claimed (canonicalize_candidate() and the wave engine's `claimed` set
// respectively), which is what makes calling this function safely
// parallelizable across a whole wave's candidates.
FunctionAnalysisResult analyze_function_candidate(GuestAddress start, const AnalysisContext& ctx) {
  FunctionAnalysisResult result;
  cpu::Decoder decoder;

  // Runtime-helper short-circuit (Part 1.5/1.9): these guest addresses are
  // deliberately executed by Xenon's own generic runtime helper
  // implementations - discovery must not also decode/compile the original
  // guest helper body.
  const auto helper_it =
      std::find_if(ctx.expanded_runtime_helpers.begin(), ctx.expanded_runtime_helpers.end(),
                   [start](const auto& helper) { return helper.address == start; });
  if (helper_it != ctx.expanded_runtime_helpers.end()) {
    result.outcome = FunctionAnalysisResult::Outcome::Skipped;
    return result;
  }

  // Native replacement short-circuit (Part 1.10/1.11).
  if (ctx.hint_set_v2) {
    const auto replacement_it =
        std::find_if(ctx.hint_set_v2->native_replacements.begin(), ctx.hint_set_v2->native_replacements.end(),
                     [start](const auto& r) { return r.guest_address == start; });
    if (replacement_it != ctx.hint_set_v2->native_replacements.end()) {
      if (native_replacements::entry_for(replacement_it->kind) != nullptr) {
        DiscoveredFunction function{};
        function.guest_start = start;
        function.guest_end = start;
        function.name = !replacement_it->source_name.empty()
                            ? sanitize_cpp_name(replacement_it->source_name, start)
                            : cpp_name(start);
        add_source(function, DiscoverySource::ModuleHint);
        function.native_replacement = replacement_it->kind;
        function.compiled = true;
        function.confidence = 100;
        function.ranges = {start, start};
        result.function = std::move(function);
        result.outcome = FunctionAnalysisResult::Outcome::Accepted;
        return result;
      }
      result.warnings.push_back(
          "native replacement '" + replacement_it->source_name + "' (" +
          std::string(analysis::native_replacement_kind_name(replacement_it->kind)) + ") at 0x" +
          [&] { std::ostringstream s; s << std::hex << start; return s.str(); }() +
          " is a recognized identity with no available Xenon implementation yet; analyzing the "
          "guest bytes normally instead");
      // Falls through to normal discovery/compilation below.
    }
  }

  const auto* section = executable_section(ctx.range_index, start);
  if (!section || start < section->virtual_address ||
      static_cast<std::uint64_t>(start - section->virtual_address) + 4 > section->bytes.size()) {
    result.unresolved.push_back({start, start, "function", "function start is outside section bytes"});
    result.outcome = FunctionAnalysisResult::Outcome::Rejected;
    return result;
  }

  DiscoveredFunction function{};
  function.guest_start = start;
  function.name = ctx.known_names.contains(start) ? sanitize_cpp_name(ctx.known_names.at(start), start)
                                                   : cpp_name(start);
  // Part 1 (Recomp Analysis V3): attach every piece of evidence that led
  // here - an original seed's source, plus every DiscoverySource any
  // discoverer(s) tagged this address with across earlier waves. A
  // candidate reached only through decode-time discovery with no evidence
  // recorded at all (should not normally happen - every discovery call site
  // tags a source) falls back to DirectBranch, matching the previous
  // algorithm's behavior for that edge case.
  if (ctx.seeds.contains(start)) add_source(function, ctx.seeds.at(start));
  if (const auto evidence = ctx.discovered_evidence.find(start); evidence != ctx.discovered_evidence.end())
    for (const auto source : evidence->second) add_source(function, source);
  if (function.sources.empty()) add_source(function, DiscoverySource::DirectBranch);
  std::vector<std::uint32_t> words;
  const auto offset = static_cast<std::size_t>(start - section->virtual_address);
  const auto max_words = std::min<std::size_t>((section->bytes.size() - offset) / 4, 4096);
  std::optional<GuestAddress> metadata_end;
  for (const auto& metadata : ctx.image.function_metadata) {
    if (metadata.valid && metadata.begin == start) {
      metadata_end = metadata.end;
      add_source(function, DiscoverySource::UnwindMetadata);
      break;
    }
  }
  for (const auto& hint : ctx.hints)
    for (const auto boundary : hint.function_boundaries)
      if (boundary > start && (!metadata_end || boundary < *metadata_end)) metadata_end = boundary;
  if (ctx.hint_set_v2) {
    // This function's own explicit end/size (Part 1.4) takes precedence
    // over anything inferred from other hints' addresses below.
    for (const auto& function_hint : ctx.hint_set_v2->functions) {
      if (function_hint.address != start) continue;
      if (function_hint.end) metadata_end = *function_hint.end;
      else if (function_hint.size) metadata_end = start + *function_hint.size;
      break;
    }
    const auto consider_boundary = [&](GuestAddress boundary) {
      if (boundary > start && (!metadata_end || boundary < *metadata_end)) metadata_end = boundary;
    };
    for (const auto& function_hint : ctx.hint_set_v2->functions) consider_boundary(function_hint.address);
    for (const auto& chunk : ctx.hint_set_v2->chunks) consider_boundary(chunk.start);
    for (const auto& replacement : ctx.hint_set_v2->native_replacements)
      consider_boundary(replacement.guest_address);
  }
  std::set<GuestAddress> local_boundaries;
  // Tail-call over-discovery fix: a plain (non-linked) direct branch's
  // target is NEVER pushed straight to `result.discovered` at the point the
  // branch is decoded - unlike a direct call, an ordinary branch is
  // routinely just an internal basic-block edge (if/else, loop, shared
  // epilogue), and this function's own final extent (and any FunctionChunk
  // ranges merged into it) is not known until the whole scan finishes. Every
  // candidate branch target is buffered here instead and only promoted to an
  // actual new-function candidate once the full extent is known (see the
  // filtering pass right after `function.ranges` is assembled below).
  // `bool` = was this the function's own terminal (no-fallthrough) branch -
  // only a terminal branch can plausibly be a tail call; a branch that
  // leaves a live fallthrough path is definitionally intra-procedural
  // control flow and must never seed a function on its own.
  std::vector<std::pair<GuestAddress, bool>> pending_direct_branch_targets;
  std::vector<GuestAddress> pending_chunk_branch_targets;
  // Part 7 (Recomp Analysis V3): bounded, straight-line-only constant
  // propagation for resolving `mtctr`/`bctrl`(`bctr`) indirect call/branch
  // targets that a compiler materialized as a locally-constructed 32-bit
  // constant (`lis`/`addis`+`ori` or `addi`, immediately feeding `mtctr`).
  // Deliberately conservative, not general symbolic execution: any
  // instruction outside the small whitelist below invalidates every tracked
  // register (and CTR), so a stale value can never leak across an
  // instruction this analysis does not reason about. See
  // docs/recomp/RECOMP_ANALYSIS_V3.md.
  std::array<std::optional<std::uint32_t>, 32> gpr_constant{};
  std::optional<std::uint32_t> ctr_constant;
  // Part 8 (Recomp Analysis V3): a coarse "was there a bounds check
  // recently" signal for the jump-table-recovery heuristic below - real
  // compiler-generated switch dispatch almost always compares the index
  // against a bound shortly before the indirect branch; an indirect branch
  // with no compare anywhere nearby is weaker evidence that recovered
  // "targets" are a real jump table rather than coincidentally
  // pointer-shaped data.
  std::size_t instructions_since_compare = 1000;
  for (std::size_t index = 0; index < max_words; ++index) {
    const auto address = static_cast<GuestAddress>(start + index * 4u);
    if (index != 0 && hinted_non_code(ctx, address)) {
      result.warnings.push_back("function scan stopped at hinted non-code region 0x" +
                                [&] { std::ostringstream s; s << std::hex << address; return s.str(); }());
      function.guest_end = address;
      break;
    }
    const auto word = be32(section->bytes, offset + index * 4);
    if (index != 0) {
      if (const auto* pattern = instruction_pattern_match(ctx, address, word)) {
        ++result.instruction_pattern_matches;
        result.warnings.push_back(
            "function scan stopped at instruction-pattern match 0x" +
            [&] { std::ostringstream s; s << std::hex << address; return s.str(); }() +
            (pattern->reason.empty() ? "" : " (" + pattern->reason + ")"));
        function.guest_end = address;
        break;
      }
    }
    const auto instruction = decoder.decode(address, word);
    if (!instruction.valid()) {
      const std::string classification = classify_decode_failure(word);
      function.error = (classification == "invalid-ppc" ? "invalid PPC at 0x" : "unsupported PPC encoding at 0x") +
                       [&] { std::ostringstream s; s << std::hex << address; return s.str(); }();
      result.unresolved.push_back({address, word, classification, function.error});
      break;
    }
    words.push_back(instruction.word);
    // Part 4: corroborating evidence only - a recognized prologue opening at
    // this candidate's very first instruction adds PrologueHeuristic to its
    // sources, never creates a new candidate by itself.
    if (index == 0 && looks_like_function_prologue(instruction)) add_source(function, DiscoverySource::PrologueHeuristic);
    if (instructions_since_compare < 1000) ++instructions_since_compare;
    if (instruction.mnemonic().rfind("cmp", 0) == 0) instructions_since_compare = 0;
    if (metadata_end && address + 4u >= *metadata_end) {
      function.guest_end = address + 4u;
      break;
    }
    if (instruction.info->group == cpu::InstructionGroup::Branch) {
      const bool terminal_branch = is_terminal(instruction);
      if (instruction.lk() && (instruction.info->format == cpu::InstructionFormat::I ||
                               instruction.info->format == cpu::InstructionFormat::B)) {
        const auto target = instruction.direct_branch_target();
        function.calls.push_back(target);
        result.discovered.push_back({target, DiscoverySource::DirectCall});
      } else if (!instruction.lk() && (instruction.info->format == cpu::InstructionFormat::I ||
                                        instruction.info->format == cpu::InstructionFormat::B)) {
        const auto target = instruction.direct_branch_target();
        function.branch_references.push_back(target);
        local_boundaries.insert(target);
        pending_direct_branch_targets.push_back({target, terminal_branch});
      } else if (instruction.info->format == cpu::InstructionFormat::XL &&
                !(instruction.mnemonic() == "bclrx" && !instruction.lk())) {
        // Schema V2 known-indirect-site resolution (Part 1.7/1.11): a site
        // the module's own analysis metadata already investigated is
        // reported and treated distinctly from one nothing ever looked at,
        // and any known/decoded targets are seeded exactly like a direct
        // branch/call.
        //
        // An unconditional, non-linked bclrx ("blr") is excluded: it is an
        // ordinary function return, not an indirect branch needing
        // resolution.
        bool resolved_by_hint = false;
        if (ctx.hint_set_v2) {
          if (instruction.lk()) {
            const auto call_it =
                std::find_if(ctx.hint_set_v2->indirect_calls.begin(), ctx.hint_set_v2->indirect_calls.end(),
                             [address](const auto& c) { return c.callsite == address; });
            if (call_it != ctx.hint_set_v2->indirect_calls.end()) {
              resolved_by_hint = true;
              for (const auto target : call_it->targets) {
                function.calls.push_back(target);
                result.discovered.push_back({target, DiscoverySource::ModuleHint});
              }
              if (call_it->targets.empty()) {
                result.unresolved.push_back(
                    {address, 0, "indirect-call-acknowledged",
                     "site flagged as indirect by module analysis metadata; no static targets known"});
              }
            }
          } else {
            const auto branch_it =
                std::find_if(ctx.hint_set_v2->indirect_branches.begin(), ctx.hint_set_v2->indirect_branches.end(),
                             [address](const auto& b) { return b.site == address; });
            const auto table_it =
                std::find_if(ctx.hint_set_v2->switches.begin(), ctx.hint_set_v2->switches.end(),
                             [address](const auto& t) { return t.site == address; });
            std::vector<GuestAddress> known_targets;
            if (branch_it != ctx.hint_set_v2->indirect_branches.end()) known_targets = branch_it->targets;
            else if (table_it != ctx.hint_set_v2->switches.end())
              known_targets = resolve_switch_targets(*table_it, ctx.range_index, result.warnings);
            if (branch_it != ctx.hint_set_v2->indirect_branches.end() ||
                table_it != ctx.hint_set_v2->switches.end()) {
              resolved_by_hint = true;
              for (const auto target : known_targets) {
                function.branch_references.push_back(target);
                local_boundaries.insert(target);
                result.discovered.push_back({target, DiscoverySource::ModuleHint});
              }
              if (known_targets.empty()) {
                result.unresolved.push_back(
                    {address, 0, "indirect-branch-acknowledged",
                     "site flagged as indirect by module analysis metadata; no resolvable targets"});
              }
            }
          }
        }
        if (!resolved_by_hint) {
          // Part 7 (Recomp Analysis V3): a compiler-materialized 32-bit
          // constant fed directly into CTR just before this site, resolved
          // by the bounded local dataflow tracker above. Only ever trusted
          // when the resolved address independently passes the same
          // alignment/executability validation every other candidate does -
          // never fabricated, never applied to bclrx (LR-based - not
          // tracked by this dataflow).
          bool resolved_by_dataflow = false;
          if (instruction.mnemonic() == "bcctrx" && ctr_constant &&
              ExecutableRangeIndex::is_aligned_ppc_address(*ctr_constant) &&
              ctx.range_index.is_executable_address(*ctr_constant)) {
            resolved_by_dataflow = true;
            ++result.resolved_indirect_via_dataflow;
            if (instruction.lk()) {
              function.calls.push_back(*ctr_constant);
            } else {
              function.branch_references.push_back(*ctr_constant);
              local_boundaries.insert(*ctr_constant);
            }
            result.discovered.push_back({*ctr_constant, DiscoverySource::ResolvedIndirect});
          }
          if (!resolved_by_dataflow) {
            result.unresolved.push_back({address, 0, instruction.lk() ? "indirect-call" : "indirect-branch",
                                         "target depends on runtime register state"});
            // Part 8 (Recomp Analysis V3): only attempt jump-table recovery
            // when a bounds check appeared recently - real compiler switch
            // dispatch compares the index against a bound shortly before
            // the indirect branch; without that corroborating evidence
            // nearby, recovered "targets" are indistinguishable from
            // coincidentally pointer-shaped data, so none are reported
            // (stricter than treating every unresolved indirect branch as a
            // possible table - Part 8's "reject arbitrary data that merely
            // resembles pointers").
            if (!instruction.lk() && instructions_since_compare <= 8u) {
              const auto table_start = offset + index * 4u + 4u;
              const auto table_end = std::min(section->bytes.size(), table_start + 256u);
              for (auto table = table_start; table + 4u <= table_end; table += 4u) {
                const auto target = be32(section->bytes, table);
                if (executable_section(ctx.range_index, target)) {
                  function.branch_references.push_back(target);
                  result.discovered.push_back({target, DiscoverySource::ResolvedIndirect});
                  ++result.resolved_indirect_via_jump_table;
                  result.warnings.push_back(
                      "recovered possible jump-table target 0x" +
                      [&] { std::ostringstream s; s << std::hex << target; return s.str(); }());
                }
              }
            }
          }
        }
      }
      if (terminal_branch) { function.guest_end = address + 4; break; }
    }
    // Part 7: update the bounded constant tracker for the NEXT instruction,
    // using this one. Deliberately runs AFTER this instruction's own branch
    // handling above (which reads gpr_constant/ctr_constant as left by
    // every PRIOR instruction) rather than before it - an indirect branch
    // instruction itself is not addi/ori/mtspr, so processing it through
    // this tracker first would immediately reset the very ctr_constant its
    // own resolution needs to read.
    {
      const auto mnemonic = instruction.mnemonic();
      if (mnemonic == "addi" || mnemonic == "addis") {
        const auto source = instruction.ra() == 0u ? std::optional<std::uint32_t>(0u)
                                                    : gpr_constant[instruction.ra()];
        if (source) {
          const auto delta = mnemonic == "addis"
                                  ? (static_cast<std::uint32_t>(instruction.uimm16()) << 16)
                                  : static_cast<std::uint32_t>(static_cast<std::int32_t>(instruction.simm16()));
          gpr_constant[instruction.rt()] = *source + delta;
        } else {
          gpr_constant[instruction.rt()] = std::nullopt;
        }
      } else if (mnemonic == "ori" || mnemonic == "oris") {
        // Note: for ori/oris the SOURCE register is the rt()/rs() field and
        // the DESTINATION is ra() - the reverse of addi/addis. Matches
        // src/cpu/ppc/lifter_integer.cpp's own field usage exactly.
        const auto source = gpr_constant[instruction.rs()];
        if (source) {
          const auto bits = mnemonic == "oris" ? (static_cast<std::uint32_t>(instruction.uimm16()) << 16)
                                                : static_cast<std::uint32_t>(instruction.uimm16());
          gpr_constant[instruction.ra()] = *source | bits;
        } else {
          gpr_constant[instruction.ra()] = std::nullopt;
        }
      } else if (mnemonic == "mtspr") {
        // mtspr never writes a GPR, so gpr_constant tracking is unaffected
        // either way; only SPR 9 (CTR) feeding a call/branch matters here.
        if (instruction.spr() == 9u) ctr_constant = gpr_constant[instruction.rs()];
      } else {
        gpr_constant.fill(std::nullopt);
        ctr_constant.reset();
      }
    }
    function.guest_end = address + 4;
  }

  // Read every discontinuous FunctionChunk owned by this parent (Part 14)
  // and make it part of the same compiler input.
  std::vector<std::vector<std::uint32_t>> chunk_words;
  std::vector<std::pair<GuestAddress, GuestAddress>> child_ranges;
  if (ctx.hint_set_v2) {
    const auto owned_count = static_cast<std::size_t>(
        std::count_if(ctx.hint_set_v2->chunks.begin(), ctx.hint_set_v2->chunks.end(),
                      [start](const auto& chunk) { return chunk.parent_function == start; }));
    chunk_words.reserve(owned_count);
    child_ranges.reserve(owned_count);
    for (const auto& chunk : ctx.hint_set_v2->chunks) {
      if (chunk.parent_function != start) continue;
      // Part 6/7 (generated-code deduplication / shard ownership fix): a
      // chunk address that ALSO has its own independent top-level
      // FunctionHint (canonicalize_candidate() deliberately lets that hint
      // win over the chunk redirect - see has_independent_function_hint()'s
      // doc comment) must never ALSO be stitched into this parent's
      // compiled body. Without this guard the exact same guest bytes get
      // compiled twice under two different canonical identities - this
      // parent's name, and the independent function's own name - a real
      // violation of the one-address/one-canonical-function invariant, even
      // though the two resulting C++ symbols differ (so it never showed up
      // as a duplicate-body compile error the way the codegen cache-key
      // collision below did). Excluded, not merged: explicit hint data
      // marking an address independent is authoritative over an unrelated
      // chunk declaration that happens to also claim it.
      if (has_independent_function_hint(ctx.hint_set_v2, chunk.start)) {
        result.warnings.push_back(
            "FunctionChunk at 0x" + hex_string(chunk.start) + " declares parent 0x" + hex_string(start) +
            " but 0x" + hex_string(chunk.start) +
            " also has its own independent FunctionHint; excluded from the parent's merged body so "
            "each guest address compiles under exactly one canonical function");
        continue;
      }
      const auto* chunk_section = executable_section(ctx.range_index, chunk.start);
      const auto chunk_size = static_cast<std::uint64_t>(chunk.end) - chunk.start;
      if (!chunk_section || chunk.end <= chunk.start || (chunk.start & 3u) != 0u ||
          (chunk.end & 3u) != 0u ||
          static_cast<std::uint64_t>(chunk.start - chunk_section->virtual_address) + chunk_size >
              chunk_section->bytes.size()) {
        function.error = "FunctionChunk range is outside executable section bytes";
        result.unresolved.push_back({chunk.start, chunk.end, "function-chunk", function.error});
        break;
      }

      auto& storage = chunk_words.emplace_back();
      storage.reserve(static_cast<std::size_t>(chunk_size / 4u));
      const auto chunk_offset = static_cast<std::size_t>(chunk.start - chunk_section->virtual_address);
      auto chunk_end_used = chunk.end;
      for (GuestAddress address = chunk.start; address < chunk.end; address += 4u) {
        const auto word = be32(chunk_section->bytes, chunk_offset + (address - chunk.start));
        if (address != chunk.start) {
          if (const auto* pattern = instruction_pattern_match(ctx, address, word)) {
            ++result.instruction_pattern_matches;
            result.warnings.push_back(
                "FunctionChunk scan stopped at instruction-pattern match 0x" +
                [&] { std::ostringstream s; s << std::hex << address; return s.str(); }() +
                (pattern->reason.empty() ? "" : " (" + pattern->reason + ")"));
            chunk_end_used = address;
            break;
          }
        }
        const auto instruction = decoder.decode(address, word);
        if (!instruction.valid()) {
          const std::string classification = classify_decode_failure(word);
          function.error = (classification == "invalid-ppc" ? "invalid PPC in FunctionChunk at 0x"
                                                              : "unsupported PPC encoding in FunctionChunk at 0x") +
              [&] { std::ostringstream text; text << std::hex << address; return text.str(); }();
          result.unresolved.push_back({address, word, classification, function.error});
          break;
        }
        storage.push_back(word);
        if (instruction.info->group == cpu::InstructionGroup::Branch &&
            (instruction.info->format == cpu::InstructionFormat::I ||
             instruction.info->format == cpu::InstructionFormat::B)) {
          const auto target = instruction.direct_branch_target();
          if (instruction.lk()) {
            function.calls.push_back(target);
            result.discovered.push_back({target, DiscoverySource::DirectCall});
          } else {
            // Same over-discovery fix as the primary scan above: buffered,
            // filtered once this function's complete extent (primary range +
            // every chunk) is known, not pushed unconditionally here.
            function.branch_references.push_back(target);
            pending_chunk_branch_targets.push_back(target);
          }
        }
      }
      if (!function.error.empty()) break;
      child_ranges.emplace_back(chunk.start, chunk_end_used);
    }
  }

  function.ranges = {function.guest_start, function.guest_end};
  for (const auto& [range_start, range_end] : child_ranges) {
    function.ranges.push_back(range_start);
    function.ranges.push_back(range_end);
  }

  // Tail-call over-discovery fix, promotion pass: this function's complete
  // extent (primary range + every FunctionChunk merged into it) is now
  // known, so every direct-branch target buffered above can finally be
  // judged against it instead of being trusted unconditionally the moment
  // it was decoded.
  const auto inside_own_extent = [&](GuestAddress target) {
    for (std::size_t i = 0; i + 1u < function.ranges.size(); i += 2u)
      if (target >= function.ranges[i] && target < function.ranges[i + 1u]) return true;
    return false;
  };
  // Part 3 (recursive-explosion guard): a candidate whose own identity rests
  // solely on a plain direct branch - no seed evidence (entry/export/module-
  // hint/unwind/TLS callback), no direct call, no resolved-indirect proof,
  // and no independently recognized compiler prologue at its own first
  // instruction - does not get full "discovery authority": its own direct-
  // branch targets are still recorded (`branch_references`, so an unclaimed
  // one is still reported via the existing branch-into-unknown-code
  // diagnostic below) but are not themselves promoted to new candidates.
  // This stops a chain of unconfirmed heuristic guesses from recursively
  // manufacturing unbounded numbers of functions, while any candidate with
  // real corroborating evidence (including a matched prologue) keeps full
  // authority to seed further discovery exactly as before.
  const bool self_confirmed =
      std::any_of(function.sources.begin(), function.sources.end(),
                 [](DiscoverySource source) { return source != DiscoverySource::DirectBranch; });
  for (const auto& [target, terminal] : pending_direct_branch_targets) {
    // Only a terminal (architecturally no-fallthrough) branch can plausibly
    // be a tail call; a branch with a live fallthrough path is ordinary
    // intra-procedural control flow by construction (item 2's "control flow
    // clearly terminates at branch" / "no valid fallthrough" requirement).
    if (!terminal) continue;
    // A target already covered by this function's own established extent
    // (including its chunks) is an internal basic block / loop back-edge /
    // shared-epilogue jump within the same compiled unit, never an
    // independent function (item 1's core requirement).
    if (inside_own_extent(target)) continue;
    if (!self_confirmed) continue;
    result.discovered.push_back({target, DiscoverySource::DirectBranch});
  }
  for (const auto target : pending_chunk_branch_targets) {
    if (inside_own_extent(target)) continue;
    if (!self_confirmed) continue;
    result.discovered.push_back({target, DiscoverySource::DirectBranch});
  }

  if (words.empty() || !function.error.empty()) {
    if (!ctx.allow_partial) {
      result.outcome = FunctionAnalysisResult::Outcome::Rejected;
      return result;
    }
    // allow_partial: fall through and still accept this (uncompiled)
    // function, matching the previous algorithm exactly.
  } else {
    cpu::StaticFunctionCompiler compiler;
    std::vector<cpu::FunctionCodeRange> compile_ranges;
    compile_ranges.reserve(1u + child_ranges.size());
    compile_ranges.push_back({start, words});
    for (std::size_t i = 0; i < child_ranges.size(); ++i)
      compile_ranges.push_back({child_ranges[i].first, chunk_words[i]});

    const auto compile_result = compiler.compile_ranges(start, compile_ranges);
    if (!compile_result.ok) {
      // NOTE: a compile failure (as opposed to a decode failure above) is
      // always recorded as an uncompiled DiscoveredFunction entry
      // regardless of allow_partial - matches the previous algorithm, which
      // only gated the decode-failure/empty-words case on allow_partial.
      function.error = compile_result.error;
      result.unresolved.push_back(
          {compile_result.error_address, compile_result.error_word, "compile", compile_result.error});
    } else {
      function.ir = compile_result.function;
      function.compiled = true;
      // Part 11 (Recomp Analysis V3): confidence is evidence-based
      // (confidence_for_sources()), not the single arbitrary
      // internal-branch-boundary check this used to be alone. That signal
      // is kept as a secondary modifier - a function whose own scan crossed
      // an address that was ALSO independently discovered as a branch
      // target suggests possible boundary ambiguity worth a small penalty,
      // regardless of how strong the function's own provenance is.
      function.confidence = confidence_for_sources(function.sources);
      if (!local_boundaries.empty())
        function.confidence = function.confidence > 10u ? function.confidence - 10u : 0u;
      auto source_hash = hash_bytes(std::as_bytes(std::span(words)), ctx.configuration_hash);
      for (std::size_t i = 0; i < child_ranges.size(); ++i) {
        source_hash = hash_bytes(std::as_bytes(std::span(&child_ranges[i].first, 1)), source_hash);
        source_hash = hash_bytes(std::as_bytes(std::span(chunk_words[i])), source_hash);
      }
      function.source_hash = source_hash;
    }
  }

  result.function = std::move(function);
  result.outcome = FunctionAnalysisResult::Outcome::Accepted;
  return result;
}

// Part 2 (generated-code deduplication / shard ownership fix): enforces "ONE
// guest callable function address -> ONE canonical DiscoveredFunction
// record" directly on `functions`, as a safety net against any discovery
// path that manages to produce two entries sharing a guest_start. The wave
// engine's `claimed` set (see load_and_analyze()'s wave loop) already
// prevents this for every address-claiming path, and the FunctionChunk/
// independent-FunctionHint mutual-exclusion fix above closes the one path
// that could produce the same bytes under two canonical identities without
// literally duplicating a guest_start - so this should never actually merge
// anything in production. It exists anyway because Part 9's contract is "do
// not silently discard conflicting function definitions" for ANY future
// regression in this area, not just the ones already understood, and because
// downstream passes (registry/codegen emission) must never have to
// separately re-derive this invariant to stay correct. Requires `functions`
// to already be sorted by guest_start. Duplicate entries are merged, not
// dropped: provenance (`sources`) is unioned onto the kept record, and a
// compiled record is always preferred over an uncompiled one.
std::size_t deduplicate_functions_by_address(std::vector<DiscoveredFunction>& functions,
                                             std::vector<std::string>& warnings) {
  std::size_t merged = 0;
  std::vector<DiscoveredFunction> unique_functions;
  unique_functions.reserve(functions.size());
  for (auto& function : functions) {
    if (!unique_functions.empty() && unique_functions.back().guest_start == function.guest_start) {
      auto& kept = unique_functions.back();
      warnings.push_back("duplicate canonical function record for guest address 0x" +
                         hex_string(function.guest_start) + " ('" + kept.name + "' vs '" + function.name +
                         "') merged into one canonical record");
      for (const auto source : function.sources) add_source(kept, source);
      if (!kept.compiled && function.compiled) kept = std::move(function);
      ++merged;
      continue;
    }
    unique_functions.push_back(std::move(function));
  }
  functions = std::move(unique_functions);
  return merged;
}

// Part 2/3/9 (generated-code deduplication / shard ownership fix):
// pre-emission validation. The hard invariant this whole pass exists to
// enforce is ONE guest address -> ONE canonical function -> ONE emitted C++
// definition. `codegen_items` is already guaranteed address-unique by this
// point (report.functions was deduplicated by guest_start in
// load_and_analyze() - see deduplicate_functions_by_address() - well before
// generate_project() ever runs), so the address-collision branch below is a
// second, independent check of the same invariant right before it actually
// matters (belt-and-suspenders, per Part 9's "do not silently discard
// conflicting function definitions"). The symbol-collision branch catches
// the one class of conflict address-uniqueness alone can't: two DIFFERENT
// addresses whose generated names collide (a hint-file authoring bug, since
// Xenon derives every other name deterministically from its own address).
// Per Part 13, a genuine name collision must fail codegen loudly - never be
// silently renamed, suppressed, or worked around - so a real conflict here
// is reported and generate_project() returns false before any C++ is
// written for this run.
bool validate_codegen_uniqueness(const std::vector<const DiscoveredFunction*>& codegen_items,
                                 AnalysisDiagnostics& diagnostics, std::string& error) {
  std::unordered_map<GuestAddress, const DiscoveredFunction*> by_address;
  std::unordered_map<std::string, const DiscoveredFunction*> by_symbol;
  by_address.reserve(codegen_items.size());
  by_symbol.reserve(codegen_items.size() * 3u);
  for (const auto* function : codegen_items) {
    if (const auto existing = by_address.find(function->guest_start); existing != by_address.end()) {
      error = "codegen: guest address 0x" + hex_string(function->guest_start) +
              " has more than one canonical function record ('" + existing->second->name + "' and '" +
              function->name +
              "') reaching code generation - this indicates a bug earlier in the analysis pipeline, not a "
              "legitimate alias";
      return false;
    }
    by_address.emplace(function->guest_start, function);
    const std::string symbols[] = {function->name, function->name + "_v2", function->name + "_dispatch_v2"};
    for (const auto& symbol : symbols) {
      if (const auto existing = by_symbol.find(symbol); existing != by_symbol.end()) {
        ++diagnostics.codegen_duplicate_symbols_rejected;
        error = "codegen: generated symbol '" + symbol + "' would be emitted for both guest address 0x" +
                hex_string(existing->second->guest_start) + " (first owner, name '" + existing->second->name +
                "') and 0x" + hex_string(function->guest_start) + " (second attempted owner, name '" +
                function->name +
                "') - refusing to emit conflicting C++ definitions under one symbol name; check the "
                "module hint data for two different addresses assigned the same name";
        return false;
      }
      by_symbol.emplace(symbol, function);
    }
  }
  diagnostics.codegen_input_functions = codegen_items.size();
  diagnostics.codegen_unique_functions = by_address.size();
  return true;
}

}  // namespace

ExecutableRangeIndex::ExecutableRangeIndex(const xbox::XexImage& image) {
  entries_.reserve(image.sections.size());
  for (const auto& section : image.sections) {
    const auto size = std::max(section.virtual_size, section.raw_size);
    if (size == 0) continue;
    const auto begin = section.virtual_address;
    const auto end = std::min<std::uint64_t>(static_cast<std::uint64_t>(begin) + size, 0xFFFFFFFFull);
    entries_.push_back(Entry{begin, static_cast<std::uint32_t>(end), &section});
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) { return a.begin < b.begin; });
}

const xbox::XexSection* ExecutableRangeIndex::containing_section(std::uint32_t address) const noexcept {
  // Binary search for the last entry whose `begin` is <= address, then a
  // direct range check against its `end` - O(log section_count) instead of
  // the previous O(section_count) linear scan (Part 7). Real XEX images have
  // few, non-overlapping sections, so this matters less for correctness than
  // for call volume: this lookup runs many times per candidate address
  // across a 10k-function analysis.
  auto it = std::upper_bound(entries_.begin(), entries_.end(), address,
                             [](std::uint32_t value, const Entry& entry) { return value < entry.begin; });
  if (it == entries_.begin()) return nullptr;
  --it;
  return (address >= it->begin && address < it->end) ? it->section : nullptr;
}

bool ExecutableRangeIndex::is_executable_address(std::uint32_t address) const noexcept {
  const auto* section = containing_section(address);
  return section != nullptr && section->executable;
}

bool ExecutableRangeIndex::is_mapped_address(std::uint32_t address) const noexcept {
  return containing_section(address) != nullptr;
}

bool ExecutableRangeIndex::is_aligned_ppc_address(std::uint32_t address) noexcept {
  return (address & 3u) == 0u;
}

void ModuleCatalog::register_provider(const ModuleHintProvider& provider) {
  providers_.push_back(&provider);
}

const char* discovery_source_name(DiscoverySource source) noexcept {
  switch (source) {
    case DiscoverySource::EntryPoint: return "entry";
    case DiscoverySource::Export: return "export";
    case DiscoverySource::DirectCall: return "direct-call";
    case DiscoverySource::DirectBranch: return "direct-branch";
    case DiscoverySource::UnwindMetadata: return "unwind-metadata";
    case DiscoverySource::ModuleHint: return "module-hint";
    case DiscoverySource::TlsCallback: return "tls-callback";
    case DiscoverySource::ResolvedIndirect: return "resolved-indirect";
    case DiscoverySource::ValidatedTailCall: return "validated-tail-call";
    case DiscoverySource::PrologueHeuristic: return "prologue-heuristic";
  }
  return "unknown";
}

std::uint32_t discovery_source_base_confidence(DiscoverySource source) noexcept {
  switch (source) {
    case DiscoverySource::EntryPoint: return 100;
    case DiscoverySource::ModuleHint: return 100;
    case DiscoverySource::UnwindMetadata: return 95;
    case DiscoverySource::TlsCallback: return 95;
    case DiscoverySource::Export: return 90;
    case DiscoverySource::DirectCall: return 85;
    case DiscoverySource::ResolvedIndirect: return 80;
    case DiscoverySource::ValidatedTailCall: return 65;
    case DiscoverySource::DirectBranch: return 60;
    case DiscoverySource::PrologueHeuristic: return 40;
  }
  return 50;
}

std::uint32_t confidence_for_sources(const std::vector<DiscoverySource>& sources) noexcept {
  if (sources.empty()) return 50;
  std::uint32_t best = 0;
  for (const auto source : sources) best = std::max(best, discovery_source_base_confidence(source));
  // Corroboration bonus (Part 11): two or more independent pieces of
  // evidence agreeing on the same start address are stronger together than
  // either alone - e.g. a direct call target that is ALSO the target of a
  // validated tail call from elsewhere.
  if (sources.size() >= 2) best = std::min<std::uint32_t>(100, best + 5);
  return best;
}

bool load_and_analyze(const DriverOptions& options, AnalysisReport& report, std::string& error) {
  const auto analysis_start = std::chrono::steady_clock::now();
  if (options.progress) options.progress("[Xenon Recomp] Loading executable...");
  std::vector<ModuleHint> hints;
  if (!load_hints(options, hints, error)) return false;
  DriverOptions effective = options;
  effective.hints = std::move(hints);
  if (options.pre_parsed_image.has_value()) {
    report.image = *options.pre_parsed_image;
  } else {
    std::ifstream file(options.input, std::ios::binary);
    if (!file) { error = "unable to open input XEX: " + options.input.string(); return false; }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    if (!xbox::parse_xex_image(bytes, report.image, &error)) return false;
  }
  for (const auto* provider : effective.module_providers) {
    if (provider == nullptr) continue;
    if (!provider->provide(report.image, effective.hints, error)) return false;
  }
  report.configuration_hash = hash_config(effective);

  // Analysis Hint Schema V2 (Part 1/2): resolve, validate, and scope-check
  // before anything below consumes it. hint_provider_v2 (the production
  // path - see ModuleHintProviderV2) takes precedence over a directly
  // supplied hint_set_v2 (tests/manual invocation); a provider that cannot
  // supply a hint set for this exact executable revision is a hard failure,
  // never a silent "run without hints" or "use the nearest-looking
  // revision" (Part 2.5).
  const auto effective_identity = xbox::compute_effective_identity(report.image);
  if (options.hint_provider_v2) {
    analysis::AnalysisHintSetV2 resolved{};
    if (!options.hint_provider_v2->provide(effective_identity, resolved, error)) return false;
    report.hint_set_v2 = std::move(resolved);
  } else if (options.hint_set_v2) {
    report.hint_set_v2 = options.hint_set_v2;
  }
  if (report.hint_set_v2) {
    std::vector<std::string> schema_errors;
    if (!analysis::validate_hint_set(*report.hint_set_v2, schema_errors)) {
      error = "analysis hint set failed schema validation:";
      for (const auto& schema_error : schema_errors) error += " " + schema_error + ";";
      return false;
    }
    if (!analysis::hint_set_matches_identity(*report.hint_set_v2, effective_identity)) {
      error =
          "analysis hint set is scoped to a different executable revision than the one being "
          "analyzed (module '" +
          report.hint_set_v2->module_name + "')";
      return false;
    }
  }
  const auto& hint_set_v2 = report.hint_set_v2;

  // Phase B (Part 2): immutable executable-memory description, built once
  // and shared read-only by every analysis worker below.
  const ExecutableRangeIndex range_index(report.image);

  std::map<GuestAddress, DiscoverySource> seeds;
  seeds[report.image.entry_point] = DiscoverySource::EntryPoint;
  for (const auto& export_entry : report.image.exports)
    seeds[export_entry.address] = DiscoverySource::Export;
  for (const auto& hint : effective.hints)
    for (const auto address : hint.function_boundaries)
      seeds[address] = DiscoverySource::ModuleHint;
  std::map<GuestAddress, std::string> known_names;
  for (const auto& hint : effective.hints)
    for (const auto& symbol : hint.known_symbols) {
      seeds[symbol.address] = DiscoverySource::ModuleHint;
      known_names[symbol.address] = symbol.name;
    }
  for (const auto& hint : effective.hints)
    for (const auto address : hint.data_regions)
      seeds.erase(address);
  for (const auto& hint : effective.hints)
    for (const auto address : hint.ignored_regions)
      seeds.erase(address);
  for (const auto& metadata : report.image.function_metadata)
    if (metadata.valid) seeds[metadata.begin] = DiscoverySource::UnwindMetadata;
  // Part 2 (Recomp Analysis V3): a XEX TLS directory callback is a real,
  // generic loader-exposed callable address - the loader itself invokes it
  // on thread attach/detach, exactly as reachable as the entry point, just
  // never reached via any direct call/branch a decode-time scan would ever
  // find. No title ever needs a hint to tell Xenon this address is code.
  if (report.image.tls && report.image.tls->callback_address != 0)
    seeds[report.image.tls->callback_address] = DiscoverySource::TlsCallback;

  // Schema V2 seeding (Part 1.11: the driver actually uses this metadata
  // during function discovery, not merely parses/stores it).
  if (hint_set_v2) {
    for (const auto& function_hint : hint_set_v2->functions) {
      seeds[function_hint.address] = DiscoverySource::ModuleHint;
      if (!function_hint.name.empty()) known_names[function_hint.address] = function_hint.name;
    }
    // FunctionChunk ranges are owned by their declared parent and are compiled
    // with that parent below. They are intentionally not seeded as unrelated
    // top-level functions; explicit independent FunctionHint entries still
    // seed normally.
    // Known indirect call/branch targets and explicit/decoded switch targets
    // are all real, reachable code entry points - seed them exactly like any
    // other module hint so they get discovered/compiled even though nothing
    // in the image contains a direct `bl`/branch to them.
    for (const auto& call : hint_set_v2->indirect_calls)
      for (const auto target : call.targets) seeds[target] = DiscoverySource::ModuleHint;
    for (const auto& branch : hint_set_v2->indirect_branches)
      for (const auto target : branch.targets) seeds[target] = DiscoverySource::ModuleHint;
    for (const auto& table : hint_set_v2->switches)
      for (const auto target : resolve_switch_targets(table, range_index, report.warnings))
        seeds[target] = DiscoverySource::ModuleHint;
    // Native replacement addresses are real, dispatchable entry points too
    // (Part 1.10/1.11) - seeded so a `bl` to one is discoverable even when
    // the hint set never separately lists it as a FunctionHint.
    for (const auto& replacement : hint_set_v2->native_replacements)
      seeds[replacement.guest_address] = DiscoverySource::ModuleHint;
    // Data/ignored/invalid-instruction regions (Part 1.8) must never seed a
    // function, regardless of source - a range-based erase since V2 regions
    // are ranges, not single addresses.
    for (const auto& region : hint_set_v2->regions) {
      if (region.kind == analysis::RegionKind::CodeOverride) continue;
      for (auto it = seeds.begin(); it != seeds.end();) {
        if (it->first >= region.start && it->first < region.end) it = seeds.erase(it);
        else ++it;
      }
    }
  }

  // Register-range RuntimeHelperKinds (Part 1.5) declare only their family's
  // lowest variant address in the raw hint list; expand once up front so
  // every actually-callable variant address is available for both the
  // discovery short-circuit below and the diagnostics counters.
  const std::vector<analysis::RuntimeHelper> expanded_runtime_helpers =
      hint_set_v2 ? analysis::expand_runtime_helpers(hint_set_v2->runtime_helpers)
                  : std::vector<analysis::RuntimeHelper>{};

  if (effective.progress) effective.progress("[Xenon Recomp] Collecting function candidates...");

  // Phase D/E (Part 2): validate the initial seed set and freeze it as the
  // first discovery-wave frontier. A seed outside any executable section is
  // rejected right here (never reaches per-candidate analysis at all) -
  // matches the previous serial algorithm's pre-loop filter exactly.
  std::size_t rejected_nonexec = 0;
  std::vector<GuestAddress> frontier;
  frontier.reserve(seeds.size());
  for (const auto& [address, source] : seeds) {
    if (range_index.is_executable_address(address)) {
      frontier.push_back(address);
    } else {
      report.unresolved.push_back({address, address, "seed", "target is not in an executable section"});
      ++rejected_nonexec;
    }
  }
  if (effective.progress)
    effective.progress("[Xenon Recomp] Candidate functions: " + std::to_string(frontier.size()));

  // Part 1 (Recomp Analysis V3): accumulates every DiscoverySource ever
  // attached to a runtime-discovered candidate address, across all waves.
  // Mutated only by this (single) calling thread, strictly between waves -
  // never while a wave's WorkerPool::parallel_for() call is in flight - so
  // sharing it into AnalysisContext by const reference is safe (matches
  // `seeds`/`known_names`'s existing contract exactly).
  std::map<GuestAddress, std::vector<DiscoverySource>> discovered_evidence;

  const AnalysisContext ctx{report.image,
                            range_index,
                            effective.hints,
                            hint_set_v2 ? &*hint_set_v2 : nullptr,
                            seeds,
                            known_names,
                            expanded_runtime_helpers,
                            effective.allow_partial,
                            report.configuration_hash,
                            discovered_evidence};

  // Phase F (Part 2/3/4): parallel per-function analysis, driven in
  // deterministic discovery waves (Part 5) so that compiling one function
  // (which can discover new call/branch targets) never requires a worker to
  // mutate the canonical function registry directly - each wave's workers
  // return purely local results (FunctionAnalysisResult), and only the
  // single calling thread ever merges them into `report`, in a fixed
  // address-sorted order independent of which worker finished which item
  // first. jobs=1 and jobs=N run through this exact same code path (no
  // separate serial algorithm to keep in sync), so their output is
  // byte-identical by construction, not by careful parallel bug-for-bug
  // matching of two implementations.
  const auto worker_count = resolve_worker_count(effective.analysis_jobs);
  if (effective.progress)
    effective.progress("[Xenon Recomp] Analysis workers: " + std::to_string(worker_count));
  // WorkerPool itself degrades to pure inline execution (zero threading
  // overhead) when worker_count == 1, so constructing and using it
  // unconditionally - rather than branching around it for the jobs=1 case -
  // is both simpler and already optimal.
  const WorkerPool pool(worker_count);

  std::set<GuestAddress> claimed;  // every address ever handed to per-candidate analysis (Part 5/22:
                                   // no global function registry is ever mutated by a worker - this
                                   // set, and `report` itself, are only ever touched by this thread,
                                   // between waves)
  std::size_t functions_analyzed = 0;
  std::size_t instruction_pattern_match_count = 0;
  std::size_t wave_count = 0;
  std::size_t rejected_unaligned = 0;
  std::size_t resolved_indirect_via_dataflow = 0;
  std::size_t resolved_indirect_via_jump_table = 0;

  while (!frontier.empty()) {
    // Phase D (repeated per wave): canonicalize (chunk-owner redirect, Part
    // 14) and validate every newly discovered candidate, then dedupe against
    // everything ever claimed in a previous wave - deterministically
    // address-sorted (std::set) so a wave's processing order, and therefore
    // every unresolved/warning entry it produces, never depends on
    // completion order between workers or between wave/non-wave (jobs=1) runs.
    std::set<GuestAddress> wave_set;
    for (const auto raw : frontier) {
      bool cyclic = false;
      const auto canonical = canonicalize_candidate(raw, ctx.hint_set_v2, cyclic);
      if (cyclic) {
        report.warnings.push_back(
            "chunk parent-redirect did not converge for candidate 0x" +
            [&] { std::ostringstream s; s << std::hex << raw; return s.str(); }() +
            " (possible cyclic FunctionChunk hint data)");
      }
      if (claimed.contains(canonical)) continue;
      if (!ExecutableRangeIndex::is_aligned_ppc_address(canonical)) {
        if (claimed.insert(canonical).second) {
          report.unresolved.push_back({canonical, canonical, "candidate-unaligned",
                                       "candidate address is not 4-byte aligned; cannot be a PPC "
                                       "instruction"});
          ++rejected_unaligned;
        }
        continue;
      }
      wave_set.insert(canonical);
    }
    frontier.clear();
    if (wave_set.empty()) break;
    for (const auto address : wave_set) claimed.insert(address);

    const std::vector<GuestAddress> wave(wave_set.begin(), wave_set.end());
    std::vector<FunctionAnalysisResult> results(wave.size());
    const auto run_one = [&](std::size_t i) { results[i] = analyze_function_candidate(wave[i], ctx); };
    pool.parallel_for(wave.size(), run_one);

    // Phase G (Part 2): deterministic merge - strictly in `wave`'s
    // address-sorted order, identical for jobs=1 and jobs=N.
    for (auto& result : results) {
      ++functions_analyzed;
      instruction_pattern_match_count += result.instruction_pattern_matches;
      resolved_indirect_via_dataflow += result.resolved_indirect_via_dataflow;
      resolved_indirect_via_jump_table += result.resolved_indirect_via_jump_table;
      for (auto& item : result.unresolved) report.unresolved.push_back(std::move(item));
      for (auto& warning : result.warnings) report.warnings.push_back(std::move(warning));
      if (result.outcome == FunctionAnalysisResult::Outcome::Accepted)
        report.functions.push_back(std::move(result.function));
      for (const auto& [discovered_address, discovered_source] : result.discovered) {
        frontier.push_back(discovered_address);
        auto& evidence = discovered_evidence[discovered_address];
        if (std::find(evidence.begin(), evidence.end(), discovered_source) == evidence.end())
          evidence.push_back(discovered_source);
      }
    }

    ++wave_count;
    if (effective.progress) {
      std::ostringstream message;
      message << "[Analysis] wave " << wave_count << ": " << functions_analyzed << " analyzed, "
              << report.functions.size() << " accepted, " << frontier.size() << " newly discovered";
      effective.progress(message.str());
    }
  }
  if (effective.progress) effective.progress("[Analysis] complete");


  std::sort(report.functions.begin(), report.functions.end(),
            [](const auto& a, const auto& b) { return a.guest_start < b.guest_start; });
  const auto codegen_duplicate_addresses_merged =
      deduplicate_functions_by_address(report.functions, report.warnings);
  for (auto& function : report.functions) {
    for (auto& other : report.functions) {
      if (&function == &other) continue;
      if (function.guest_start < other.guest_end && other.guest_start < function.guest_end) {
        report.warnings.push_back("overlapping functions at 0x" +
                                  [&] { std::ostringstream s; s << std::hex << function.guest_start; return s.str(); }() +
                                  " and 0x" +
                                  [&] { std::ostringstream s; s << std::hex << other.guest_start; return s.str(); }());
        break;
      }
    }
    for (const auto target : function.calls)
      for (auto& callee : report.functions)
        if (callee.guest_start == target) {
          callee.callers.push_back(function.guest_start);
          add_source(callee, DiscoverySource::DirectCall);
        }
    // Part 4/10/11 (Recomp Analysis V3): a non-linked branch whose target is
    // ANOTHER function's own start address (not an internal jump/chunk of
    // this same function - a chunk is merged into its parent's own IR, so
    // it never appears here as a separate DiscoveredFunction to match) is
    // real, proven control flow - genuine evidence the target is a callable
    // unit even though nothing ever `bl`'d it directly (a validated tail
    // call).
    for (const auto target : function.branch_references)
      if (target != function.guest_start)
        for (auto& callee : report.functions)
          if (callee.guest_start == target) add_source(callee, DiscoverySource::ValidatedTailCall);
    for (const auto target : function.branch_references)
      if (!executable_section(range_index, target))
        report.unresolved.push_back({function.guest_start, target, "branch-target", "target is not executable"});
      else if (!std::any_of(report.functions.begin(), report.functions.end(),
                            [target](const auto& candidate) {
                              if (candidate.guest_start == target) return true;
                              for (std::size_t i = 0; i + 1u < candidate.ranges.size(); i += 2u)
                                if (target >= candidate.ranges[i] && target < candidate.ranges[i + 1u])
                                  return true;
                              return false;
                            }))
        report.unresolved.push_back({function.guest_start, target, "branch-into-unknown-code",
                                     "executable target has no discovered function/chunk owner"});
  }

  // Part 4 (over-discovery fix): deduplicate final unresolved diagnostics by
  // (kind, address, target, detail) identity. The same physical site can
  // legitimately surface more than once while it is being produced above -
  // e.g. the exact same unresolved branch target referenced by several
  // different branch instructions within one function's `branch_references`
  // - but that is repeated emission of the SAME finding, not multiple
  // distinct findings, and must not inflate the top-level counters computed
  // below. A sort+unique on the full identity tuple (rather than an
  // unordered hash set) keeps this deterministic and identical between
  // jobs=1 and jobs=N, matching every other pass in this function.
  std::sort(report.unresolved.begin(), report.unresolved.end(),
           [](const UnresolvedReference& a, const UnresolvedReference& b) {
             if (a.kind != b.kind) return a.kind < b.kind;
             if (a.address != b.address) return a.address < b.address;
             if (a.target != b.target) return a.target < b.target;
             return a.detail < b.detail;
           });
  report.unresolved.erase(
      std::unique(report.unresolved.begin(), report.unresolved.end(),
                 [](const UnresolvedReference& a, const UnresolvedReference& b) {
                   return a.kind == b.kind && a.address == b.address && a.target == b.target &&
                          a.detail == b.detail;
                 }),
      report.unresolved.end());

  // Analysis diagnostics (Part 1.12/6.2) - computed once, after every
  // function/unresolved entry above is final.
  auto& diagnostics = report.diagnostics;
  diagnostics = {};
  for (const auto& function : report.functions) {
    if (function.native_replacement) {
      ++diagnostics.native_replacements_applied;
      continue;
    }
    if (source_contains(function, DiscoverySource::ModuleHint)) ++diagnostics.hinted_functions;
    else ++diagnostics.auto_discovered_functions;
    if (!function.compiled) ++diagnostics.analysis_errors;
    if (source_contains(function, DiscoverySource::TlsCallback)) ++diagnostics.candidates_from_tls_callbacks;
    // Part 5 (resolved-indirect reconciliation): distinct-function count,
    // directly comparable against resolved_indirect_via_dataflow/
    // _via_jump_table below (see AnalysisDiagnostics's doc comment).
    if (source_contains(function, DiscoverySource::ResolvedIndirect))
      ++diagnostics.functions_with_resolved_indirect_provenance;
  }
  if (report.hint_set_v2) {
    diagnostics.manual_chunks = report.hint_set_v2->chunks.size();
    diagnostics.switch_tables_resolved = report.hint_set_v2->switches.size();
    diagnostics.known_indirect_calls = report.hint_set_v2->indirect_calls.size();
    diagnostics.known_indirect_branches = report.hint_set_v2->indirect_branches.size();
    diagnostics.data_or_ignored_regions = report.hint_set_v2->regions.size();
    for (const auto& replacement : report.hint_set_v2->native_replacements)
      if (native_replacements::entry_for(replacement.kind) == nullptr)
        ++diagnostics.native_replacements_unsupported;
    for (const auto& helper : report.hint_set_v2->runtime_helpers) {
      if (!analysis::runtime_helper_kind_is_register_range(helper.kind)) continue;
      if (analysis::runtime_helper_kind_is_save(helper.kind)) ++diagnostics.register_save_helpers;
      else ++diagnostics.register_restore_helpers;
    }
    diagnostics.instruction_patterns_loaded = report.hint_set_v2->instruction_patterns.size();
  }
  diagnostics.instruction_pattern_matches = instruction_pattern_match_count;
  for (const auto& item : report.unresolved) {
    if (item.kind == "indirect-call" || item.kind == "indirect-branch")
      ++diagnostics.unresolved_indirect_sites;
    if (item.kind == "indirect-call") ++diagnostics.unresolved_indirect_calls;
    if (item.kind == "indirect-branch") ++diagnostics.unresolved_indirect_branches;
    if (item.kind == "invalid-ppc") ++diagnostics.invalid_ppc_sites;
  }
  if (report.hint_set_v2) {
    for (const auto& call : report.hint_set_v2->indirect_calls)
      if (!call.targets.empty()) ++diagnostics.resolved_indirect_calls;
    for (const auto& branch : report.hint_set_v2->indirect_branches)
      if (!branch.targets.empty()) ++diagnostics.resolved_indirect_branches;
    for (const auto& table : report.hint_set_v2->switches)
      if (!table.explicit_targets.empty() || (table.table_address && table.entry_count))
        ++diagnostics.resolved_indirect_branches;
  }

  // Performance/parallelism diagnostics (Part 19) - the wave engine's local
  // counters, folded in here (after the `diagnostics = {}` reset above)
  // rather than written directly during the loop, exactly like
  // `instruction_pattern_match_count` already was before this pass.
  diagnostics.codegen_duplicate_addresses_merged = codegen_duplicate_addresses_merged;
  diagnostics.candidate_functions_total = claimed.size();
  diagnostics.function_candidates_rejected_nonexec = rejected_nonexec;
  diagnostics.function_candidates_rejected_unaligned = rejected_unaligned;
  diagnostics.analysis_waves = wave_count;
  diagnostics.analysis_workers = worker_count;
  diagnostics.functions_analyzed = functions_analyzed;
  diagnostics.functions_compiled =
      static_cast<std::size_t>(std::count_if(report.functions.begin(), report.functions.end(),
                                             [](const auto& function) { return function.compiled; }));
  diagnostics.resolved_indirect_via_dataflow = resolved_indirect_via_dataflow;
  diagnostics.resolved_indirect_via_jump_table = resolved_indirect_via_jump_table;
  for (const auto& item : report.unresolved) {
    if (item.kind == "unsupported-ppc") ++diagnostics.unsupported_ppc_sites;
    if (item.kind == "unsupported-vmx") ++diagnostics.unsupported_vmx_sites;
  }
  diagnostics.analysis_duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - analysis_start)
          .count();

  return true;
}

bool generate_project(const DriverOptions& options, AnalysisReport& report, std::string& error) {
  const auto codegen_start = std::chrono::steady_clock::now();
  std::error_code ec;
  std::filesystem::create_directories(options.output / "functions", ec);
  if (ec) { error = "unable to create output directory: " + ec.message(); return false; }
  const auto cache_directory = options.output / ".cache";
  std::filesystem::create_directories(cache_directory, ec);
  // Shard the cache into 256 subdirectories (first 2 hex characters of each
  // function's content hash), created once up front here rather than
  // per-file inside the parallel loop below. Measured during development: a
  // single flat directory receiving thousands of concurrent small-file
  // creates from many worker threads showed real filesystem-level
  // contention (NTFS directory metadata locking, likely compounded by
  // antivirus real-time scanning on typical Windows dev/CI machines) severe
  // enough to make parallel codegen slower than serial on an I/O-dominated
  // synthetic benchmark. Sharding removes the single shared hot directory;
  // pre-creating every shard here (idempotent, cheap, serial) means no
  // worker ever needs to create a directory itself.
  for (unsigned high = 0; high < 16; ++high)
    for (unsigned low = 0; low < 16; ++low) {
      std::ostringstream shard_name;
      shard_name << std::hex << high << low;
      std::filesystem::create_directories(cache_directory / shard_name.str(), ec);
    }

  // Phase K (Part 2/16): parallel native code generation. Native-replacement
  // entries have no IR/guest bytes to emit (only a registry.cpp
  // lookup_compiled() case, handled below), so they never occupy a codegen
  // slot or count toward shard membership - matches the previous serial
  // loop's `continue` exactly.
  std::vector<const DiscoveredFunction*> codegen_items;
  codegen_items.reserve(report.functions.size());
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement) codegen_items.push_back(&function);

  if (!validate_codegen_uniqueness(codegen_items, report.diagnostics, error)) return false;

  if (options.progress)
    options.progress("[Xenon Recomp] Codegen: " + std::to_string(codegen_items.size()) + " functions");

  // Each item's shard membership is a pure function of its position in this
  // already-deterministic (guest-address-sorted) list, decided up front -
  // never by which worker happens to finish which item first (Part 16's
  // "stable sorted function addresses determine shard membership").
  const auto worker_count = resolve_worker_count(options.codegen_jobs);
  const WorkerPool pool(worker_count);
  std::vector<std::string> function_sources(codegen_items.size());
  pool.parallel_for(codegen_items.size(), [&](std::size_t i) {
    // cpu::backend::CppAotBackend is stateless (no members, every method
    // const - see include/xenon/cpu/backend/cpp_aot.hpp), so a fresh
    // instance per call is both correct and cheap; sharing one instance
    // across workers would be equally safe but this avoids any doubt.
    cpu::backend::CppAotBackend backend;
    const auto& function = *codegen_items[i];
    const auto hash_text = hash_name(function.source_hash);
    // Codegen cache key fix (generated-code deduplication / shard ownership
    // fix): the cache MUST be keyed by this function's own canonical
    // identity (guest_start), not by content hash alone. source_hash is
    // derived only from guest instruction bytes + FunctionChunk words +
    // configuration - it says nothing about WHICH address those bytes came
    // from, so two different, unrelated functions with byte-identical
    // machine code (extremely common for trivial stub bodies - a bare
    // `blr`, a `li r3,0; blr` return-0 thunk, etc. - across a real title's
    // tens of thousands of functions) previously hashed to the exact same
    // cache_path. Whichever function reached that path first won: every
    // other colliding function's `function_source` became a cache HIT
    // containing the FIRST function's own emitted text - literally defining
    // the first function's `_dispatch_v2`/`_v2`/base symbols a second time
    // (wherever the colliding function landed in shard order), while the
    // colliding function's OWN symbols were never emitted at all despite
    // registry.cpp still declaring and referencing them. This is the actual
    // root cause of the "function already has a body" (C2084) duplicate-
    // definition errors this fix addresses - not a duplicate
    // DiscoveredFunction, but a cache entry silently shared by two unrelated
    // ones. Including guest_start in the key makes that collision
    // impossible while still letting the exact same function hit its own
    // cache entry across incremental runs (same address + same bytes/config
    // -> same key -> valid, intentional reuse).
    const auto cache_path =
        cache_directory / hash_text.substr(0, 2) / (hash_text + "_" + hex_string(function.guest_start) + ".cpp");
    std::ifstream cached(cache_path);
    std::string function_source((std::istreambuf_iterator<char>(cached)), {});
    cached.close();
    if (function_source.empty()) {
      function_source = backend.emit_translation_unit(function.ir, function.name);
      // Each function's cache file is now keyed by (content hash, guest
      // address), which is unique per function by construction (report.
      // functions is address-deduplicated before codegen ever runs - see
      // deduplicate_functions_by_address()), so concurrent writes from
      // different workers never target the same path - no lock needed here.
      std::ofstream cache(cache_path);
      cache << function_source;
    }
    function_sources[i] = std::move(function_source);
  });

  if (options.progress) options.progress("[Codegen] " + std::to_string(function_sources.size()) + " functions emitted, writing shards...");

  // Phase L (Part 2/16): deterministic shard assembly - a serial pass over
  // already-computed text, in original order, identical regardless of how
  // the parallel emission above was scheduled.
  std::vector<std::filesystem::path> shards;
  std::string shard_text;
  std::filesystem::path shard_path;
  // Part 4 (shard ownership): each codegen item's shard membership is
  // already a one-to-one function of its position in the deduplicated,
  // address-sorted `codegen_items`/`function_sources` (no function can
  // appear in two shards or twice in one - this loop visits `index` exactly
  // once each, strictly increasing). `shard_function_count_current`/`_max`
  // below are purely a diagnostic record of that fact (Part 9's
  // codegen_max_functions_per_shard), not an enforcement mechanism.
  std::size_t shard_function_count_current = 0;
  std::size_t shard_function_count_max = 0;
  for (std::size_t index = 0; index < codegen_items.size(); ++index) {
    if (index % options.shard_function_count == 0) {
      if (!shard_path.empty()) {
        shard_function_count_max = std::max(shard_function_count_max, shard_function_count_current);
        std::ifstream old(shard_path);
        const std::string previous((std::istreambuf_iterator<char>(old)), {});
        if (previous != shard_text) {
          std::ofstream output(shard_path);
          output << shard_text;
        }
      }
      const auto path = options.output / "functions" / ("shard_" + [&] {
        std::ostringstream s; s << std::setfill('0') << std::setw(3) << (index / options.shard_function_count); return s.str();
      }() + ".cpp");
      shards.push_back(path);
      shard_path = path;
      shard_text = "// Generated by xenon codegen. Do not edit.\n";
      shard_function_count_current = 0;
    }
    shard_text += function_sources[index];
    ++shard_function_count_current;
  }
  if (!shard_path.empty()) {
    shard_function_count_max = std::max(shard_function_count_max, shard_function_count_current);
    std::ifstream old(shard_path);
    const std::string previous((std::istreambuf_iterator<char>(old)), {});
    if (previous != shard_text) {
      std::ofstream output(shard_path);
      output << shard_text;
    }
  }
  report.diagnostics.codegen_shards = shards.size();
  report.diagnostics.codegen_max_functions_per_shard = shard_function_count_max;
  if (options.progress) options.progress("[Codegen] " + std::to_string(shards.size()) + " source shards");
  std::ostringstream registry_text;
  std::ofstream registry_header(options.output / "registry.hpp");
  const bool has_native_replacements = std::any_of(
      report.functions.begin(), report.functions.end(),
      [](const auto& function) { return function.native_replacement.has_value(); });
  const bool has_runtime_helpers = report.hint_set_v2 &&
      !report.hint_set_v2->runtime_helpers.empty();
  registry_header << "#pragma once\n#include <cstdint>\n#include \"xenon/cpu/runtime.hpp\"\nnamespace xenon::recomp {\n"
                     "void bind_compiled_registry(xenon::cpu::ExecutionContext&) noexcept;\n"
                     "xenon::cpu::NativeCompiledEntry lookup_compiled(void*, xenon::cpu::ExecutionContext&, xenon::cpu::GuestAddress, xenon::cpu::CompiledLookupKind);\n"
                     // Part 1.9: setjmp/longjmp RuntimeHelper metadata, carried through to a
                     // real generated symbol a runtime/codegen consumer can read - see
                     // metadata.cpp for the values and docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md for
                     // what actually trapping/restoring at these addresses still requires
                     // (a CPU V2 change out of scope for this pass).
                     "extern const bool kHasSetJmpAddress;\n"
                     "extern const std::uint32_t kSetJmpAddress;\n"
                     "extern const bool kHasLongJmpAddress;\n"
                     "extern const std::uint32_t kLongJmpAddress;\n"
                     "}\n";
  registry_text << "// Generated compiled-function registry.\n#include \"registry.hpp\"\n"
                   "#include <cstddef>\n#include <cstdint>\n";
  if (has_runtime_helpers) {
    registry_text << "#include \"xenon/recomp/runtime_helpers.hpp\"\n";
  }
  if (has_native_replacements) {
    // A NativeReplacement hint (Part 1.10) means Xenon owns the
    // implementation at this address - the lookup_compiled() switch below
    // calls straight into xenon_recomp's own native_replacements.cpp
    // instead of a generated shard function.
    registry_text << "#include \"xenon/recomp/native_replacements.hpp\"\n";
  }
  // Function shards (functions/shard_*.cpp) define each compiled function at
  // global scope (see backend_cpp_aot.cpp's codegen - `using namespace
  // xenon::cpu;` then a bare `ExecutionResult <name>(...)`), so these
  // forward declarations must also be at global scope to name the same
  // symbol. Declaring them inside `namespace xenon::recomp` here was a real,
  // previously-undetected bug: a static-library-only build (xenon_game)
  // never needs to actually resolve these references (archiving doesn't
  // link), so the mismatched-namespace declarations silently built an
  // *unrelated*, always-undefined xenon::recomp::<name> symbol that only a
  // real caller linking a full executable/shared module would ever notice.
  // Native-replacement entries are excluded from every loop below: they have
  // no generated shard function to declare/reference, only a
  // lookup_compiled() case pointing at Xenon's own implementation.
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement)
      registry_text << "extern xenon::cpu::ExecutionResult " << function.name
                    << "(xenon::cpu::CpuState&, xenon::cpu::MemoryPort&, xenon::cpu::RuntimeServices&);\n";
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement)
      registry_text << "extern xenon::cpu::ExecutionResult " << function.name
                    << "_v2(xenon::cpu::ExecutionContext&);\n";
  const auto legacy_compiled_count = std::count_if(
      report.functions.begin(), report.functions.end(),
      [](const auto& function) { return function.compiled && !function.native_replacement; });
  registry_text << "namespace xenon::recomp {\n"
                   "using CompiledFunction = xenon::cpu::ExecutionResult (*)(xenon::cpu::CpuState&, xenon::cpu::MemoryPort&, xenon::cpu::RuntimeServices&);\n"
                   "using CompiledFunctionV2 = xenon::cpu::NativeCompiledEntry;\n"
                   "struct CompiledEntry { std::uint32_t guest_start; CompiledFunction function; };\n"
                << "const CompiledEntry kCompiledFunctions["
                << std::max<std::size_t>(1u, static_cast<std::size_t>(legacy_compiled_count))
                << "] = {\n";
  // registry_text is a single ostringstream shared by every section below
  // (the compiled-function table, the lookup_compiled() switch, and the
  // trailing kCompiledFunctionCount declaration). std::hex/std::dec are
  // *sticky* stream-formatting state, not per-call flags: a previous bug
  // here applied std::hex to emit each guest_start address in the loop
  // below, then wrote kCompiledFunctionCount immediately after with no
  // reset, so the count itself was emitted as an invalid (or, for a count
  // like 16 -> "10", silently wrong-but-valid) hex literal. Every guest
  // address destined for this stream is now formatted through hex_string(),
  // which uses its own throwaway ostringstream, so registry_text itself
  // never enters hex mode and no later decimal write can inherit stale
  // formatting state.
  for (const auto& function : report.functions)
    if (function.compiled && !function.native_replacement)
      registry_text << "  {0x" << hex_string(function.guest_start) << ", &" << function.name << "},\n";
  registry_text << "};\nconst std::size_t kCompiledFunctionCount = " << legacy_compiled_count << ";\n}\n";
  registry_text << "namespace xenon::recomp {\n"
                   "CompiledFunctionV2 lookup_compiled(void*, xenon::cpu::ExecutionContext&, xenon::cpu::GuestAddress target, xenon::cpu::CompiledLookupKind) {\n"
                   "  switch (target) {\n";
  if (report.hint_set_v2) {
    for (const auto& helper :
         analysis::expand_runtime_helpers(report.hint_set_v2->runtime_helpers)) {
      registry_text << "    case 0x" << hex_string(helper.address)
                    << ": return &xenon::recomp::runtime_helpers::"
                    << runtime_helper_native_symbol(helper) << ";\n";
    }
  }
  for (const auto& function : report.functions) {
    if (function.native_replacement) {
      const auto* native_name = native_replacement_function_name(*function.native_replacement);
      if (native_name != nullptr) {
        registry_text << "    case 0x" << hex_string(function.guest_start)
                      << ": return &xenon::recomp::native_replacements::" << native_name << ";\n";
      }
      continue;
    }
    if (function.compiled)
      registry_text << "    case 0x" << hex_string(function.guest_start) << ": return &" << function.name << "_v2;\n";
  }
  registry_text << "    default: return nullptr;\n  }\n}\n"
                   "void bind_compiled_registry(xenon::cpu::ExecutionContext& context) noexcept {\n"
                   "  context.compiled_registry = nullptr;\n"
                   "  context.compiled_lookup = &lookup_compiled;\n"
                   "}\n}\n";
  std::ofstream registry(options.output / "registry.cpp");
  registry << registry_text.str();
  std::ofstream imports(options.output / "imports.cpp");
  imports << "// Generated import manifest.\n#include <cstddef>\n#include <cstdint>\nnamespace xenon::recomp {\n"
             "struct GeneratedImport { const char* module; const char* symbol; std::uint16_t ordinal; std::uint32_t thunk; };\n"
             "const GeneratedImport kGeneratedImports["
             + std::to_string(std::max<std::size_t>(1u, report.image.imports.size()))
             + "] = {\n";
  // A real, previously-undetected bug lived here: std::hex applied to
  // `imports` for one entry's guest_thunk is STICKY on the underlying
  // ostream - it silently also applied to the NEXT entry's `item.ordinal`
  // (printed before any hex/dec marker of its own), corrupting that
  // decimal field. Most ordinal values happened to still be all-digit
  // hex representations (e.g. 0x190 in hex prints as "190", a numerically
  // different but still syntactically valid decimal-looking literal), so
  // this silently produced a WRONG (never validated by any test - nothing
  // reads this generated table back) ordinal for every import after the
  // first. An ordinal whose hex digits include a letter (e.g. NtCreateFile,
  // 0x00D2 = 210, prints as "d2" in leftover hex mode) turns this from a
  // silently wrong value into an outright compile error - which is how this
  // pass found it. Fixed by explicitly forcing std::dec immediately before
  // the decimal field and std::dec again after the hex one, so no iteration
  // can inherit stream state from a previous one.
  for (const auto& item : report.image.imports)
    imports << "  {\"" << item.module << "\", \"" << item.symbol << "\", " << std::dec << item.ordinal
            << ", 0x" << std::hex << item.guest_thunk << std::dec << "},\n";
  imports << "};\nconst std::size_t kGeneratedImportCount = "
             "sizeof(kGeneratedImports) / sizeof(kGeneratedImports[0]);\n}\n";
  std::optional<std::uint32_t> setjmp_address, longjmp_address;
  if (report.hint_set_v2) {
    for (const auto& helper : report.hint_set_v2->runtime_helpers) {
      if (helper.kind == analysis::RuntimeHelperKind::SetJmp) setjmp_address = helper.address;
      else if (helper.kind == analysis::RuntimeHelperKind::LongJmp) longjmp_address = helper.address;
    }
  }
  std::ofstream metadata(options.output / "metadata.cpp");
  metadata << "// Generated analysis metadata.\n#include \"registry.hpp\"\nnamespace xenon::recomp {\n";
  metadata << "const bool kHasSetJmpAddress = " << (setjmp_address ? "true" : "false") << ";\n";
  metadata << "const std::uint32_t kSetJmpAddress = 0x" << std::hex << setjmp_address.value_or(0) << std::dec << ";\n";
  metadata << "const bool kHasLongJmpAddress = " << (longjmp_address ? "true" : "false") << ";\n";
  metadata << "const std::uint32_t kLongJmpAddress = 0x" << std::hex << longjmp_address.value_or(0) << std::dec << ";\n";
  metadata << "}\n";
  metadata << "// entry=0x" << std::hex << report.image.entry_point << " functions=" << std::dec << report.functions.size()
           << " unresolved=" << report.unresolved.size() << " config_hash=0x" << std::hex << report.configuration_hash << "\n";
  std::ofstream hooks(options.output / "hooks.cpp");
  hooks << "// Generated module hooks and patch manifest. Runtime integration owns semantics.\n";
  for (const auto& hint : options.hints) {
    for (const auto& hook : hint.special_hooks)
      hooks << "// hook[" << hint.name << "] " << hook << "\n";
    for (const auto& patch : hint.patches)
      hooks << "// patch[" << hint.name << "] " << patch << "\n";
  }
  if (report.hint_set_v2) {
    for (const auto& hook : report.hint_set_v2->hooks)
      hooks << "// hook-v2[0x" << std::hex << hook.address << std::dec << "] "
            << hook.native_replacement_identity << " " << hook.description << "\n";
    for (const auto& patch : report.hint_set_v2->patches)
      hooks << "// patch-v2[0x" << std::hex << patch.address << std::dec << "] bytes="
            << patch.patch_bytes_hex << " " << patch.description << "\n";
  }
  std::ofstream json(options.output / "analysis.json");
  json << format_report_json(report);
  std::ofstream manifest(options.output / "manifest.txt");
  manifest << "configuration_hash=0x" << std::hex << report.configuration_hash << "\n";
  for (const auto& path : shards) manifest << path.filename().string() << "\n";
  const auto function_directory = options.output / "functions";
  for (const auto& entry : std::filesystem::directory_iterator(function_directory, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp" ||
        entry.path().filename().string().find("shard_") != 0)
      continue;
    if (std::find(shards.begin(), shards.end(), entry.path()) == shards.end())
      std::filesystem::remove(entry.path(), ec);
  }
  // The canonical Xenon module ABI (docs/runtime/RUNTIME_HOST.md): a single dynamic
  // library exporting Xenon_BindCompiledRegistry(ExecutionContext&), which
  // XenonSession::load_native_extension() resolves via LoadLibrary/dlopen.
  // xenon_game (STATIC, above) stays the reusable embedding/development
  // artifact; this wraps it in a SHARED module without recompiling any
  // generated game code, so a real title's build can produce both from the
  // same generated project.
  // The effective-image identity of exactly the executable this module was
  // analyzed/compiled from (report.image - whatever bytes options.input
  // pointed at, base or already-title-update-patched by upstream tooling).
  // Emitted below as Xenon_SupportedExecutableRevisions() so
  // XenonSession::load_native_extension() can refuse to run this module's
  // compiled code against a different effective executable revision (see
  // docs/runtime/RUNTIME_HOST.md "Effective executable identity" / section 8 of
  // docs/runtime/RUNTIME_SESSION.md's title-update integration).
  const auto module_revision_hex =
      xbox::format_effective_image_hash(xbox::compute_effective_image_hash(report.image));

  std::ofstream module_export(options.output / "module_export.cpp");
  module_export << "// Generated Xenon game module export shim - wraps the\n"
                   "// generated compiled-code registry (registry.cpp) in the\n"
                   "// canonical Xenon_BindCompiledRegistry ABI a loadable game\n"
                   "// module must export. See docs/runtime/RUNTIME_HOST.md.\n"
                   "#include \"registry.hpp\"\n"
                   "#if defined(_WIN32)\n"
                   "#define XENON_GAME_MODULE_EXPORT extern \"C\" __declspec(dllexport)\n"
                   "#else\n"
                   "#define XENON_GAME_MODULE_EXPORT extern \"C\" __attribute__((visibility(\"default\")))\n"
                   "#endif\n"
                   "XENON_GAME_MODULE_EXPORT void Xenon_BindCompiledRegistry(xenon::cpu::ExecutionContext& context) {\n"
                   "  xenon::recomp::bind_compiled_registry(context);\n"
                   "}\n"
                   "// Declares the exact effective-XEX revision this module's compiled\n"
                   "// registry was generated from - see XenonSession::load_native_extension().\n"
                   "XENON_GAME_MODULE_EXPORT const char* Xenon_SupportedExecutableRevisions() {\n"
                   "  return \"" + module_revision_hex + "\";\n"
                   "}\n";
  std::ofstream build(options.output / "CMakeLists.txt");
  build << "cmake_minimum_required(VERSION 3.25)\n"
           "project(xenon_game_generated LANGUAGES CXX)\n"
           "set(CMAKE_CXX_STANDARD 20)\n"
           "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
           "set(XENON_BUILD_TESTS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_BUILD_BENCHMARKS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_BUILD_LAUNCHER OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_GRAPHICS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_AUDIO OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_INPUT OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_DEPENDENCY_MODE SYSTEM CACHE STRING \"\" FORCE)\n"
           "set(XENON_AUTO_BOOTSTRAP_DEPS OFF CACHE BOOL \"\" FORCE)\n"
           "if(MSVC)\n"
           "  set(CMAKE_CXX_FLAGS \"${CMAKE_CXX_FLAGS} /constexpr:steps2147483647\")\n"
           "endif()\n"
           "set(XENON_RECOMP_ROOT \"\" CACHE PATH \"Path to the Xenon-Recomp source tree\")\n"
           "if(NOT XENON_RECOMP_ROOT)\n"
           "  message(FATAL_ERROR \"Set XENON_RECOMP_ROOT to the Xenon-Recomp source tree\")\n"
           "endif()\n"
           "add_subdirectory(${XENON_RECOMP_ROOT} xenon-recomp EXCLUDE_FROM_ALL)\n"
           "add_library(xenon_game STATIC\n";
  for (const auto& path : shards)
    build << "  " << std::filesystem::relative(path, options.output).generic_string() << "\n";
  build << "  registry.cpp\n  imports.cpp\n  metadata.cpp\n  hooks.cpp\n"
           ")\n"
           // Xenon::Recomp: native_replacements.hpp/.cpp - referenced by
           // registry.cpp's lookup_compiled() switch whenever this module
           // used at least one NativeReplacement hint (Part 1.10). Always
           // linked (small, already a dependency of the driver itself) so a
           // module gaining its first native replacement never needs a
           // build-system change.
           "target_link_libraries(xenon_game PRIVATE Xenon::CPU Xenon::Memory Xenon::XboxKernelIo Xenon::Recomp)\n"
           "target_include_directories(xenon_game PRIVATE ${XENON_RECOMP_ROOT}/include)\n"
           "set_property(TARGET xenon_game PROPERTY POSITION_INDEPENDENT_CODE ON)\n"
           "add_library(xenon_game_module SHARED module_export.cpp)\n"
           "target_link_libraries(xenon_game_module PRIVATE xenon_game)\n"
           "target_include_directories(xenon_game_module PRIVATE ${XENON_RECOMP_ROOT}/include)\n";
  report.diagnostics.codegen_duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - codegen_start)
          .count();
  if (options.progress) options.progress("[Codegen] complete");
  return true;
}

std::string format_report(const AnalysisReport& report) {
  std::ostringstream out;
  out << "entry: 0x" << std::hex << report.image.entry_point << "\nfunctions: " << std::dec << report.functions.size()
      << "\nunresolved: " << report.unresolved.size() << "\n";
  const auto& diagnostics = report.diagnostics;
  out << "diagnostics: auto_discovered=" << diagnostics.auto_discovered_functions
      << " hinted=" << diagnostics.hinted_functions << " manual_chunks=" << diagnostics.manual_chunks
      << " switch_tables=" << diagnostics.switch_tables_resolved
      << " known_indirect_calls=" << diagnostics.known_indirect_calls
      << " known_indirect_branches=" << diagnostics.known_indirect_branches
      << " unresolved_indirect_sites=" << diagnostics.unresolved_indirect_sites
      << " native_replacements_applied=" << diagnostics.native_replacements_applied
      << " native_replacements_unsupported=" << diagnostics.native_replacements_unsupported
      << " data_or_ignored_regions=" << diagnostics.data_or_ignored_regions
      << " analysis_errors=" << diagnostics.analysis_errors << "\n";
  out << "runtime helpers: register-save helpers: " << diagnostics.register_save_helpers
      << " register-restore helpers: " << diagnostics.register_restore_helpers << "\n";
  out << "instruction pattern rules: loaded: " << diagnostics.instruction_patterns_loaded
      << " matches: " << diagnostics.instruction_pattern_matches << "\n";
  out << "codegen: input=" << diagnostics.codegen_input_functions
      << " unique=" << diagnostics.codegen_unique_functions
      << " duplicate_addresses_merged=" << diagnostics.codegen_duplicate_addresses_merged
      << " duplicate_symbols_rejected=" << diagnostics.codegen_duplicate_symbols_rejected
      << " shards=" << diagnostics.codegen_shards
      << " max_functions_per_shard=" << diagnostics.codegen_max_functions_per_shard << "\n";
  for (const auto& function : report.functions)
    out << "0x" << std::hex << function.guest_start << "-0x" << function.guest_end << " "
        << function.name << " confidence=" << std::dec << function.confidence
        << " status=" << (function.compiled ? "compiled" : "unresolved") << "\n";
  for (const auto& warning : report.warnings)
    out << "warning: " << warning << "\n";
  for (const auto& item : report.unresolved)
    out << "warning 0x" << std::hex << item.address << " " << item.kind << ": " << item.detail << "\n";
  return out.str();
}

std::string format_report_json(const AnalysisReport& report) {
  std::ostringstream out;
  const auto& diagnostics = report.diagnostics;
  out << "{\n  \"entry_point\": " << report.image.entry_point
      << ",\n  \"configuration_hash\": " << report.configuration_hash
      << ",\n  \"diagnostics\": {"
      << "\"auto_discovered_functions\": " << diagnostics.auto_discovered_functions
      << ", \"hinted_functions\": " << diagnostics.hinted_functions
      << ", \"manual_chunks\": " << diagnostics.manual_chunks
      << ", \"switch_tables_resolved\": " << diagnostics.switch_tables_resolved
      << ", \"known_indirect_calls\": " << diagnostics.known_indirect_calls
      << ", \"known_indirect_branches\": " << diagnostics.known_indirect_branches
      << ", \"unresolved_indirect_sites\": " << diagnostics.unresolved_indirect_sites
      << ", \"native_replacements_applied\": " << diagnostics.native_replacements_applied
      << ", \"native_replacements_unsupported\": " << diagnostics.native_replacements_unsupported
      << ", \"data_or_ignored_regions\": " << diagnostics.data_or_ignored_regions
      << ", \"analysis_errors\": " << diagnostics.analysis_errors
      << ", \"register_save_helpers\": " << diagnostics.register_save_helpers
      << ", \"register_restore_helpers\": " << diagnostics.register_restore_helpers
      << ", \"instruction_patterns_loaded\": " << diagnostics.instruction_patterns_loaded
      << ", \"instruction_pattern_matches\": " << diagnostics.instruction_pattern_matches
      // Performance/parallelism diagnostics (Part 19) - purely additive
      // fields; a consumer that only reads the counters above is unaffected.
      << ", \"candidate_functions_total\": " << diagnostics.candidate_functions_total
      << ", \"function_candidates_rejected_nonexec\": " << diagnostics.function_candidates_rejected_nonexec
      << ", \"function_candidates_rejected_unaligned\": " << diagnostics.function_candidates_rejected_unaligned
      << ", \"analysis_waves\": " << diagnostics.analysis_waves
      << ", \"analysis_workers\": " << diagnostics.analysis_workers
      << ", \"functions_analyzed\": " << diagnostics.functions_analyzed
      << ", \"functions_compiled\": " << diagnostics.functions_compiled
      << ", \"invalid_ppc_sites\": " << diagnostics.invalid_ppc_sites
      << ", \"unresolved_indirect_calls\": " << diagnostics.unresolved_indirect_calls
      << ", \"unresolved_indirect_branches\": " << diagnostics.unresolved_indirect_branches
      << ", \"resolved_indirect_calls\": " << diagnostics.resolved_indirect_calls
      << ", \"resolved_indirect_branches\": " << diagnostics.resolved_indirect_branches
      << ", \"analysis_duration_ms\": " << diagnostics.analysis_duration_ms
      << ", \"codegen_duration_ms\": " << diagnostics.codegen_duration_ms
      // Recomp Analysis V3 additions - purely additive.
      << ", \"resolved_indirect_via_dataflow\": " << diagnostics.resolved_indirect_via_dataflow
      << ", \"candidates_from_tls_callbacks\": " << diagnostics.candidates_from_tls_callbacks
      << ", \"unsupported_ppc_sites\": " << diagnostics.unsupported_ppc_sites
      << ", \"unsupported_vmx_sites\": " << diagnostics.unsupported_vmx_sites
      // Tail-call over-discovery fix additions - purely additive; see
      // AnalysisDiagnostics's doc comments for how these three reconcile.
      << ", \"resolved_indirect_via_jump_table\": " << diagnostics.resolved_indirect_via_jump_table
      << ", \"functions_with_resolved_indirect_provenance\": "
      << diagnostics.functions_with_resolved_indirect_provenance
      // Codegen ownership/dedup diagnostics (generated-code deduplication /
      // shard ownership fix) - purely additive.
      << ", \"codegen_duplicate_addresses_merged\": " << diagnostics.codegen_duplicate_addresses_merged
      << ", \"codegen_input_functions\": " << diagnostics.codegen_input_functions
      << ", \"codegen_unique_functions\": " << diagnostics.codegen_unique_functions
      << ", \"codegen_duplicate_symbols_rejected\": " << diagnostics.codegen_duplicate_symbols_rejected
      << ", \"codegen_shards\": " << diagnostics.codegen_shards
      << ", \"codegen_max_functions_per_shard\": " << diagnostics.codegen_max_functions_per_shard
      << "},\n  \"functions\": [\n";
  for (std::size_t i = 0; i < report.functions.size(); ++i) {
    const auto& function = report.functions[i];
    out << "    {\"start\": " << function.guest_start
        << ", \"end\": " << function.guest_end
        << ", \"name\": \"" << json_escape(function.name)
        << "\", \"confidence\": " << function.confidence
        << ", \"compiled\": " << (function.compiled ? "true" : "false");
    // Provenance (Part 9): every DiscoverySource that contributed to this
    // function being found - lets a real-title investigation answer "why
    // does Xenon think this address is a function" without re-running
    // analysis under a debugger.
    out << ", \"sources\": [";
    for (std::size_t s = 0; s < function.sources.size(); ++s) {
      out << "\"" << discovery_source_name(function.sources[s]) << "\"";
      if (s + 1 != function.sources.size()) out << ", ";
    }
    out << "]";
    if (function.native_replacement)
      out << ", \"native_replacement\": \""
          << analysis::native_replacement_kind_name(*function.native_replacement) << "\"";
    if (!function.error.empty()) out << ", \"error\": \"" << json_escape(function.error) << "\"";
    out << "}";
    if (i + 1 != report.functions.size()) out << ',';
    out << '\n';
  }
  out << "  ],\n  \"unresolved\": [\n";
  for (std::size_t i = 0; i < report.unresolved.size(); ++i) {
    const auto& item = report.unresolved[i];
    out << "    {\"address\": " << item.address << ", \"target\": " << item.target
        << ", \"kind\": \"" << json_escape(item.kind) << "\", \"detail\": \""
        << json_escape(item.detail) << "\"}";
    if (i + 1 != report.unresolved.size()) out << ',';
    out << '\n';
  }
  out << "  ]\n}\n";
  return out.str();
}

std::string format_ir(const DiscoveredFunction& function) {
  std::ostringstream out;
  out << "function " << function.name << " @ 0x" << std::hex << function.guest_start << "\n";
  for (const auto& block : function.ir.blocks)
    out << "  block 0x" << std::hex << block.guest_address << "-0x" << block.end_address
        << " instructions=" << std::dec << block.instructions.size() << "\n";
  for (const auto& block : function.ir.blocks)
    for (const auto& instruction : block.instructions)
      out << "    0x" << std::hex << instruction.guest_address
          << " word=0x" << instruction.guest_word
          << " op=" << std::dec << static_cast<unsigned>(instruction.op)
          << " result=" << instruction.result << "\n";
  return out.str();
}

}  // namespace xenon::recomp
