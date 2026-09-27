#include "xenon/recomp/knowledge_base.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "xenon/core/json.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/instruction.hpp"
#include "xenon/recomp/driver.hpp"

namespace xenon::recomp {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kDefaultAnchorInstructions = 8u;
constexpr std::uint32_t kMaxFingerprintInstructions = 1'000'000u;

void mix_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
  hash ^= value;
  hash *= kFnvPrime;
}

template <typename T, bool IsEnum = std::is_enum_v<T>>
struct UnsignedFor;
template <typename T>
struct UnsignedFor<T, false> { using type = std::make_unsigned_t<T>; };
template <typename T>
struct UnsignedFor<T, true> { using type = std::make_unsigned_t<std::underlying_type_t<T>>; };
template <>
struct UnsignedFor<bool, false> { using type = std::uint8_t; };

template <typename T>
void mix_value(std::uint64_t& hash, T value) noexcept {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  using U = typename UnsignedFor<T>::type;
  U bits = static_cast<U>(value);
  for (std::size_t i = 0; i < sizeof(U); ++i) {
    mix_byte(hash, static_cast<std::uint8_t>(bits & 0xFFu));
    bits >>= 8u;
  }
}

void mix_string(std::uint64_t& hash, std::string_view text) noexcept {
  mix_value(hash, static_cast<std::uint32_t>(text.size()));
  for (const auto ch : text) mix_byte(hash, static_cast<std::uint8_t>(ch));
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

[[nodiscard]] bool parse_hex64(std::string_view text, std::uint64_t& value) noexcept {
  if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2u);
  if (text.empty() || text.size() > 16u) return false;
  value = 0u;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] const xbox::XexSection* section_for(const xbox::XexImage& image,
                                                  std::uint32_t address) noexcept {
  for (const auto& section : image.sections) {
    const auto begin = static_cast<std::uint64_t>(section.virtual_address);
    const auto end = begin + section.bytes.size();
    if (address >= begin && static_cast<std::uint64_t>(address) + 4u <= end) return &section;
  }
  return nullptr;
}

[[nodiscard]] bool read_word(const xbox::XexImage& image, std::uint32_t address,
                             std::uint32_t& word) noexcept {
  const auto* section = section_for(image, address);
  if (!section || address < section->virtual_address) return false;
  const auto offset = static_cast<std::size_t>(address - section->virtual_address);
  if (offset + 4u > section->bytes.size()) return false;
  word = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section->bytes[offset])) << 24u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section->bytes[offset + 1u])) << 16u) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section->bytes[offset + 2u])) << 8u) |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(section->bytes[offset + 3u]));
  return true;
}

struct NormalizedToken {
  std::uint64_t instruction{};
  std::uint64_t constant{};
  bool terminator{};
  bool linked_branch{};
  bool indirect_branch{};
};

