#pragma once

// Program Analysis Hint Schema V2 (Part 1 of the Project Gracemeria readiness
// pass). Additive to the existing xenon::recomp::ModuleHint (driver.hpp),
// which remains supported unchanged for simple titles/tests - this schema
// exists because a complex commercial title (Ace Combat 6) needs to express
// function chunks/parent relationships, explicit switch-table targets, known
// indirect call/branch targets, native CRT/heap replacements, setjmp/longjmp
// runtime-helper identities, and revision-scoped data/ignored/invalid-code
// regions, none of which the plain ModuleHint model can represent.
//
// Modeled after inspecting sal063/AC6_recomp's public ac6recomp_config.toml
// (research reference per CLAUDE.md - reimplemented independently, not
// copied): that config's root longjmp_address/setjmp_address keys map to
// RuntimeHelper, its bare `indirect_calls = [addr, ...]` array maps to
// KnownIndirectCall (call sites the title's own analysis already flagged as
// indirect, without necessarily supplying resolved targets), its `[rexcrt]`
// section (symbolic CRT/heap name -> guest address) maps to
// NativeReplacement, and its `[functions]` section (guest address -> name)
// maps to FunctionHint. The actual ac6recomp_config.toml -> AnalysisHintSetV2
// migration tool deliberately does NOT live in this repository - see
// docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md's "AC6 config migration tool lives in
// Project Gracemeria, not here" section for why (and for the full field
// reference) - this header only defines the generic schema every game module
// (AC6 or otherwise) targets.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp::analysis {

// Bumped whenever a field's meaning changes incompatibly. A hint set whose
// schema_version is not one this build understands must be rejected outright
// (see validate()) rather than partially applied - matches the Runtime
// Host/Title Update "no partial/undefined-behavior half-application" pattern
// established elsewhere in Xenon.
inline constexpr int kAnalysisSchemaVersion = 2;

enum class FunctionFlags : std::uint32_t {
  None = 0,
  NoReturn = 1u << 0,   // function never returns to its caller (e.g. abort/exit paths)
  Leaf = 1u << 1,       // function makes no further calls
  Thunk = 1u << 2,      // trivial forwarding stub (e.g. import/vtable thunk)
};

