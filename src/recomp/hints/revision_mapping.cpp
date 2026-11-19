#include "xenon/recomp/revision_mapping.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace xenon::recomp {

namespace {

using CodeStream = RevisionAddressMap::Code;

CodeStream code_stream(const xbox::XexImage& image) {
  CodeStream stream;
  std::vector<const xbox::XexSection*> sections;
  for (const auto& section : image.sections)
    if (section.executable) sections.push_back(&section);
  std::sort(sections.begin(), sections.end(),
            [](const auto* a, const auto* b) { return a->virtual_address < b->virtual_address; });
  for (const auto* section_ptr : sections) {
    const auto& section = *section_ptr;
    const auto words = std::min<std::size_t>(section.bytes.size(), section.virtual_size) / 4u;
    for (std::size_t i = 0; i < words; ++i) {
      const auto* b = &section.bytes[i * 4u];
      const auto word = (std::to_integer<std::uint32_t>(b[0]) << 24) |
                        (std::to_integer<std::uint32_t>(b[1]) << 16) |
                        (std::to_integer<std::uint32_t>(b[2]) << 8) |
                        std::to_integer<std::uint32_t>(b[3]);
      stream.raw.push_back(word);
      stream.normalized.push_back(relocation_insensitive_word(word));
      stream.address.push_back(static_cast<std::uint32_t>(section.virtual_address + i * 4u));
    }
  }
  return stream;
}

std::uint64_t window_hash(const std::vector<std::uint32_t>& words, std::size_t at, std::size_t length) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (std::size_t i = 0; i < length; ++i) {
    hash ^= words[at + i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// A padding or all-zero window anchors nothing meaningful.
bool informative(const std::vector<std::uint32_t>& words, std::size_t at, std::size_t length) {
  for (std::size_t i = 1; i < length; ++i)
    if (words[at + i] != words[at]) return true;
  return false;
}

std::optional<std::size_t> index_of(const CodeStream& code, std::uint32_t address) {
  const auto it = std::lower_bound(code.address.begin(), code.address.end(), address);
  if (it == code.address.end() || *it != address) return std::nullopt;
  return static_cast<std::size_t>(it - code.address.begin());
}

// The destination of a direct b/bl or bc at `address`.
std::optional<std::uint32_t> direct_branch_target(std::uint32_t word, std::uint32_t address) {
  const auto opcode = word >> 26u;
  std::int32_t displacement;
  if (opcode == 18u) {
    displacement = static_cast<std::int32_t>(word & 0x03FFFFFCu);
    if (displacement & 0x02000000) displacement -= 0x04000000;
  } else if (opcode == 16u) {
    displacement = static_cast<std::int32_t>(word & 0xFFFCu);
    if (displacement & 0x8000) displacement -= 0x10000;
  } else {
    return std::nullopt;
  }
  const bool absolute = (word & 2u) != 0u;
  return static_cast<std::uint32_t>((absolute ? 0u : address) + static_cast<std::uint32_t>(displacement));
}

// blr, bctr or an unlinked b: control never falls past it.
bool ends_function_body(std::uint32_t word) {
  return word == 0x4E800020u || word == 0x4E800420u || ((word >> 26u) == 18u && (word & 1u) == 0u);
}

// A function body compared strictly: only immediates that can be
// relocations are masked - branch displacements, lis/addis address halves,
// and the low half used through a register a lis set or through r2/r13
// (small-data bases). Struct offsets and constants must match exactly, so
// sibling helpers differing only in a field offset stay apart.
std::vector<std::uint32_t> body_keys(const std::vector<std::uint32_t>& raw, std::size_t start,
                                     std::size_t length) {
  std::uint32_t address_registers = (1u << 2u) | (1u << 13u);
  std::vector<std::uint32_t> keys;
  keys.reserve(length);
  for (std::size_t k = 0; k < length; ++k) {
    const auto word = raw[start + k];
    const auto opcode = word >> 26u;
    const auto rt = (word >> 21u) & 31u;
    const auto ra = (word >> 16u) & 31u;
    const auto uses = [&](std::uint32_t reg) { return ((address_registers >> reg) & 1u) != 0u; };
    if (opcode == 18u || opcode == 16u) {
      keys.push_back(relocation_insensitive_word(word));
    } else if (opcode == 15u && ra == 0u) {  // lis
      keys.push_back(word & 0xFFFF0000u);
      address_registers |= 1u << rt;
    } else if ((opcode == 24u || opcode == 25u) && uses(rt)) {  // ori/oris rA, rS
      keys.push_back(word & 0xFFFF0000u);
      address_registers |= 1u << ra;
    } else if ((opcode == 14u || opcode == 15u) && ra != 0u && uses(ra)) {  // addi/addis rD, rA
      keys.push_back(word & 0xFFFF0000u);
      address_registers |= 1u << rt;
    } else if (((opcode >= 32u && opcode <= 55u) || opcode == 58u || opcode == 62u) && uses(ra)) {
      keys.push_back(relocation_insensitive_word(word));
    } else {
      keys.push_back(word);
    }
  }
  return keys;
}

struct Run {
  std::size_t source{};  // word index
  std::size_t target{};
  std::size_t words{};
};

std::string hex(std::uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
  return out.str();
}

}  // namespace

std::uint32_t relocation_insensitive_word(std::uint32_t word) noexcept {
  const auto opcode = word >> 26u;
  switch (opcode) {
    case 18:  // b/bl: LI
      return word & 0xFC000003u;
    case 16:  // bc: BD
      return word & 0xFFFF0003u;
    case 14: case 15:  // addi/addis (li/lis)
    case 24: case 25:  // ori/oris
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
      return word & 0xFFFF0000u;
    case 58: case 62:  // ld/std families: DS plus the two-bit form selector
      return word & 0xFFFF0003u;
    default:
      return word;
  }
}

RevisionAddressMap RevisionAddressMap::build(const xbox::XexImage& source, const xbox::XexImage& target,
                                             std::size_t anchor_words) {
  RevisionAddressMap map;
  map.source_ = code_stream(source);
  map.target_ = code_stream(target);
  const auto& src = map.source_;
  const auto& dst = map.target_;
  map.source_words_ = src.normalized.size();
  if (anchor_words == 0u || src.normalized.size() < anchor_words || dst.normalized.size() < anchor_words)
    return map;

  // Windows unique to one position in each revision.
  struct Occurrence {
    std::uint32_t source_count{}, target_count{};
    std::size_t source{}, target{};
  };
  std::unordered_map<std::uint64_t, Occurrence> windows;
  windows.reserve(src.normalized.size() + dst.normalized.size());
  for (std::size_t i = 0; i + anchor_words <= src.normalized.size(); ++i) {
    if (!informative(src.normalized, i, anchor_words)) continue;
    auto& o = windows[window_hash(src.normalized, i, anchor_words)];
    ++o.source_count;
    o.source = i;
  }
  for (std::size_t i = 0; i + anchor_words <= dst.normalized.size(); ++i) {
    if (!informative(dst.normalized, i, anchor_words)) continue;
    const auto it = windows.find(window_hash(dst.normalized, i, anchor_words));
    if (it == windows.end()) continue;
    ++it->second.target_count;
    it->second.target = i;
  }
  std::vector<std::pair<std::size_t, std::size_t>> anchors;
  for (const auto& [hash, o] : windows)
    if (o.source_count == 1u && o.target_count == 1u &&
        std::equal(src.normalized.begin() + static_cast<std::ptrdiff_t>(o.source),
                   src.normalized.begin() + static_cast<std::ptrdiff_t>(o.source + anchor_words),
                   dst.normalized.begin() + static_cast<std::ptrdiff_t>(o.target)))
      anchors.emplace_back(o.source, o.target);
  windows.clear();
  std::sort(anchors.begin(), anchors.end());

  // Consecutive anchors at one offset form a run covering their windows.
  std::vector<Run> runs;
  for (const auto& [s, t] : anchors) {
    if (!runs.empty()) {
      auto& last = runs.back();
      if (s - last.source == t - last.target && s <= last.source + last.words) {
        last.words = std::max(last.words, s + anchor_words - last.source);
        continue;
      }
    }
    runs.push_back({s, t, anchor_words});
  }

  // Longest runs claim their ranges first; a run overlapping a claimed range
  // in either revision is a weaker, conflicting alignment and is dropped.
  std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
    return a.words != b.words ? a.words > b.words : a.source < b.source;
  });
  std::vector<std::uint8_t> source_claimed(src.normalized.size()), target_claimed(dst.normalized.size());
  std::vector<Run> chosen;
  for (const auto& run : runs) {
    const auto free = [&](const std::vector<std::uint8_t>& claimed, std::size_t at) {
      return std::none_of(claimed.begin() + static_cast<std::ptrdiff_t>(at),
                          claimed.begin() + static_cast<std::ptrdiff_t>(at + run.words),
                          [](std::uint8_t c) { return c != 0u; });
    };
    if (!free(source_claimed, run.source) || !free(target_claimed, run.target)) continue;
    std::fill_n(source_claimed.begin() + static_cast<std::ptrdiff_t>(run.source), run.words, std::uint8_t{1});
    std::fill_n(target_claimed.begin() + static_cast<std::ptrdiff_t>(run.target), run.words, std::uint8_t{1});
    chosen.push_back(run);
  }