[[nodiscard]] NormalizedToken normalize_instruction(const cpu::DecodedInstruction& instruction) noexcept {
  NormalizedToken token{};
  std::uint64_t shape = kFnvOffset;
  std::uint64_t constants = kFnvOffset;
  if (!instruction.valid()) {
    mix_value(shape, 0xFFFFFFFFu);
    mix_value(shape, instruction.primary());
    mix_value(constants, instruction.word);
    token.instruction = shape;
    token.constant = constants;
    return token;
  }

  const auto& info = *instruction.info;
  mix_value(shape, info.id.value);
  mix_value(shape, info.format);
  mix_value(shape, info.group);

  // Registers and architecturally meaningful flag bits stay in the shape;
  // absolute/relocatable immediates do not. This makes the primary identity
  // robust to rebasing and most linker movement while preserving operand
  // structure strongly enough to avoid opcode-only collisions.
  switch (info.format) {
    case cpu::InstructionFormat::D:
    case cpu::InstructionFormat::DS:
      mix_value(shape, instruction.rt());
      mix_value(shape, instruction.ra());
      mix_value(constants, instruction.uimm16());
      break;
    case cpu::InstructionFormat::B:
      mix_value(shape, instruction.bo());
      mix_value(shape, instruction.bi());
      mix_value(shape, instruction.aa());
      mix_value(shape, instruction.lk());
      break;
    case cpu::InstructionFormat::I:
      mix_value(shape, instruction.aa());
      mix_value(shape, instruction.lk());
      break;
    case cpu::InstructionFormat::M:
      mix_value(shape, instruction.rs());
      mix_value(shape, instruction.ra());
      mix_value(constants, instruction.sh32());
      mix_value(constants, instruction.mb32());
      mix_value(constants, instruction.me32());
      mix_value(shape, instruction.rc());
      break;
    case cpu::InstructionFormat::MD:
    case cpu::InstructionFormat::MDS:
      mix_value(shape, instruction.rs());
      mix_value(shape, instruction.ra());
      mix_value(shape, instruction.rb());
      mix_value(constants, instruction.sh64_md());
      mix_value(constants, instruction.mb64_md());
      mix_value(shape, instruction.rc());
      break;
    case cpu::InstructionFormat::VX128:
    case cpu::InstructionFormat::VX128_1:
    case cpu::InstructionFormat::VX128_2:
    case cpu::InstructionFormat::VX128_3:
    case cpu::InstructionFormat::VX128_4:
    case cpu::InstructionFormat::VX128_5:
    case cpu::InstructionFormat::VX128_R:
    case cpu::InstructionFormat::VX128_P:
      mix_value(shape, instruction.vx128_vd());
      mix_value(shape, instruction.vx128_va());
      mix_value(shape, instruction.vx128_vb());
      mix_value(shape, instruction.rc());
      break;
    default:
      mix_value(shape, instruction.rt());
      mix_value(shape, instruction.ra());
      mix_value(shape, instruction.rb());
      mix_value(shape, instruction.frc());
      mix_value(shape, instruction.rc());
      mix_value(shape, instruction.oe());
      if (info.format == cpu::InstructionFormat::XFX) mix_value(constants, instruction.spr());
      break;
  }

  // Secondary constant channel: useful corroboration, never mandatory for a
  // match. Branch displacements are intentionally excluded because they are
  // linker/layout dependent. The full D/DS immediate lives only here.
  if (info.format != cpu::InstructionFormat::D && info.format != cpu::InstructionFormat::DS &&
      info.format != cpu::InstructionFormat::B && info.format != cpu::InstructionFormat::I &&
      info.format != cpu::InstructionFormat::M && info.format != cpu::InstructionFormat::MD &&
      info.format != cpu::InstructionFormat::MDS && info.format != cpu::InstructionFormat::XFX) {
    mix_value(constants, instruction.word & ~cpu::mask_for_format(info.format));
  }

  token.instruction = shape;
  token.constant = constants;
  token.terminator = info.group == cpu::InstructionGroup::Branch ||
                     info.mnemonic == "sc" || info.mnemonic == "tw" ||
                     info.mnemonic == "twi" || info.mnemonic == "td" ||
                     info.mnemonic == "tdi";
  token.linked_branch = info.group == cpu::InstructionGroup::Branch && instruction.lk();
  token.indirect_branch = info.group == cpu::InstructionGroup::Branch &&
                          (info.mnemonic == "bctr" || info.mnemonic == "bcctr" ||
                           info.mnemonic == "bclr" || info.mnemonic == "blr" ||
                           info.mnemonic == "bcctrl" || info.mnemonic == "bclrl");
  return token;
}

struct FingerprintAccumulator {
  std::uint64_t instructions{kFnvOffset};
  std::uint64_t constants{kFnvOffset};
  std::uint32_t instruction_count{};
  std::uint32_t byte_size{};
};

void append_instruction(FingerprintAccumulator& out, const cpu::DecodedInstruction& instruction) noexcept {
  const auto token = normalize_instruction(instruction);
  mix_value(out.instructions, token.instruction);
  mix_value(out.constants, token.constant);
  ++out.instruction_count;
  out.byte_size += 4u;
}

