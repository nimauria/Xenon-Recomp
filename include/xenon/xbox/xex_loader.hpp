#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/memory/address_space.hpp"

namespace xenon::xbox {

enum class XexFormat : std::uint8_t {
  Unknown = 0,
  Xex1,
  Xex2,
};

// XEX_ENCRYPTION_* (xex2_opt_file_format_info::encryption_type).
enum class XexEncryptionType : std::uint16_t {
  None = 0,
  Normal = 1,
};

// XEX_COMPRESSION_* (xex2_opt_file_format_info::compression_type).
enum class XexCompressionType : std::uint16_t {
  None = 0,
  Basic = 1,
  Normal = 2,
  Delta = 3,
};

// xex2_section_type (low 4 bits of a page descriptor's big-endian value).
enum class XexPageType : std::uint8_t {
  Unknown = 0,
  Code = 1,
  Data = 2,
  ReadOnlyData = 3,
};

// XEX_MODULE_* (base header module_flags).
namespace module_flags {
constexpr std::uint32_t kTitle = 0x00000001u;
constexpr std::uint32_t kExportsToTitle = 0x00000002u;
constexpr std::uint32_t kSystemDebugger = 0x00000004u;
constexpr std::uint32_t kDllModule = 0x00000008u;
constexpr std::uint32_t kModulePatch = 0x00000010u;
constexpr std::uint32_t kPatchFull = 0x00000020u;
constexpr std::uint32_t kPatchDelta = 0x00000040u;
constexpr std::uint32_t kUserMode = 0x00000080u;
}  // namespace module_flags

// XEX native-import records have two distinct on-image roles. A type-0
// record is an import-address/value slot (or a true imported variable when no
// matching type-1 record exists); a type-1 record is the callable import
// thunk. Keeping that distinction is essential: flattening both records into
// identical "imports" duplicates diagnostics and makes true variable imports
// impossible to bind correctly. PE imports are represented as callable
// imports as well.
enum class XexImportKind : std::uint8_t {
  Variable = 0,
  FunctionThunk = 1,
  FunctionAddress = 2,
  PeFunction = 3,
  Unknown = 0xFF,
};

// One import record discovered from the XEX-native import-libraries optional
// header (XEX_HEADER_IMPORT_LIBRARIES) or, as a fallback, the PE import
// directory. `guest_thunk` is the guest address of the record/IAT slot. A
// FunctionAddress record is metadata paired with a callable FunctionThunk; on
// Xbox kernel imports its slot is not itself a callable function pointer. True
// Variable records are rewritten by the runtime to the guest address exported
// by the system module. `attributes` preserves the encoded upper 16 bits of
// the native placeholder for diagnostics/tooling.
struct XexImport {
  std::string module;
  std::string symbol;
  std::uint16_t ordinal{};
  std::uint32_t guest_thunk{};
  std::uint32_t attributes{};
  XexImportKind kind{XexImportKind::Unknown};