  // Extend each run over equal unclaimed neighbours, then split it wherever
  // the guest addresses stop being contiguous (a section boundary).
  for (auto& run : chosen) {
    while (run.source > 0 && run.target > 0 && !source_claimed[run.source - 1] &&
           !target_claimed[run.target - 1] &&
           src.normalized[run.source - 1] == dst.normalized[run.target - 1]) {
      --run.source;
      --run.target;
      ++run.words;
      source_claimed[run.source] = target_claimed[run.target] = 1u;
    }
    while (run.source + run.words < src.normalized.size() &&
           run.target + run.words < dst.normalized.size() &&
           !source_claimed[run.source + run.words] && !target_claimed[run.target + run.words] &&
           src.normalized[run.source + run.words] == dst.normalized[run.target + run.words]) {
      source_claimed[run.source + run.words] = target_claimed[run.target + run.words] = 1u;
      ++run.words;
    }
  }
  const auto emit_segments = [&] {
    map.segments_.clear();
    for (const auto& run : chosen) {
      std::size_t start = 0;
      for (std::size_t i = 1; i <= run.words; ++i) {
        const bool split = i == run.words ||
                           src.address[run.source + i] != src.address[run.source + i - 1] + 4u ||
                           dst.address[run.target + i] != dst.address[run.target + i - 1] + 4u;
        if (!split) continue;
        map.segments_.push_back({src.address[run.source + start], dst.address[run.target + start],
                                 static_cast<std::uint32_t>(i - start)});
        start = i;
      }
    }
    std::sort(map.segments_.begin(), map.segments_.end(),
              [](const Segment& a, const Segment& b) { return a.source < b.source; });
  };
  emit_segments();