[[nodiscard]] std::pair<std::uint64_t, std::uint32_t> compute_anchor(
    const xbox::XexImage& image, std::uint32_t address, std::uint32_t max_instructions) noexcept {
  cpu::Decoder decoder;
  std::uint64_t hash = kFnvOffset;
  std::uint32_t count = 0u;
  max_instructions = std::clamp(max_instructions, 1u, 32u);
  for (std::uint32_t index = 0; index < max_instructions; ++index) {
    std::uint32_t word = 0u;
    const auto pc64 = static_cast<std::uint64_t>(address) + index * 4ull;
    if (pc64 > std::numeric_limits<std::uint32_t>::max() ||
        !read_word(image, static_cast<std::uint32_t>(pc64), word)) break;
    const auto instruction = decoder.decode(static_cast<std::uint32_t>(pc64), word);
    const auto token = normalize_instruction(instruction);
    mix_value(hash, token.instruction);
    ++count;
    if (!instruction.valid() || token.terminator) break;
  }
  mix_value(hash, count);
  return {count == 0u ? 0u : hash, count};
}

[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> function_ranges(
    const DiscoveredFunction& function) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
  if (!function.ranges.empty()) {
    for (std::size_t i = 0; i + 1u < function.ranges.size(); i += 2u)
      if (function.ranges[i] < function.ranges[i + 1u])
        ranges.emplace_back(function.ranges[i], function.ranges[i + 1u]);
  } else if (function.guest_start < function.guest_end) {
    ranges.emplace_back(function.guest_start, function.guest_end);
  }
  std::sort(ranges.begin(), ranges.end());
  return ranges;
}

[[nodiscard]] std::uint64_t fingerprint_cfg(const DiscoveredFunction& function) noexcept {
  std::uint64_t hash = kFnvOffset;
  std::vector<const cpu::ir::Block*> blocks;
  blocks.reserve(function.ir.blocks.size());
  for (const auto& block : function.ir.blocks) blocks.push_back(&block);
  std::sort(blocks.begin(), blocks.end(), [](const auto* a, const auto* b) {
    return a->guest_address < b->guest_address;
  });
  std::unordered_map<std::uint32_t, std::uint32_t> ordinal;
  ordinal.reserve(blocks.size());
  for (std::uint32_t i = 0; i < blocks.size(); ++i) ordinal.emplace(blocks[i]->guest_address, i);

  mix_value(hash, static_cast<std::uint32_t>(blocks.size()));
  for (std::uint32_t i = 0; i < blocks.size(); ++i) {
    const auto& block = *blocks[i];
    mix_value(hash, i);
    mix_value(hash, static_cast<std::uint32_t>(block.instructions.size()));
    mix_value(hash, static_cast<std::uint32_t>(block.successors.size()));
    mix_value(hash, static_cast<std::uint32_t>(block.predecessors.size()));
    mix_value(hash, static_cast<std::uint8_t>(block.has_external_exit));
    mix_value(hash, static_cast<std::uint8_t>(block.has_indirect_exit));
    mix_value(hash, static_cast<std::uint8_t>(block.has_indirect_call));
    std::vector<std::tuple<std::uint8_t, std::uint8_t, std::int32_t>> edges;
    edges.reserve(block.successors.size());
    for (const auto& edge : block.successors) {
      std::int32_t target_delta = std::numeric_limits<std::int32_t>::min();
      if (edge.local) {
        if (const auto it = ordinal.find(edge.target); it != ordinal.end())
          target_delta = static_cast<std::int32_t>(it->second) - static_cast<std::int32_t>(i);
      }
      edges.emplace_back(static_cast<std::uint8_t>(edge.kind),
                         static_cast<std::uint8_t>(edge.local), target_delta);
    }
    std::sort(edges.begin(), edges.end());
    for (const auto& [kind, local, delta] : edges) {
      mix_value(hash, kind);
      mix_value(hash, local);
      mix_value(hash, delta);
    }
  }
  return hash;
}

