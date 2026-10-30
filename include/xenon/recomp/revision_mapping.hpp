#pragma once

// Carries a module's analysis hints from one executable revision of a title
// to another (another region's disc, a later build). The two revisions'
// code is aligned instruction by instruction, ignoring the fields linking
// changes (branch displacements, address immediates), and every hint address
// is translated through that alignment. A hint whose address falls outside
// the aligned code is reported, never guessed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

// One PowerPC instruction with the bits that move when code or data is
// relinked cleared: the displacement of b/bc and the 16-bit immediate of
// D-form arithmetic, logical, load and store instructions (address halves
// and small-data offsets).
[[nodiscard]] std::uint32_t relocation_insensitive_word(std::uint32_t word) noexcept;

class RevisionAddressMap {
 public:
  // A run of instructions equal in both revisions.
  struct Segment {
    std::uint32_t source{};  // guest address of the first instruction
    std::uint32_t target{};
    std::uint32_t words{};
  };

  // Aligns the executable sections of `source` and `target`. Anchors are
  // windows of `anchor_words` instructions that occur exactly once in each
  // revision; runs of anchors with a constant offset become segments (longest
  // first, so moved code still maps), then extend over equal neighbours.
  [[nodiscard]] static RevisionAddressMap build(const xbox::XexImage& source,
                                                const xbox::XexImage& target,
                                                std::size_t anchor_words = 8u);

  // The target address of the instruction at `source_address`, when it lies
  // in an aligned segment.
  [[nodiscard]] std::optional<std::uint32_t> translate(std::uint32_t source_address) const;

  // translate(), or for a function entry outside the aligned code (a short
  // thunk repeated across the image has no unique window) the one target
  // address its references agree on: every aligned direct branch to it must
  // land on that one address, or else its body must occur exactly once in
  // the target with each outgoing branch landing where the alignment maps
  // the source's.
  [[nodiscard]] std::optional<std::uint32_t> translate_entry(std::uint32_t source_address) const;

  [[nodiscard]] const std::vector<Segment>& segments() const noexcept { return segments_; }
  [[nodiscard]] std::size_t source_words() const noexcept { return source_words_; }
  [[nodiscard]] std::size_t mapped_words() const noexcept;

  // One revision's executable code as a word stream, by ascending address.
  struct Code {
    std::vector<std::uint32_t> raw;
    std::vector<std::uint32_t> normalized;
    std::vector<std::uint32_t> address;
  };

 private:
  [[nodiscard]] std::optional<std::uint32_t> translate_by_callers(std::size_t source_index) const;
  [[nodiscard]] std::optional<std::uint32_t> translate_by_body(std::size_t source_index) const;

  std::vector<Segment> segments_;  // sorted by source, non-overlapping
  std::size_t source_words_{};
  Code source_;
  Code target_;
};

struct HintPortReport {
  std::size_t mapped{};
  // Of those, entries placed by their references rather than the alignment.
  std::size_t by_reference{};
  // "category 0xADDRESS" for each hint (or hint field) left behind.
  std::vector<std::string> unmapped;
};

// `source` translated to the target revision, stamped with
// `target_identity`. Hints whose addresses do not all translate are dropped
// and listed in the report. Data addresses (switch tables, data regions and
// symbols outside code) never translate through a code alignment.
[[nodiscard]] analysis::AnalysisHintSetV2 port_hint_set(
    const analysis::AnalysisHintSetV2& source, const RevisionAddressMap& map,
    const xbox::XexEffectiveIdentity& target_identity, HintPortReport& report);

}  // namespace xenon::recomp