  // Matching words equal after masking can pair look-alike neighbours a
  // revision reordered (two thunks branching to different functions) - by
  // extension, or even inside an anchor window that reaches into them. A
  // branch whose destination the map sends somewhere other than where the
  // paired target branch goes exposes it. Either the branch's own function
  // or its destination's function is misaligned. The one whose own branches
  // conflict the most (as a share of its branches) loses its alignment - a
  // function with no branches of its own, such as a register save ladder,
  // cannot be the inconsistent side - and between equals the shorter one,
  // since misplaced look-alikes are short thunks. Repeat until the map is
  // consistent.
  const auto function_around = [&](std::size_t index) {
    constexpr std::size_t kScan = 4096u;
    auto begin = index;
    while (begin > 0 && index - begin < kScan && !ends_function_body(src.raw[begin - 1])) --begin;
    auto end = index;
    while (end < src.raw.size() && end - index < kScan && !ends_function_body(src.raw[end])) ++end;
    return std::pair{begin, std::min(end + 1, src.raw.size())};
  };
  for (int round = 0; round < 16; ++round) {
    using Range = std::pair<std::size_t, std::size_t>;
    struct Conflict {
      Range here, there;
    };
    std::vector<Conflict> found;
    std::unordered_map<std::size_t, std::size_t> conflicts_in;  // by function start
    for (const auto& run : chosen) {
      for (auto s = run.source; s < run.source + run.words; ++s) {
        const auto destination = direct_branch_target(src.raw[s], src.address[s]);
        if (!destination) continue;
        const auto mapped = map.translate(*destination);
        const auto t = run.target + (s - run.source);
        if (!mapped || direct_branch_target(dst.raw[t], dst.address[t]) == mapped) continue;
        const auto here = function_around(s);
        const auto there_index = index_of(src, *destination);
        found.push_back({here, there_index ? function_around(*there_index) : here});
        ++conflicts_in[here.first];
      }
    }
    if (found.empty()) break;
    const auto conflict_share = [&](const Range& f) {
      std::size_t branches = 0;
      for (auto i = f.first; i < f.second; ++i)
        branches += direct_branch_target(src.raw[i], src.address[i]).has_value() ? 1u : 0u;
      const auto it = conflicts_in.find(f.first);
      return branches == 0 || it == conflicts_in.end() ? 0.0
                                                       : static_cast<double>(it->second) / branches;
    };
    std::vector<Range> drop;
    for (const auto& c : found) {
      const auto here_share = conflict_share(c.here), there_share = conflict_share(c.there);
      if (here_share != there_share) drop.push_back(here_share > there_share ? c.here : c.there);
      else drop.push_back(c.here.second - c.here.first <= c.there.second - c.there.first ? c.here : c.there);
    }
    std::vector<Run> kept;
    for (const auto& run : chosen) {
      std::vector<std::uint8_t> removed(run.words);
      for (const auto& [begin, end] : drop)
        for (auto s = std::max(begin, run.source); s < std::min(end, run.source + run.words); ++s)
          removed[s - run.source] = 1u;
      for (std::size_t i = 0; i < run.words;) {
        if (removed[i]) { ++i; continue; }
        auto j = i;
        while (j < run.words && !removed[j]) ++j;
        kept.push_back({run.source + i, run.target + i, j - i});
        i = j;
      }
    }
    chosen = std::move(kept);
    emit_segments();
  }
  return map;
}