[[nodiscard]] std::uint64_t fingerprint_call_neighborhood(const DiscoveredFunction& function,
                                                          std::uint32_t& external_calls) noexcept {
  std::uint64_t hash = kFnvOffset;
  external_calls = 0u;
  std::vector<const BranchReference*> branches;
  branches.reserve(function.branches.size());
  for (const auto& branch : function.branches) branches.push_back(&branch);
  std::sort(branches.begin(), branches.end(), [](const auto* a, const auto* b) {
    if (a->site != b->site) return a->site < b->site;
    if (a->linked != b->linked) return a->linked < b->linked;
    return a->target < b->target;
  });
  mix_value(hash, static_cast<std::uint32_t>(branches.size()));
  for (const auto* branch : branches) {
    const bool local = branch->target >= function.guest_start && branch->target < function.guest_end;
    mix_value(hash, static_cast<std::uint8_t>(branch->linked));
    mix_value(hash, static_cast<std::uint8_t>(branch->indirect));
    mix_value(hash, static_cast<std::uint8_t>(branch->conditional));
    mix_value(hash, static_cast<std::uint8_t>(branch->terminal));
    mix_value(hash, static_cast<std::uint8_t>(branch->fallthrough));
    mix_value(hash, static_cast<std::uint8_t>(local));
    if (branch->linked && !local) ++external_calls;
  }
  mix_value(hash, external_calls);
  mix_value(hash, static_cast<std::uint32_t>(function.calls.size()));
  mix_value(hash, static_cast<std::uint32_t>(function.callers.size()));
  return hash;
}

[[nodiscard]] std::string default_record_id(std::string_view image_hash,
                                            std::uint32_t address,
                                            const FunctionFingerprint& fp) {
  std::ostringstream out;
  out << "fnv1:";
  if (!image_hash.empty()) out << image_hash.substr(0u, std::min<std::size_t>(12u, image_hash.size())) << ':';
  out << std::hex << address << ':' << hex64(fp.instruction_shape_hash).substr(0u, 12u);
  return out.str();
}

[[nodiscard]] std::uint32_t bounded_u32(const core::JsonValue& object, std::string_view key,
                                        std::uint32_t fallback = 0u) noexcept {
  const auto value = object.get_number(key, static_cast<double>(fallback));
  if (value < 0.0) return fallback;
  return static_cast<std::uint32_t>(std::min<double>(value, std::numeric_limits<std::uint32_t>::max()));
}

}  // namespace

const char* knowledge_kind_name(KnowledgeKind kind) noexcept {
  switch (kind) {
    case KnowledgeKind::Function: return "function";
    case KnowledgeKind::CompilerHelper: return "compiler-helper";
    case KnowledgeKind::Crt: return "crt";
    case KnowledgeKind::RuntimeHelper: return "runtime-helper";
    case KnowledgeKind::Middleware: return "middleware";
    case KnowledgeKind::Engine: return "engine";
  }
  return "function";
}

std::optional<KnowledgeKind> parse_knowledge_kind(std::string_view text) noexcept {
  if (text == "function") return KnowledgeKind::Function;
  if (text == "compiler-helper") return KnowledgeKind::CompilerHelper;
  if (text == "crt") return KnowledgeKind::Crt;
  if (text == "runtime-helper") return KnowledgeKind::RuntimeHelper;
  if (text == "middleware") return KnowledgeKind::Middleware;
  if (text == "engine") return KnowledgeKind::Engine;
  return std::nullopt;
}

FunctionFingerprint fingerprint_entry_anchor(const xbox::XexImage& image, std::uint32_t address,
                                             std::uint32_t max_instructions) noexcept {
  FunctionFingerprint fingerprint{};
  const auto [anchor, count] = compute_anchor(image, address, max_instructions);
  fingerprint.entry_anchor_hash = anchor;
  fingerprint.entry_anchor_instructions = count;
  fingerprint.instruction_shape_hash = anchor;
  fingerprint.instruction_count = count;
  fingerprint.byte_size = count * 4u;
  return fingerprint;
}