[[nodiscard]] constexpr FunctionFlags operator|(FunctionFlags a, FunctionFlags b) noexcept {
  return static_cast<FunctionFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has_flag(FunctionFlags value, FunctionFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

// A single function's static-analysis hint. Only `address` is required;
// automatic analysis remains primary (Part 1.4) - explicit end/size only
// resolve ambiguity the analyzer's own heuristics could not.
struct FunctionHint {
  std::uint32_t address{};
  std::optional<std::uint32_t> end;    // exclusive end address, if known
  std::optional<std::uint32_t> size;   // in bytes; used when `end` is absent
  std::string name;
  // Address of this function's logical parent, when this hint itself
  // describes a discontinuous *chunk* rather than a function's primary
  // entry. Absent for an ordinary top-level function.
  std::optional<std::uint32_t> parent_function;
  FunctionFlags flags{FunctionFlags::None};
};

// A discontinuous secondary range that belongs to `parent_function` (e.g. a
// cold/exception-handling tail the compiler placed elsewhere in the image).
// Represented separately from FunctionHint so a function can own any number
// of chunks without repeating its own metadata per-chunk.
struct FunctionChunk {
  std::uint32_t start{};
  std::uint32_t end{};  // exclusive
  std::uint32_t parent_function{};
};

enum class SwitchEntryFormat : std::uint8_t {
  AbsoluteWord32,   // table entry is the absolute target address
  RelativeWord32,   // table entry is a 32-bit displacement from the table base
  RelativeInt16,    // 16-bit signed displacement (compact jump tables)
};

// Manual jump/switch-table metadata for sites automatic nearby-aligned-word
// heuristics cannot safely resolve (Part 1.6). Either supply table_address/
// entry_count/entry_format for the analyzer to decode, or supply
// explicit_targets directly when even the table's shape is not safely
// inferable; both may be given (explicit_targets always wins where present).
struct SwitchTableHint {
  std::uint32_t site{};  // address of the indirect branch instruction itself
  std::optional<std::uint32_t> table_address;
  std::optional<std::uint32_t> entry_count;
  SwitchEntryFormat entry_format{SwitchEntryFormat::AbsoluteWord32};
  std::optional<std::uint32_t> index_register;  // PPC GPR index (0-31), if relevant
  std::vector<std::uint32_t> explicit_targets;
};

// A `bl`/indirect-call site whose possible target(s) are known even though
// the target register's value cannot be determined statically. An empty
// `targets` list is itself meaningful: it records that this site was
// deliberately investigated and acknowledged as indirect (matching e.g.
// AC6/ReXGlue's bare `indirect_calls = [addr, ...]` list), distinguishing it
// in diagnostics from a site nothing ever looked at (Part 1.7 - "report
// unresolved sites clearly" applies to sites with no hint at all, not to
// these).
struct KnownIndirectCall {
  std::uint32_t callsite{};
  std::vector<std::uint32_t> targets;
};

// Same idea for a non-linking indirect branch (tail-call/computed jump).
struct KnownIndirectBranch {
  std::uint32_t site{};
  std::vector<std::uint32_t> targets;
};

// Generic Xenon-owned CRT/heap replacement identities a module may bind a
// guest address to (Part 1.10). Xenon supplies the actual implementation
// (see native_replacements.hpp); a module only ever declares the mapping.
// `Unsupported` is a real, reachable value - not a stub - covering a real
// AC6/rexcrt symbol (e.g. XMemCpy) this build does not yet implement a
// verified replacement for; the driver reports it, it never silently
// no-ops.
enum class NativeReplacementKind : std::uint8_t {
  Unsupported = 0,
  Memcpy,
  Memmove,
  Memset,
  MemcpyChecked,   // memcpy_s-shaped: (dst, dst_size, src, count)
  MemmoveChecked,  // memmove_s-shaped
  Memcmp,
  Strlen,
  Strncmp,
  Strncpy,
  Strchr,
  Strstr,
  Strrchr,
  StrcpyChecked,   // strcpy_s-shaped: (dst, dst_size, src)
  HeapAllocate,    // RtlAllocateHeap-shaped: (heap, flags, size) -> ptr
  HeapFree,        // RtlFreeHeap-shaped: (heap, flags, ptr) -> bool
  HeapSize,        // RtlSizeHeap-shaped: (heap, flags, ptr) -> size
  HeapReAllocate,  // RtlReAllocateHeap-shaped: (heap, flags, ptr, size) -> ptr
};

[[nodiscard]] const char* native_replacement_kind_name(NativeReplacementKind kind) noexcept;
// Maps a symbolic rexcrt-style name (case-sensitive, matching common
// ReXGlue/AC6Recomp spelling) to a supported kind, or Unsupported if this
// build recognizes no verified replacement for that name. Used by both the
// driver's own hint consumption and tools/ac6_migrate.cpp.
[[nodiscard]] NativeReplacementKind native_replacement_kind_from_name(const std::string& name) noexcept;

struct NativeReplacement {
  std::uint32_t guest_address{};
  NativeReplacementKind kind{NativeReplacementKind::Unsupported};
  // Original symbolic name from the source metadata (AC6/rexcrt name, a
  // module manifest's own label, ...), kept for diagnostics even once
  // resolved to `kind`.
  std::string source_name;
};

// PPC/Xenon-toolchain nonvolatile-register prologue/epilogue "fall-through
// ladder" spill helpers, in addition to SetJmp/LongJmp. These correspond
// exactly to hedge-dev/XenonRecomp's own RecompilerConfig fields
// (restGpr14Address, saveGpr14Address, restFpr14Address, saveFpr14Address,
// restVmx14Address, saveVmx14Address, restVmx64Address, saveVmx64Address -
// research reference only, reimplemented independently; see
// docs/runtime/RUNTIME_HELPERS.md) - a real, title-independent compiler convention
// used broadly by XenonRecomp-toolchain-based static recompilations (first
// evidenced via Project Gaia's Sonic Unleashed/UnleashedRecomp SWA.toml
// migration, see GENERIC_XENON_BLOCKERS.md), not something invented for one
// title. Each kind has many callable *variants*, one per starting register
// index the real toolchain emits a distinct fall-through entry point for
// (Part 1.5) - see RuntimeHelper::register_start and
// expand_runtime_helpers() below rather than declaring one enum value per
// register count.
enum class RuntimeHelperKind : std::uint8_t {
  SetJmp,
  LongJmp,
  // Saves/restores r{register_start}..r31 (64-bit slots) plus LR, entered at
  // a register-count-specific offset from the family's declared base
  // address. SaveGprLr is entered via `bl` (returns into the caller's own
  // remaining prologue); RestoreGprLr is entered via a tail `b` (returns to
  // the caller's *caller*, replacing the function's own epilogue).
  SaveGprLr,
  RestoreGprLr,
  // Saves/restores f{register_start}..f31 (64-bit slots). Never touches LR.
  SaveFpr,
  RestoreFpr,
  // Saves/restores v{register_start}..v31 (128-bit slots, standard VMX
  // range). Never touches LR.
  SaveVmx,
  RestoreVmx,
  // Saves/restores v{register_start}..v127 (128-bit slots, the Xenon CPU's
  // extended VMX128-only register range, register_start in [64,127]).
  // Never touches LR.
  SaveVmx128,
  RestoreVmx128,
};

// True for the 8 register-range RuntimeHelperKind values above (as opposed
// to SetJmp/LongJmp, which denote exactly one address each).
[[nodiscard]] bool runtime_helper_kind_is_register_range(RuntimeHelperKind kind) noexcept;
// True for the "Save*" register-range kinds, false for "Restore*". Not
// meaningful (returns false) for SetJmp/LongJmp.
[[nodiscard]] bool runtime_helper_kind_is_save(RuntimeHelperKind kind) noexcept;
// Exclusive upper bound on the register index this family's ladder can
// reach: 32 for the GPR/FPR/VMX base families (registers 14..31), 128 for
// the extended VMX128 family (registers 64..127). Only meaningful for
// register-range kinds.
[[nodiscard]] std::uint32_t runtime_helper_register_limit(RuntimeHelperKind kind) noexcept;
// The lowest starting register index this family's toolchain-declared base
// address can represent: 14 for GPR/FPR/VMX, 64 for extended VMX128. Only
// meaningful for register-range kinds.
[[nodiscard]] std::uint32_t runtime_helper_register_family_base(RuntimeHelperKind kind) noexcept;
// Byte distance between two adjacent register-count entry points (i.e.
// between the address for register_start=N and register_start=N+1) -
// matches XenonRecomp's own Analyse() formula: 4 bytes (one single
// register+immediate store/load instruction) for GPR/FPR, 8 bytes (an
// index-register setup instruction plus one indexed vector store/load) for
// VMX/VMX128, which cannot address a stack slot with an immediate
// displacement. Only meaningful for register-range kinds.
[[nodiscard]] std::uint32_t runtime_helper_register_stride(RuntimeHelperKind kind) noexcept;

// A title-specific runtime-helper implementation address (Part 1.9). Xenon
// models this generically; no address is ever hard-coded into Xenon core.
// For the 8 register-range kinds, `address` is the family's single
// toolchain-declared BASE address (the lowest register-count variant, e.g.
// XenonRecomp's `restGpr14Address`) and `register_start` is that base
// variant's starting register (e.g. 14, or 64 for the extended VMX128
// family) - Xenon itself derives every other callable variant address in
// the family (see expand_runtime_helpers()), rather than the module
// declaring one entry per register count. Absent (nullopt) for SetJmp/
// LongJmp, which denote exactly one address each.
struct RuntimeHelper {
  std::uint32_t address{};
  RuntimeHelperKind kind{};
  std::optional<std::uint32_t> register_start;
};

// Expands a possibly-compact runtime helper list - where each register-range
// kind (Part 1.5) declares only its family's lowest-numbered callable
// variant - into one concrete RuntimeHelper entry per actually-callable
// variant address, using the exact address/register-count formula
// XenonRecomp's own toolchain convention uses (its Analyse() function).
// SetJmp/LongJmp entries pass through unchanged (they already denote
// exactly one address). The result may contain far more entries than the
// input; callers needing "is this address a runtime helper" should query
// the expanded list, not `AnalysisHintSetV2::runtime_helpers` directly.
[[nodiscard]] std::vector<RuntimeHelper> expand_runtime_helpers(
    const std::vector<RuntimeHelper>& helpers);

enum class RegionKind : std::uint8_t {
  Data,              // embedded data - never attempt disassembly here
  Ignored,           // skip entirely (padding, unreachable, etc.)
  InvalidInstruction,// known-bad decode region (obfuscation/tooling artifact)
  CodeOverride,      // force-treat as code even if other heuristics disagree
};

struct RegionHint {
  std::uint32_t start{};
  std::uint32_t end{};  // exclusive
  RegionKind kind{};
  std::string reason;
};

struct SymbolHint {
  std::uint32_t address{};
  std::string name;
};

// A literal PPC instruction-word content match, evaluated during the
// analyzer's linear disassembly scan - distinct from RegionHint, which is
// address-range-based. Matches hedge-dev/XenonRecomp's own
// `invalidInstructions` convention (`std::unordered_map<word, skip_bytes>`,
// consulted at every scan position via `config.invalidInstructions.find(
// ByteSwap(*(uint32_t*)data))` - research reference only, reimplemented
// independently; see docs/runtime/RUNTIME_HELPERS.md and GENERIC_XENON_BLOCKERS.md
// for the evidence this is a content rule, not an address, e.g. real SWA.toml
// entries `data = 0x00000000 # Padding` / `data = 0x00485645 # End of .text`
// which are not plausible guest addresses at all): whenever the exact 4-byte
// big-endian instruction word at the current scan position matches `value`
// (masked by `mask`; default all-ones means an exact match), the scanner
// treats it and the next `skip_bytes` bytes as non-code/embedded data and
// ends the current function/chunk's decode there (Part 2.2 - "classify
// matching word as embedded data" / "mark instruction site invalid";
// deliberately NOT a runtime illegal-instruction trap, since that is not
// what this metadata means).
struct InstructionPatternHint {
  std::uint32_t value{};
  std::uint32_t mask{0xFFFFFFFFu};
  std::uint32_t skip_bytes{4u};
  std::string reason;
  // Optional scope restriction (Part 2.3): when both are set, the pattern is
  // only matched at scan positions within [scope_start, scope_end); when
  // either is absent, matching applies globally across every scanned
  // position, matching real XenonRecomp semantics (checked at every scan
  // position with no address restriction).
  std::optional<std::uint32_t> scope_start;
  std::optional<std::uint32_t> scope_end;  // exclusive
};

// Raw byte patch applied to guest memory/image data by the runtime
// (application is XenonSession/module-loading's responsibility - this
// schema only represents and validates the declaration). `patch_bytes` is a
// plain hex string ("DE AD BE EF" or "DEADBEEF"), kept as source text rather
// than a decoded buffer so a malformed patch is a validate() error, not a
// silent truncation.
struct PatchDeclaration {
  std::uint32_t address{};
  std::string patch_bytes_hex;
  std::string description;
};

struct HookDeclaration {
  std::uint32_t address{};
  std::string native_replacement_identity;  // free-form; Gracemeria-defined
  std::string description;
};

// A complete, versioned analysis hint set, scoped to one effective
// executable revision (Part 1.3). `identity` must match the currently
// loaded xbox::XexEffectiveIdentity exactly (title_id, media_id, and
// effective_image_hash) for this set to be eligible - see
// hint_set_matches_identity() - so a base-executable hint set is never
// silently applied to a title-update-patched revision or vice versa.
struct AnalysisHintSetV2 {
  int schema_version{kAnalysisSchemaVersion};
  std::string module_name;
  xbox::XexEffectiveIdentity identity;

  std::vector<FunctionHint> functions;
  std::vector<FunctionChunk> chunks;
  std::vector<SwitchTableHint> switches;
  std::vector<KnownIndirectCall> indirect_calls;
  std::vector<KnownIndirectBranch> indirect_branches;
  std::vector<NativeReplacement> native_replacements;
  std::vector<RuntimeHelper> runtime_helpers;
  std::vector<InstructionPatternHint> instruction_patterns;
  std::vector<RegionHint> regions;
  std::vector<SymbolHint> symbols;
  std::vector<PatchDeclaration> patches;
  std::vector<HookDeclaration> hooks;
};

// True when `hint_set.identity` is compatible with the executable actually
// loaded (`effective`): title_id/media_id must match, and the effective
// image hash must match exactly - never a "closest looking" fallback (Part
// 1.3/2.5: wrong-revision hints must never be silently applied).
[[nodiscard]] bool hint_set_matches_identity(const AnalysisHintSetV2& hint_set,
                                             const xbox::XexEffectiveIdentity& effective) noexcept;

// Structural validation independent of any particular executable (Part
// 1.5's cycle/overlap checks, schema version, malformed ranges). Appends a
// human-readable message per problem found to `errors` and returns true iff
// none were found. Does not check identity scoping - see
// hint_set_matches_identity() for that, which needs the loaded image and is
// therefore checked separately by the provider/driver.
[[nodiscard]] bool validate_hint_set(const AnalysisHintSetV2& hint_set,
                                     std::vector<std::string>& errors);

}  // namespace xenon::recomp::analysis