std::optional<std::uint32_t> RevisionAddressMap::translate(std::uint32_t source_address) const {
  if ((source_address & 3u) != 0u) return std::nullopt;
  auto it = std::upper_bound(segments_.begin(), segments_.end(), source_address,
                             [](std::uint32_t address, const Segment& s) { return address < s.source; });
  if (it == segments_.begin()) return std::nullopt;
  --it;
  const auto offset = source_address - it->source;
  if (offset >= it->words * 4ull) return std::nullopt;
  return it->target + offset;
}

std::optional<std::uint32_t> RevisionAddressMap::translate_entry(std::uint32_t source_address) const {
  if (const auto aligned = translate(source_address)) return aligned;
  const auto index = index_of(source_, source_address);
  if (!index) return std::nullopt;
  if (const auto by_callers = translate_by_callers(*index)) return by_callers;
  return translate_by_body(*index);
}

std::optional<std::uint32_t> RevisionAddressMap::translate_by_callers(std::size_t source_index) const {
  const auto entry = source_.address[source_index];
  std::optional<std::uint32_t> agreed;
  for (std::size_t i = 0; i < source_.raw.size(); ++i) {
    const auto destination = direct_branch_target(source_.raw[i], source_.address[i]);
    if (!destination || *destination != entry) continue;
    const auto site = translate(source_.address[i]);
    if (!site) continue;
    const auto site_index = index_of(target_, *site);
    if (!site_index) continue;
    const auto landed = direct_branch_target(target_.raw[*site_index], *site);
    if (!landed) return std::nullopt;
    if (agreed && *agreed != *landed) return std::nullopt;  // callers disagree
    agreed = landed;
  }
  if (!agreed) return std::nullopt;
  const auto target_index = index_of(target_, *agreed);
  if (!target_index || target_.normalized[*target_index] != source_.normalized[source_index])
    return std::nullopt;
  return agreed;
}