std::vector<std::uint32_t> find_knowledge_seed_candidates(
    const xbox::XexImage& image, std::span<const KnowledgeRecord> records,
    std::uint32_t minimum_record_confidence) {
  std::array<std::unordered_set<std::uint64_t>, 9> anchors_by_length;
  std::uint32_t maximum_length = 0u;
  for (const auto& record : records) {
    const auto& fp = record.fingerprint;
    if (!fp.valid() || record.confidence < minimum_record_confidence ||
        fp.entry_anchor_instructions < 4u || fp.entry_anchor_instructions > 8u)
      continue;
    anchors_by_length[fp.entry_anchor_instructions].insert(fp.entry_anchor_hash);
    maximum_length = std::max(maximum_length, fp.entry_anchor_instructions);
  }
  if (maximum_length == 0u) return {};

  cpu::Decoder decoder;
  std::vector<std::uint32_t> candidates;
  for (const auto& section : image.sections) {
    if (!section.executable || section.bytes.size() < 16u) continue;
    for (std::size_t offset = 0; offset + 16u <= section.bytes.size(); offset += 4u) {
      const auto address64 = static_cast<std::uint64_t>(section.virtual_address) + offset;
      if (address64 > std::numeric_limits<std::uint32_t>::max()) break;
      const auto address = static_cast<std::uint32_t>(address64);

      std::uint64_t running = kFnvOffset;
      std::uint32_t count = 0u;
      bool matched = false;
      for (std::uint32_t index = 0u; index < maximum_length; ++index) {
        const auto word_offset = offset + static_cast<std::size_t>(index) * 4u;
        if (word_offset + 4u > section.bytes.size()) break;
        const auto word =
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[word_offset])) << 24u) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[word_offset + 1u])) << 16u) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[word_offset + 2u])) << 8u) |
            static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[word_offset + 3u]));
        const auto pc64 = address64 + static_cast<std::uint64_t>(index) * 4u;
        if (pc64 > std::numeric_limits<std::uint32_t>::max()) break;
        const auto instruction = decoder.decode(static_cast<std::uint32_t>(pc64), word);
        const auto token = normalize_instruction(instruction);
        mix_value(running, token.instruction);
        ++count;

        if (!anchors_by_length[count].empty()) {
          auto final_hash = running;
          mix_value(final_hash, count);
          if (anchors_by_length[count].contains(final_hash)) {
            matched = true;
            break;
          }
        }
        if (!instruction.valid() || token.terminator) break;
      }
      if (matched) candidates.push_back(address);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  return candidates;
}

FunctionFingerprint fingerprint_function(const xbox::XexImage& image,
                                         const DiscoveredFunction& function) noexcept {
  FunctionFingerprint fingerprint{};
  FingerprintAccumulator accumulator{};
  cpu::Decoder decoder;
  auto ranges = function_ranges(function);
  std::uint32_t decoded = 0u;
  for (const auto& [begin, end] : ranges) {
    // Explicit separator prevents two differently chunked functions with the
    // same concatenated words from becoming identical.
    mix_value(accumulator.instructions, 0x52414E47u);  // "RANG"
    mix_value(accumulator.constants, 0x52414E47u);
    for (std::uint64_t pc = begin; pc + 4u <= end && decoded < kMaxFingerprintInstructions; pc += 4u) {
      std::uint32_t word = 0u;
      if (!read_word(image, static_cast<std::uint32_t>(pc), word)) break;
      append_instruction(accumulator, decoder.decode(static_cast<std::uint32_t>(pc), word));
      ++decoded;
    }
  }
  if (accumulator.instruction_count == 0u) return fingerprint;
  mix_value(accumulator.instructions, accumulator.instruction_count);
  mix_value(accumulator.constants, accumulator.instruction_count);
  fingerprint.instruction_shape_hash = accumulator.instructions;
  fingerprint.constant_shape_hash = accumulator.constants;
  fingerprint.instruction_count = accumulator.instruction_count;
  fingerprint.byte_size = accumulator.byte_size;
  const auto [anchor, anchor_count] = compute_anchor(image, function.guest_start, kDefaultAnchorInstructions);
  fingerprint.entry_anchor_hash = anchor;
  fingerprint.entry_anchor_instructions = anchor_count;
  fingerprint.cfg_shape_hash = fingerprint_cfg(function);
  fingerprint.block_count = static_cast<std::uint32_t>(function.ir.blocks.size());
  fingerprint.call_neighborhood_hash = fingerprint_call_neighborhood(function, fingerprint.external_call_count);
  return fingerprint;
}

