#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

struct DiscoveredFunction;
struct AnalysisReport;

// Gen 9 knowledge records are deliberately semantic/category-neutral. Xenon
// can learn ordinary game functions, curated compiler/CRT helpers, SDK/runtime
// helpers and middleware/engine routines through the same versioned format.
enum class KnowledgeKind : std::uint8_t {
  Function,
  CompilerHelper,
  Crt,
  RuntimeHelper,
  Middleware,
  Engine,
};

[[nodiscard]] const char* knowledge_kind_name(KnowledgeKind kind) noexcept;
[[nodiscard]] std::optional<KnowledgeKind> parse_knowledge_kind(std::string_view text) noexcept;

// Address-independent function identity. No single hash is authoritative:
// matching scores independent dimensions and only exact-enough combinations
// are allowed to become analysis evidence.
struct FunctionFingerprint {
  static constexpr std::uint32_t kVersion = 1u;

  std::uint32_t version{kVersion};
  std::uint64_t instruction_shape_hash{};
  std::uint64_t entry_anchor_hash{};
  std::uint64_t cfg_shape_hash{};
  std::uint64_t constant_shape_hash{};
  std::uint64_t call_neighborhood_hash{};
  std::uint32_t instruction_count{};
  std::uint32_t entry_anchor_instructions{};
  std::uint32_t block_count{};
  std::uint32_t external_call_count{};
  std::uint32_t byte_size{};

  [[nodiscard]] bool valid() const noexcept {
    return version == kVersion && instruction_shape_hash != 0u &&
           entry_anchor_hash != 0u && instruction_count != 0u;
  }
};

struct KnowledgeRecord {
  std::string id;
  KnowledgeKind kind{KnowledgeKind::Function};
  std::string family;
  std::string label;
  FunctionFingerprint fingerprint;

  // Provenance is retained so cross-revision reuse is explicit instead of
  // silently treating stale profile data as current truth.
  std::string source_image_hash;
  std::uint32_t source_address{};
  std::uint32_t observations{1u};
  std::uint32_t confidence{50u};
  bool curated{};
};

struct KnowledgeMatch {
  std::size_t record_index{};
  std::uint32_t score{};
  bool instruction_shape{};
  bool entry_anchor{};
  bool cfg_shape{};
  bool constants{};
  bool call_neighborhood{};
  bool cross_revision{};
};

struct KnowledgeMatchReport {
  std::uint32_t address{};
  std::string record_id;
  std::string label;
  std::string family;
  KnowledgeKind kind{KnowledgeKind::Function};
  std::uint32_t score{};
  bool cross_revision{};
  bool accepted{};
};

// Builds a versioned normalized fingerprint from the final discovered
// function. Relocated/direct target addresses are deliberately not part of
// instruction_shape_hash; CFG topology and call neighbourhood are separate
// dimensions so they can corroborate (or reject) a stale match.
[[nodiscard]] FunctionFingerprint fingerprint_function(
    const xbox::XexImage& image, const DiscoveredFunction& function) noexcept;

// Cheap first-block anchor used only to nominate possible moved functions.
// It is never sufficient by itself to label or trust a function.
[[nodiscard]] FunctionFingerprint fingerprint_entry_anchor(
    const xbox::XexImage& image, std::uint32_t address,
    std::uint32_t max_instructions = 8u) noexcept;

// Efficiently scans executable sections for first-block anchor matches. The
// scanner decodes at most max(required anchor length) instructions per guest
// address and derives all requested prefix hashes from that one pass, avoiding
// O(anchor-count * anchor-length) rescans when a large universal DB is loaded.
// Results are nomination candidates only; callers must still perform full
// function fingerprint matching before accepting knowledge as evidence.
[[nodiscard]] std::vector<std::uint32_t> find_knowledge_seed_candidates(
    const xbox::XexImage& image, std::span<const KnowledgeRecord> records,
    std::uint32_t minimum_record_confidence = 70u);

[[nodiscard]] std::vector<KnowledgeMatch> match_knowledge(
    const FunctionFingerprint& fingerprint,
    std::span<const KnowledgeRecord> records,
    std::string_view current_image_hash,
    std::uint32_t minimum_score = 70u);

// Stable identity of the distinct records which affect analysis. Ordering,
// duplicate records and observation counts do not make builds spuriously
// different; semantic fingerprint/category/label changes do.
[[nodiscard]] std::uint64_t knowledge_base_fingerprint(
    std::span<const KnowledgeRecord> records) noexcept;

[[nodiscard]] bool load_knowledge_base(const std::filesystem::path& path,
                                       std::vector<KnowledgeRecord>& records,
                                       std::string& error);
[[nodiscard]] bool save_knowledge_base(const std::filesystem::path& path,
                                       std::span<const KnowledgeRecord> records,
                                       std::string& error);

// Converts an analysis report into reusable generic function records. Curated
// CRT/middleware labels remain external data; Xenon never invents a library
// identity merely because bytes look similar.
[[nodiscard]] std::vector<KnowledgeRecord> export_analysis_knowledge(
    const AnalysisReport& report, std::string_view source_image_hash);

}  // namespace xenon::recomp