std::optional<std::uint32_t> RevisionAddressMap::translate_by_body(std::size_t source_index) const {
  constexpr std::size_t kMaxBody = 64u;
  const auto begin = source_.address[source_index];
  // The body runs to a blr/bctr/b that no earlier forward branch jumps past.
  std::size_t length = 0;
  std::uint32_t furthest = begin;
  while (length < kMaxBody && source_index + length < source_.raw.size()) {
    const auto word = source_.raw[source_index + length];
    const auto address = source_.address[source_index + length++];
    if (const auto destination = direct_branch_target(word, address);
        destination && *destination > address && *destination - begin < kMaxBody * 4u)
      furthest = std::max(furthest, *destination);
    if (ends_function_body(word) && furthest <= address) break;
  }
  const auto end = begin + static_cast<std::uint32_t>(length * 4u);
  // Branches leaving the body must each have an aligned destination.
  struct Outbound {
    std::size_t offset;
    std::uint32_t destination;
  };
  std::vector<Outbound> outbound;
  for (std::size_t k = 0; k < length; ++k) {
    const auto destination =
        direct_branch_target(source_.raw[source_index + k], source_.address[source_index + k]);
    if (!destination || (*destination >= begin && *destination < end)) continue;
    const auto mapped = translate(*destination);
    if (!mapped) return std::nullopt;
    outbound.push_back({k, *mapped});
  }
  if (outbound.empty()) return std::nullopt;  // nothing to tell repeats apart

  // Where the neighbouring aligned code puts the entry: between the target of
  // the segment ending before it and the segment starting after it. Settles
  // look-alike siblings (stubs differing only in a masked immediate).
  std::uint32_t gap_begin = 0, gap_end = 0xFFFFFFFFu;
  const auto next = std::upper_bound(segments_.begin(), segments_.end(), begin,
                                     [](std::uint32_t address, const Segment& s) { return address < s.source; });
  if (next != segments_.end()) gap_end = next->target;
  if (next != segments_.begin()) gap_begin = std::prev(next)->target + std::prev(next)->words * 4u;

  const auto source_keys = body_keys(source_.raw, source_index, length);
  std::vector<std::uint32_t> candidates, strict;
  for (std::size_t j = 0; j + length <= target_.raw.size(); ++j) {
    if (target_.normalized[j] != source_.normalized[source_index]) continue;
    bool match = true;
    for (std::size_t k = 1; k < length && match; ++k)
      match = target_.normalized[j + k] == source_.normalized[source_index + k] &&
              target_.address[j + k] == target_.address[j] + k * 4u;
    for (std::size_t n = 0; n < outbound.size() && match; ++n) {
      const auto at = j + outbound[n].offset;
      match = direct_branch_target(target_.raw[at], target_.address[at]) == outbound[n].destination;
    }
    if (match) {
      candidates.push_back(target_.address[j]);
      if (body_keys(target_.raw, j, length) == source_keys) strict.push_back(target_.address[j]);
    }
  }
  // Exact constants settle siblings that differ only in a field offset; a
  // revision that changed a class layout still matches on shape and
  // branches alone.
  if (strict.size() == 1u) return strict.front();
  if (candidates.size() == 1u) return candidates.front();
  std::optional<std::uint32_t> in_gap;
  for (const auto candidate : candidates) {
    if (candidate < gap_begin || candidate >= gap_end) continue;
    if (in_gap) return std::nullopt;  // more than one place fits
    in_gap = candidate;
  }
  return in_gap;
}

std::size_t RevisionAddressMap::mapped_words() const noexcept {
  std::size_t total = 0;
  for (const auto& segment : segments_) total += segment.words;
  return total;
}