std::vector<KnowledgeMatch> match_knowledge(const FunctionFingerprint& fingerprint,
                                            std::span<const KnowledgeRecord> records,
                                            std::string_view current_image_hash,
                                            std::uint32_t minimum_score) {
  std::vector<KnowledgeMatch> matches;
  if (!fingerprint.valid()) return matches;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    const auto& candidate = record.fingerprint;
    if (!candidate.valid() || candidate.version != fingerprint.version) continue;

    KnowledgeMatch match{};
    match.record_index = index;
    match.instruction_shape = candidate.instruction_shape_hash == fingerprint.instruction_shape_hash &&
                              candidate.instruction_count == fingerprint.instruction_count;
    match.entry_anchor = candidate.entry_anchor_hash == fingerprint.entry_anchor_hash &&
                         candidate.entry_anchor_instructions == fingerprint.entry_anchor_instructions;
    match.cfg_shape = candidate.cfg_shape_hash == fingerprint.cfg_shape_hash &&
                      candidate.block_count == fingerprint.block_count;
    match.constants = candidate.constant_shape_hash == fingerprint.constant_shape_hash;
    match.call_neighborhood = candidate.call_neighborhood_hash == fingerprint.call_neighborhood_hash &&
                              candidate.external_call_count == fingerprint.external_call_count;
    match.cross_revision = !record.source_image_hash.empty() && !current_image_hash.empty() &&
                           record.source_image_hash != current_image_hash;

    std::uint32_t structural = 0u;
    if (match.instruction_shape) structural += 50u;
    if (match.entry_anchor) structural += 15u;
    if (match.cfg_shape) structural += 20u;
    if (match.constants) structural += 5u;
    if (match.call_neighborhood) structural += 10u;

    // A first-block collision alone is nomination evidence, never a full
    // function match. Full instruction shape is mandatory. Record confidence
    // then bounds how much authority stale/learned data can contribute.
    if (!match.instruction_shape) continue;
    const auto confidence = std::min<std::uint32_t>(100u, std::max<std::uint32_t>(1u, record.confidence));
    match.score = static_cast<std::uint32_t>((structural * confidence + 50u) / 100u);
    if (match.score >= minimum_score) matches.push_back(match);
  }
  std::sort(matches.begin(), matches.end(), [&](const auto& a, const auto& b) {
    if (a.score != b.score) return a.score > b.score;
    const auto& ar = records[a.record_index];
    const auto& br = records[b.record_index];
    if (ar.curated != br.curated) return ar.curated > br.curated;
    if (ar.observations != br.observations) return ar.observations > br.observations;
    return ar.id < br.id;
  });
  return matches;
}