  [[nodiscard]] bool callable() const noexcept {
    return kind == XexImportKind::FunctionThunk || kind == XexImportKind::PeFunction;
  }
  [[nodiscard]] bool is_function_address() const noexcept {
    return kind == XexImportKind::FunctionAddress;
  }
  [[nodiscard]] bool is_variable() const noexcept {
    return kind == XexImportKind::Variable;
  }
};

struct XexExport {
  std::string name;
  std::uint16_t ordinal{};
  std::uint32_t address{};
  std::uint32_t attributes{};
};

// Native XEX TLS descriptor (XEX_HEADER_TLS_INFO), distinct from - and
// preferred over, when present - a PE TLS directory entry.
struct XexTls {
  std::uint32_t slot{};
  std::uint32_t raw_data_start{};
  std::uint32_t raw_data_size{};
  std::uint32_t index_address{};
  std::uint32_t callback_address{};
  std::uint32_t data_size{};
};

struct XexRelocation {
  // IMAGE_BASE_RELOCATION::VirtualAddress is the target page RVA, not the
  // image offset of the relocation block. It remains an RVA in this model.
  std::uint32_t virtual_address{};
  std::uint32_t size{};
  std::uint16_t type{};
  std::vector<std::uint32_t> entries;
};

struct XexFunctionMetadata {
  memory::GuestAddress begin{};
  memory::GuestAddress end{};
  std::uint32_t unwind_data{};
  bool valid{};
};

struct XexSection {
  std::string name;
  memory::GuestAddress virtual_address{};
  std::uint32_t virtual_size{};
  std::uint32_t raw_size{};
  std::uint32_t raw_pointer{};
  std::uint32_t characteristics{};
  memory::Protect protect{memory::Protect::None};
  bool executable{false};
  bool writable{false};
  bool readable{false};
  std::vector<std::byte> bytes;
};

struct XexRegion {
  memory::GuestAddress begin{};
  memory::GuestAddress end{};
  std::uint32_t page_size{4096u};
  memory::Protect protect{memory::Protect::None};
};

// xex2_page_descriptor: one run of `page_count` pages of the same type,
// covering security_info.image_size in order starting at load_address. This
// is the ground-truth RWX/section-kind description of the effective image,
// independent of (and cross-checked against) PE section headers.
struct XexPageDescriptor {
  XexPageType type{XexPageType::Unknown};
  std::uint32_t page_count{};
  std::array<std::byte, 20> data_digest{};
};

// xex2_opt_execution_info (XEX_HEADER_EXECUTION_INFO).
struct XexVersion {
  std::uint32_t value{};
  [[nodiscard]] std::uint8_t major() const noexcept { return static_cast<std::uint8_t>(value & 0xFu); }
  [[nodiscard]] std::uint8_t minor() const noexcept { return static_cast<std::uint8_t>((value >> 4u) & 0xFu); }
  [[nodiscard]] std::uint16_t build() const noexcept { return static_cast<std::uint16_t>((value >> 8u) & 0xFFFFu); }
  [[nodiscard]] std::uint8_t qfe() const noexcept { return static_cast<std::uint8_t>((value >> 24u) & 0xFFu); }
};

struct XexExecutionInfo {
  std::uint32_t media_id{};
  XexVersion version{};
  XexVersion base_version{};
  std::uint32_t title_id{};
  std::uint8_t platform{};
  std::uint8_t executable_table{};
  std::uint8_t disc_number{};
  std::uint8_t disc_count{};
  std::uint32_t savegame_id{};
};

// xex2_security_info. `encrypted_image_key` is the per-title AES key as
// stored on disk (wrapped/encrypted with the fixed retail or devkit key -
// see xex_crypto.hpp); it is never exposed decrypted outside the loader's
// decryption step.
// Field-for-field this matches XEX2's security_info layout; parse_security_info()
// (xex_loader.cpp) populates it correctly from either the XEX2 or the
// structurally different XEX1 on-disk layout (XEX1 has no header_digest/
// export_table/import_table_count fields of its own - xex1_root_import_address
// is set instead of import_table_count/import_table_digest for that format,
// header_digest/export_table are left at zero).
struct XexSecurityInfo {
  std::uint32_t header_size{};
  std::uint32_t image_size{};
  std::array<std::byte, 0x100> rsa_signature{};
  std::uint32_t image_flags{};
  std::uint32_t load_address{};
  std::array<std::byte, 20> section_digest{};
  std::uint32_t import_table_count{};
  std::array<std::byte, 20> import_table_digest{};
  std::array<std::byte, 16> xgd2_media_id{};
  std::array<std::byte, 16> encrypted_image_key{};
  std::uint32_t export_table{};
  std::array<std::byte, 20> header_digest{};
  std::uint32_t region{};
  std::uint32_t allowed_media_types{};
  std::vector<XexPageDescriptor> page_descriptors;
  // XEX1 only: guest RVA of the "HVI"-tagged root import table
  // (xex_opt::xex1::HvImageRootImport), XEX1's own import-table mechanism
  // distinct from XEX2's import_table_count/import_table_digest pair. Zero
  // for XEX2 images. Exposed for completeness; not resolved into XexImport
  // entries by this loader (XEX_HEADER_IMPORT_LIBRARIES, present on both
  // formats' title EXEs, remains the primary import mechanism parse_xex_image()
  // resolves - see docs/xbox/XEX_LOADER_V2.md "Known limitations").
  std::uint32_t xex1_root_import_address{};
};

// XEX_HEADER_DELTA_PATCH_DESCRIPTOR (xex2_opt_delta_patch_descriptor), used
// by apply_title_update() both to validate a title update against its
// intended base image, and - when delta_headers_source_size != 0 - to
// reconstruct the patch's *effective* on-disk header from the base image's
// header bytes before the rest of the pipeline ever sees them (real XEXP
// title updates route their header through this delta, not through an
// on-disk header that is already the final target header verbatim). Never
// used to modify the base image's own bytes in place - see
// apply_title_update().
//
// header_patch_* describe the single embedded xex2_delta_patch record at
// descriptor offset 0x4C: a 12-byte (old_addr, new_addr, uncompressed_len,
// compressed_len) header, optionally followed by compressed_len bytes of
// LZX-compressed payload (see xex_lzx::apply_delta_patch_records() for the
// exact record semantics, including the compressed_len 0/1 sentinels).
// header_patch_data_offset is the absolute byte offset of that payload
// within the *patch file*, so the payload can be read directly from the raw
// update bytes without copying it into this struct.
struct XexDeltaPatchDescriptor {
  bool present{false};
  XexVersion target_version{};
  XexVersion source_version{};
  std::array<std::byte, 20> base_signature_digest{};
  std::array<std::byte, 16> image_key_source{};
  std::uint32_t size_of_target_headers{};
  std::uint32_t delta_headers_source_offset{};
  std::uint32_t delta_headers_source_size{};
  std::uint32_t delta_headers_target_offset{};
  std::uint32_t delta_image_source_offset{};
  std::uint32_t delta_image_source_size{};
  std::uint32_t delta_image_target_offset{};
  std::uint32_t header_patch_old_addr{};
  std::uint32_t header_patch_new_addr{};
  std::uint16_t header_patch_uncompressed_len{};
  std::uint16_t header_patch_compressed_len{};
  std::size_t header_patch_data_offset{};
};

struct XexImage {
  XexFormat format{XexFormat::Unknown};
  std::uint32_t module_flags{};
  std::uint32_t header_size{};
  std::uint32_t security_offset{};
  std::uint32_t optional_header_count{};
  std::uint32_t image_base{};
  std::uint32_t entry_point{};
  std::uint32_t execution_id{};  // xex2_opt_execution_info::savegame_id.
  std::uint32_t title_id{};
  std::uint32_t media_id{};
  std::uint32_t region{};
  XexEncryptionType encryption_type{XexEncryptionType::None};
  XexCompressionType compression_type{XexCompressionType::None};
  XexSecurityInfo security{};
  XexExecutionInfo execution_info{};
  XexDeltaPatchDescriptor delta_patch{};
  bool is_patch{false};
  bool is_full_patch{false};
  bool is_delta_patch{false};
  std::vector<XexSection> sections;
  std::vector<XexImport> imports;
  std::vector<XexExport> exports;
  std::vector<XexRelocation> relocations;
  std::vector<XexFunctionMetadata> function_metadata;
  std::optional<XexTls> tls;
  std::string original_pe_name;
  // The exact on-disk header bytes ([0, header_size) of the bytes passed to
  // parse_xex_image()) this image was parsed from. Retained so
  // apply_title_update() can use a base image's own header as the source
  // half of a XEXP header-region delta (XexDeltaPatchDescriptor::
  // delta_headers_source_offset/size) without having to re-derive it.
  std::vector<std::byte> header_bytes;
  // The fully decrypted + decompressed body: PE headers/sections/imports/
  // exports/TLS/relocations are all parsed from this, never from the raw
  // on-disk (possibly encrypted/compressed) file bytes.
  std::vector<std::byte> effective_image;
};

struct LoadedXex {
  XexImage image{};
  memory::GuestAddress image_base{};
  std::vector<XexSection> mapped_sections;
  std::vector<XexRegion> executable_ranges;
  bool loaded{false};
  std::string error;
};

[[nodiscard]] XexFormat detect_xex_format(std::span<const std::byte> bytes) noexcept;

// Parses a XEX1/XEX2 file: header, optional headers, security info and page
// descriptors, then decrypts (if encryption_type != None) and decompresses
// (None/Basic/Normal/Delta) the body into `out_image.effective_image`, and
// finally parses the resulting PE image (sections, imports, exports, TLS,
// relocations, function/exception metadata) from that effective image.
//
// `reference_image` is only consulted when compression_type == Delta (XEXP
// title-update patches, which are LZX-delta-compressed against a base
// image); pass the base XexImage's effective_image. It is ignored for every
// other compression type. See apply_title_update() for the full patch flow.
[[nodiscard]] bool parse_xex_image(std::span<const std::byte> bytes,
                                  XexImage& out_image,
                                  std::string* error = nullptr,
                                  std::span<const std::byte> reference_image = {});

// Maps an already-parsed `image` (from parse_xex_image() or
// apply_title_update()) into Memory V2 with real per-page-descriptor R/W/X
// protections and hands executable ranges to CPU V2's executable-generation
// tracking (via AddressSpace::write_bytes(), which already advances the
// executable generation on physical writes - no separate XEX-specific
// executable-page registry is introduced). This is the mapping half of what
// load_xex() below does in one call; it is exposed separately so a caller
// that already has a parsed/patched XexImage (e.g. XenonSession loading the
// *effective* image after a title update was applied) does not need to
// re-serialize it back into on-disk file bytes just to map it.
[[nodiscard]] bool map_xex_image(memory::AddressSpace& memory,
                                 const XexImage& image,
                                 LoadedXex& out_loaded,
                                 memory::GuestAddress preferred_base =
                                     memory::kXex64KBase,
                                 std::string* error = nullptr);

// Parses `file_bytes` (see parse_xex_image()) and maps the result into
// Memory V2 (see map_xex_image()). Equivalent to calling both separately;
// kept as a single call for the common case of loading an unpatched XEX
// straight from disk.
[[nodiscard]] bool load_xex(memory::AddressSpace& memory,
                           std::span<const std::byte> file_bytes,
                           LoadedXex& out_loaded,
                           memory::GuestAddress preferred_base =
                               memory::kXex64KBase,
                           std::string* error = nullptr);

// Validates `update_bytes` as a title update for `base_image` (title/media
// identity, base-signature digest, source-version match) and produces the
// effective patched image in `out_image`. `base_image.effective_image` is
// read only, never modified - the result is always a new, independent
// XexImage. See docs/xbox/XEX_LOADER_V2.md for exactly what patch forms
// (XEX_MODULE_PATCH_FULL vs XEX_MODULE_PATCH_DELTA) are supported.
[[nodiscard]] bool apply_title_update(const XexImage& base_image,
                                     std::span<const std::byte> update_bytes,
                                     XexImage& out_image,
                                     std::string* error = nullptr);

// SHA1 of `image.effective_image` - the canonical "which exact executable
// bytes is this" identity XEX Loader V2 exposes to the rest of Xenon
// (module-compatibility validation, recompilation identity, diagnostics).
// Computed from the fully decrypted/decompressed/patched body, independent
// of on-disk compression/encryption, so a base image and its title-update-
// patched effective image reliably hash differently whenever the patch
// actually changes code/data, and a module built from the same bytes always
// reproduces the same hash. See docs/xbox/XEX_LOADER_V2.md "Effective executable
// identity".
[[nodiscard]] std::array<std::byte, 20> compute_effective_image_hash(const XexImage& image);

// Lowercase hex string form of a compute_effective_image_hash() result, used
// wherever the hash needs to be a plain string (native-extension ABI,
// diagnostics/status.json).
[[nodiscard]] std::string format_effective_image_hash(const std::array<std::byte, 20>& hash);

// Summarizes the identity of the executable a session actually runs:
// title/media ID, the base image's own version, the version of whichever
// image is effective (== base_version when no title update was applied),
// and the effective image's content hash. `patched_image`, when non-null,
// is XEX Loader V2's apply_title_update() output for `base_image` - passing
// it is what distinguishes "base XEX" from "base + selected title update"
// identity for module-compatibility validation and diagnostics.
struct XexEffectiveIdentity {
  std::uint32_t title_id{};
  std::uint32_t media_id{};
  XexVersion base_version{};
  XexVersion effective_version{};
  std::array<std::byte, 20> effective_image_hash{};
  // Part 12 of the AC6 Runtime Readiness pass ("Title Update fidelity"):
  // the base image's own hash, independent of whether a title update was
  // applied - equal to effective_image_hash exactly when
  // title_update_applied is false. Lets a report show "Base SHA1" and
  // "Effective SHA1" as two genuinely distinct values (per the pass's own
  // required report format) instead of only ever exposing whichever image
  // actually ran.
  std::array<std::byte, 20> base_image_hash{};
  bool title_update_applied{false};
};

[[nodiscard]] XexEffectiveIdentity compute_effective_identity(const XexImage& base_image,
                                                               const XexImage* patched_image = nullptr);

}  // namespace xenon::xbox