analysis::AnalysisHintSetV2 port_hint_set(const analysis::AnalysisHintSetV2& source,
                                          const RevisionAddressMap& map,
                                          const xbox::XexEffectiveIdentity& target_identity,
                                          HintPortReport& report) {
  analysis::AnalysisHintSetV2 out;
  out.schema_version = source.schema_version;
  out.module_name = source.module_name;
  out.identity = target_identity;
  out.instruction_patterns = source.instruction_patterns;

  // Translates every address of one hint, or reports the first that fails.
  struct Batch {
    const RevisionAddressMap& map;
    HintPortReport& report;
    const char* category;
    std::uint32_t key;
    bool ok{true};
    std::uint32_t operator()(std::uint32_t address) {
      if (!ok) return 0;
      if (const auto mapped = map.translate(address)) return *mapped;
      return fail(address);
    }
    // A function entry: the alignment, or failing that its references.
    std::uint32_t entry(std::uint32_t address) {
      if (!ok) return 0;
      if (const auto mapped = map.translate(address)) return *mapped;
      if (const auto mapped = map.translate_entry(address)) {
        ++report.by_reference;
        return *mapped;
      }
      return fail(address);
    }
    std::uint32_t fail(std::uint32_t address) {
      ok = false;
      report.unmapped.push_back(std::string(category) + " " + hex(key) +
                                (address == key ? std::string() : " (field " + hex(address) + ")"));
      return 0;
    }
    // An exclusive end is the instruction before it, plus one.
    std::uint32_t end(std::uint32_t exclusive_end) { return (*this)(exclusive_end - 4u) + 4u; }
    bool done() {
      if (ok) ++report.mapped;
      return ok;
    }
  };

  for (const auto& hint : source.functions) {
    Batch t{map, report, "function", hint.address};
    auto ported = hint;
    ported.address = t.entry(hint.address);
    if (hint.end) ported.end = t.end(*hint.end);
    if (hint.size) ported.size = t.end(hint.address + *hint.size) - ported.address;
    if (hint.parent_function) ported.parent_function = t.entry(*hint.parent_function);
    if (t.done()) out.functions.push_back(ported);
  }
  for (const auto& chunk : source.chunks) {
    Batch t{map, report, "chunk", chunk.start};
    analysis::FunctionChunk ported{t(chunk.start), t.end(chunk.end), t.entry(chunk.parent_function)};
    if (t.done()) out.chunks.push_back(ported);
  }
  for (const auto& table : source.switches) {
    Batch t{map, report, "switch", table.site};
    auto ported = table;
    ported.site = t(table.site);
    if (table.table_address) {
      // The table itself is data; its location cannot come from code.
      t.ok = false;
      report.unmapped.push_back("switch " + hex(table.site) + " (table " + hex(*table.table_address) +
                                " is data)");
    }
    for (auto& target : ported.explicit_targets) target = t(target);
    if (t.done()) out.switches.push_back(ported);
  }
  for (const auto& call : source.indirect_calls) {
    Batch t{map, report, "indirect-call", call.callsite};
    auto ported = call;
    ported.callsite = t(call.callsite);
    for (auto& target : ported.targets) target = t.entry(target);
    if (t.done()) out.indirect_calls.push_back(ported);
  }
  for (const auto& branch : source.indirect_branches) {
    Batch t{map, report, "indirect-branch", branch.site};
    auto ported = branch;
    ported.site = t(branch.site);
    for (auto& target : ported.targets) target = t(target);
    if (t.done()) out.indirect_branches.push_back(ported);
  }
  for (const auto& replacement : source.native_replacements) {
    Batch t{map, report, "native-replacement", replacement.guest_address};
    auto ported = replacement;
    ported.guest_address = t.entry(replacement.guest_address);
    if (t.done()) out.native_replacements.push_back(ported);
  }
  for (const auto& helper : source.runtime_helpers) {
    Batch t{map, report, "runtime-helper", helper.address};
    auto ported = helper;
    ported.address = t.entry(helper.address);
    if (t.done()) out.runtime_helpers.push_back(ported);
  }
  for (auto& pattern : out.instruction_patterns) {
    if (!pattern.scope_start && !pattern.scope_end) continue;
    Batch t{map, report, "instruction-pattern-scope", pattern.scope_start.value_or(0)};
    if (pattern.scope_start) pattern.scope_start = t(*pattern.scope_start);
    if (pattern.scope_end) pattern.scope_end = t.end(*pattern.scope_end);
    if (!t.done()) pattern.scope_start = pattern.scope_end = std::nullopt;
  }
  for (const auto& region : source.regions) {
    Batch t{map, report, "region", region.start};
    auto ported = region;
    ported.start = t(region.start);
    ported.end = t.end(region.end);
    if (t.done()) out.regions.push_back(ported);
  }
  for (const auto& symbol : source.symbols) {
    Batch t{map, report, "symbol", symbol.address};
    auto ported = symbol;
    ported.address = t(symbol.address);
    if (t.done()) out.symbols.push_back(ported);
  }
  for (const auto& patch : source.patches) {
    Batch t{map, report, "patch", patch.address};
    auto ported = patch;
    ported.address = t(patch.address);
    // Every patched word must sit in the same aligned run.
    const auto bytes = static_cast<std::uint32_t>(patch.patch_bytes_hex.size() / 2u);
    if (t.ok && bytes > 4u && t(patch.address + ((bytes - 1u) & ~3u)) - ported.address != ((bytes - 1u) & ~3u)) {
      t.ok = false;
      report.unmapped.push_back("patch " + hex(patch.address) + " (spans an alignment break)");
    }
    if (t.done()) out.patches.push_back(ported);
  }
  for (const auto& hook : source.hooks) {
    Batch t{map, report, "hook", hook.address};
    auto ported = hook;
    ported.address = t(hook.address);
    if (hook.on_true == analysis::HookAction::Jump) ported.jump_on_true = t(hook.jump_on_true);
    if (hook.on_false == analysis::HookAction::Jump) ported.jump_on_false = t(hook.jump_on_false);
    if (t.done()) out.hooks.push_back(ported);
  }
  return out;
}

}  // namespace xenon::recomp