std::uint64_t knowledge_base_fingerprint(std::span<const KnowledgeRecord> records) noexcept {
  struct Canonical {
    KnowledgeKind kind{};
    std::string family;
    std::string label;
    FunctionFingerprint fp{};
    std::uint32_t confidence{};
    bool curated{};
  };
  std::vector<Canonical> canonical;
  canonical.reserve(records.size());
  for (const auto& record : records) {
    if (!record.fingerprint.valid()) continue;
    canonical.push_back({record.kind, record.family, record.label, record.fingerprint,
                         record.confidence, record.curated});
  }
  const auto key = [](const Canonical& r) {
    return std::tuple{static_cast<std::uint8_t>(r.kind), r.family, r.label,
                      r.fp.instruction_shape_hash, r.fp.entry_anchor_hash,
                      r.fp.cfg_shape_hash, r.fp.constant_shape_hash,
                      r.fp.call_neighborhood_hash, r.confidence, r.curated};
  };
  std::sort(canonical.begin(), canonical.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
  canonical.erase(std::unique(canonical.begin(), canonical.end(), [&](const auto& a, const auto& b) {
                    return key(a) == key(b);
                  }), canonical.end());

  std::uint64_t hash = kFnvOffset;
  mix_value(hash, FunctionFingerprint::kVersion);
  mix_value(hash, static_cast<std::uint64_t>(canonical.size()));
  for (const auto& record : canonical) {
    mix_value(hash, record.kind);
    mix_string(hash, record.family);
    mix_string(hash, record.label);
    mix_value(hash, record.fp.instruction_shape_hash);
    mix_value(hash, record.fp.entry_anchor_hash);
    mix_value(hash, record.fp.cfg_shape_hash);
    mix_value(hash, record.fp.constant_shape_hash);
    mix_value(hash, record.fp.call_neighborhood_hash);
    mix_value(hash, record.fp.instruction_count);
    mix_value(hash, record.fp.entry_anchor_instructions);
    mix_value(hash, record.fp.block_count);
    mix_value(hash, record.fp.external_call_count);
    mix_value(hash, record.confidence);
    mix_value(hash, static_cast<std::uint8_t>(record.curated));
  }
  return canonical.empty() ? 0u : hash;
}

bool load_knowledge_base(const std::filesystem::path& path,
                         std::vector<KnowledgeRecord>& records,
                         std::string& error) {
  std::ifstream input(path);
  if (!input) {
    error = "unable to open knowledge base '" + path.string() + "'";
    return false;
  }
  constexpr std::size_t kMaxLines = 2'000'000u;
  constexpr std::size_t kMaxLineBytes = 128u * 1024u;
  std::string line;
  std::size_t line_number = 0u;
  std::vector<KnowledgeRecord> loaded;
  while (std::getline(input, line)) {
    ++line_number;
    if (line_number > kMaxLines) {
      error = "knowledge base exceeds safety limit of " + std::to_string(kMaxLines) + " records";
      return false;
    }
    if (line.size() > kMaxLineBytes) {
      error = "knowledge base line exceeds 128 KiB at " + path.string() + ":" + std::to_string(line_number);
      return false;
    }
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '#') continue;
    core::JsonValue json;
    std::string json_error;
    if (!core::JsonValue::parse(line, json, &json_error) || !json.is_object()) {
      error = "invalid knowledge record at " + path.string() + ":" + std::to_string(line_number) + ": " + json_error;
      return false;
    }
    if (bounded_u32(json, "schemaVersion") != 1u) {
      error = "unsupported knowledge schemaVersion at " + path.string() + ":" + std::to_string(line_number);
      return false;
    }
    const auto kind = parse_knowledge_kind(json.get_string("kind"));
    if (!kind) {
      error = "invalid knowledge kind at " + path.string() + ":" + std::to_string(line_number);
      return false;
    }
    KnowledgeRecord record{};
    record.id = json.get_string("id");
    record.kind = *kind;
    record.family = json.get_string("family");
    record.label = json.get_string("label");
    record.source_image_hash = json.get_string("sourceImageHash");
    record.source_address = bounded_u32(json, "sourceAddress");
    record.observations = std::max<std::uint32_t>(1u, bounded_u32(json, "observations", 1u));
    record.confidence = std::min<std::uint32_t>(100u, bounded_u32(json, "confidence", 50u));
    record.curated = json.get_bool("curated", false);
    auto& fp = record.fingerprint;
    fp.version = bounded_u32(json, "fingerprintVersion");
    fp.instruction_count = bounded_u32(json, "instructionCount");
    fp.entry_anchor_instructions = bounded_u32(json, "entryAnchorInstructions");
    fp.block_count = bounded_u32(json, "blockCount");
    fp.external_call_count = bounded_u32(json, "externalCallCount");
    fp.byte_size = bounded_u32(json, "byteSize");
    const std::array<std::pair<std::string_view, std::uint64_t*>, 5> hashes{{
        {"instructionShape", &fp.instruction_shape_hash},
        {"entryAnchor", &fp.entry_anchor_hash},
        {"cfgShape", &fp.cfg_shape_hash},
        {"constantShape", &fp.constant_shape_hash},
        {"callNeighborhood", &fp.call_neighborhood_hash},
    }};
    for (const auto& [key_name, target] : hashes) {
      if (!parse_hex64(json.get_string(key_name), *target)) {
        error = "invalid " + std::string(key_name) + " hash at " + path.string() + ":" + std::to_string(line_number);
        return false;
      }
    }
    if (!fp.valid()) {
      error = "invalid/incomplete fingerprint at " + path.string() + ":" + std::to_string(line_number);
      return false;
    }
    if (record.id.empty()) record.id = default_record_id(record.source_image_hash, record.source_address, fp);
    loaded.push_back(std::move(record));
  }
  records.insert(records.end(), loaded.begin(), loaded.end());
  return true;
}

bool save_knowledge_base(const std::filesystem::path& path,
                         std::span<const KnowledgeRecord> records,
                         std::string& error) {
  // Keep repeated learning runs compact without erasing revision provenance.
  // Exact semantic records from the same image/address collapse into one row;
  // observations accumulate and confidence can only increase. A moved copy in
  // another revision stays a separate source row so cross_revision remains
  // meaningful during later matching.
  std::vector<KnowledgeRecord> compact;
  compact.reserve(records.size());
  for (const auto& record : records)
    if (record.fingerprint.valid()) compact.push_back(record);
  const auto key = [](const KnowledgeRecord& record) {
    const auto& fp = record.fingerprint;
    return std::tuple{record.source_image_hash, record.source_address,
                      static_cast<std::uint8_t>(record.kind), record.family, record.label,
                      fp.version, fp.instruction_shape_hash, fp.entry_anchor_hash,
                      fp.cfg_shape_hash, fp.constant_shape_hash, fp.call_neighborhood_hash,
                      fp.instruction_count, fp.entry_anchor_instructions, fp.block_count,
                      fp.external_call_count, fp.byte_size};
  };
  std::sort(compact.begin(), compact.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
  std::vector<KnowledgeRecord> merged;
  merged.reserve(compact.size());
  for (auto& record : compact) {
    if (!merged.empty() && key(merged.back()) == key(record)) {
      auto& previous = merged.back();
      const auto total = static_cast<std::uint64_t>(previous.observations) +
                         static_cast<std::uint64_t>(std::max<std::uint32_t>(1u, record.observations));
      previous.observations = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
      previous.confidence = std::max(previous.confidence, record.confidence);
      previous.curated = previous.curated || record.curated;
      if (previous.id.empty()) previous.id = record.id;
      continue;
    }
    record.observations = std::max<std::uint32_t>(1u, record.observations);
    merged.push_back(std::move(record));
  }

  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    error = "unable to write knowledge base '" + path.string() + "'";
    return false;
  }
  for (const auto& record : merged) {
    const auto& fp = record.fingerprint;
    core::JsonValue json = core::JsonValue::make_object();
    json.set("schemaVersion", 1)
        .set("id", record.id)
        .set("kind", knowledge_kind_name(record.kind))
        .set("family", record.family)
        .set("label", record.label)
        .set("sourceImageHash", record.source_image_hash)
        .set("sourceAddress", record.source_address)
        .set("observations", record.observations)
        .set("confidence", record.confidence)
        .set("curated", record.curated)
        .set("fingerprintVersion", fp.version)
        .set("instructionShape", hex64(fp.instruction_shape_hash))
        .set("entryAnchor", hex64(fp.entry_anchor_hash))
        .set("cfgShape", hex64(fp.cfg_shape_hash))
        .set("constantShape", hex64(fp.constant_shape_hash))
        .set("callNeighborhood", hex64(fp.call_neighborhood_hash))
        .set("instructionCount", fp.instruction_count)
        .set("entryAnchorInstructions", fp.entry_anchor_instructions)
        .set("blockCount", fp.block_count)
        .set("externalCallCount", fp.external_call_count)
        .set("byteSize", fp.byte_size);
    output << json.dump() << '\n';
    if (!output) {
      error = "failed while writing knowledge base '" + path.string() + "'";
      return false;
    }
  }
  return true;
}

std::vector<KnowledgeRecord> export_analysis_knowledge(const AnalysisReport& report,
                                                       std::string_view source_image_hash) {
  std::vector<KnowledgeRecord> records;
  records.reserve(report.functions.size());
  for (const auto& function : report.functions) {
    if (!function.compiled || function.native_replacement.has_value()) continue;
    const auto fp = fingerprint_function(report.image, function);
    if (!fp.valid()) continue;
    KnowledgeRecord record{};
    record.fingerprint = fp;
    record.source_image_hash = std::string(source_image_hash);
    record.source_address = function.guest_start;
    record.confidence = std::min<std::uint32_t>(100u, function.confidence);
    record.observations = 1u;
    record.kind = KnowledgeKind::Function;
    if (!function.name.starts_with("xenon_fn_")) record.label = function.name;
    record.id = default_record_id(source_image_hash, function.guest_start, fp);
    records.push_back(std::move(record));
  }
  return records;
}

}  // namespace xenon::recomp
