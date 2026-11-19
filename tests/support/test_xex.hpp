#pragma once

// Builders for synthetic XEX fixtures shared by the XEX loader tests and the
// input fuzz tests: a plaintext PE body, the XEX2/XEX1 header around it, the
// compressed and delta block chains and FileFormatInfo header rewriting.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/xbox/xex_crypto.hpp"

namespace xenon::test::xex {

// ---------------------------------------------------------------------------
// A tiny append/patch binary writer, so the fixtures below can be built by
// describing structures instead of hand-computing every byte offset.
// ---------------------------------------------------------------------------
class ByteWriter {
 public:
  std::size_t append_be32(std::uint32_t v) {
    const auto offset = bytes_.size();
    bytes_.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
    bytes_.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    bytes_.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    bytes_.push_back(static_cast<std::byte>(v & 0xFFu));
    return offset;
  }
  std::size_t append_be16(std::uint16_t v) {
    const auto offset = bytes_.size();
    bytes_.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    bytes_.push_back(static_cast<std::byte>(v & 0xFFu));
    return offset;
  }
  std::size_t append_le32(std::uint32_t v) {
    const auto offset = bytes_.size();
    bytes_.push_back(static_cast<std::byte>(v & 0xFFu));
    bytes_.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    bytes_.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    bytes_.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
    return offset;
  }
  std::size_t append_le16(std::uint16_t v) {
    const auto offset = bytes_.size();
    bytes_.push_back(static_cast<std::byte>(v & 0xFFu));
    bytes_.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    return offset;
  }
  std::size_t append_bytes(std::span<const std::byte> data) {
    const auto offset = bytes_.size();
    bytes_.insert(bytes_.end(), data.begin(), data.end());
    return offset;
  }
  std::size_t append_string(const std::string& s, bool nul_terminate = true) {
    const auto offset = bytes_.size();
    for (const auto c : s) bytes_.push_back(static_cast<std::byte>(c));
    if (nul_terminate) bytes_.push_back(std::byte{0});
    return offset;
  }
  std::size_t append_zeros(std::size_t n) {
    const auto offset = bytes_.size();
    bytes_.insert(bytes_.end(), n, std::byte{0});
    return offset;
  }
  void put_be32(std::size_t offset, std::uint32_t v) {
    bytes_[offset + 0] = static_cast<std::byte>((v >> 24) & 0xFFu);
    bytes_[offset + 1] = static_cast<std::byte>((v >> 16) & 0xFFu);
    bytes_[offset + 2] = static_cast<std::byte>((v >> 8) & 0xFFu);
    bytes_[offset + 3] = static_cast<std::byte>(v & 0xFFu);
  }
  void align(std::size_t alignment) {
    while (bytes_.size() % alignment != 0) bytes_.push_back(std::byte{0});
  }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }
  [[nodiscard]] const std::vector<std::byte>& data() const { return bytes_; }
  [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

constexpr std::uint32_t kExecutionInfoKey = 0x00040006u;
constexpr std::uint32_t kOriginalPeNameKey = 0x000183FFu;
constexpr std::uint32_t kFileFormatInfoKey = 0x000003FFu;
constexpr std::uint32_t kImportLibrariesKey = 0x000103FFu;
constexpr std::uint32_t kTlsInfoKey = 0x00020104u;
constexpr std::uint32_t kEntryPointKey = 0x00010100u;
constexpr std::uint32_t kDeltaPatchDescriptorKey = 0x000005FFu;
constexpr std::uint32_t kDefaultStackSizeKey = 0x00020200u;

constexpr std::uint32_t kImageBase = 0x82000000u;
constexpr std::uint32_t kPageSize4KB = 0x1000u;
// Memory V2 maps the 0x80000000-0x8FFFFFFF XEX region using 64 KiB "large"
// pages regardless of the XEX's own page-descriptor granularity (see
// xex_page_size_for() in xex_loader.cpp). The mapper coalesces PE sections
// that share those allocation pages, while this fixture keeps its ordinary
// sections 64 KiB apart for simple address expectations.
constexpr std::uint32_t kSectionSize = 0x10000u;

// ---------------------------------------------------------------------------
// Builds a minimal, valid PE32 image (DOS+PE headers, three sections:
// .text RX, .rdata R, .data RW; optional export directory) - this is what
// XEX_HEADER_IMPORT_LIBRARIES-driven titles look like once decompressed.
// ---------------------------------------------------------------------------
struct PeBuildResult {
  std::vector<std::byte> bytes;
  std::uint32_t entry_point_rva{};
  std::uint32_t text_rva{};
  std::uint32_t data_rva{};
};

inline PeBuildResult build_pe_body(std::uint32_t image_base, bool with_export) {
  ByteWriter w;
  constexpr std::uint32_t kPeHeaderOffset = 0x80u;

  // DOS header: 'MZ' + e_lfanew at 0x3C pointing at the PE header.
  w.append_bytes(std::array<std::byte, 2>{std::byte{'M'}, std::byte{'Z'}});
  w.append_zeros(0x3Cu - 2u);
  w.append_le32(kPeHeaderOffset);  // e_lfanew
  while (w.size() < kPeHeaderOffset) w.append_zeros(1);

  // PE signature + COFF header.
  const auto pe_sig_offset = w.size();
  assert(pe_sig_offset == kPeHeaderOffset);
  w.append_bytes(std::array<std::byte, 4>{std::byte{'P'}, std::byte{'E'}, std::byte{0}, std::byte{0}});
  w.append_le16(0x01F2u);  // Machine (arbitrary, unchecked by the loader).
  w.append_le16(3u);       // NumberOfSections
  w.append_le32(0u);       // TimeDateStamp
  w.append_le32(0u);       // PointerToSymbolTable
  w.append_le32(0u);       // NumberOfSymbols
  w.append_le16(0xE0u);    // SizeOfOptionalHeader (PE32)
  w.append_le16(0x0102u);  // Characteristics

  const auto optional_header_offset = w.size();
  w.append_le16(0x10Bu);  // PE32 magic                                    @0x00
  w.append_zeros(2);      // Major/MinorLinkerVersion                      @0x02
  w.append_le32(0u);      // SizeOfCode                                    @0x04
  w.append_le32(0u);      // SizeOfInitializedData                         @0x08
  w.append_le32(0u);      // SizeOfUninitializedData                       @0x0C
  const auto entry_point_field_offset = w.append_le32(kSectionSize);  // AddressOfEntryPoint @0x10
  w.append_le32(kSectionSize);                                   // BaseOfCode          @0x14
  w.append_le32(kSectionSize * 2u);                               // BaseOfData          @0x18
  w.append_le32(image_base);                                     // ImageBase           @0x1C
  w.append_le32(kSectionSize);                                    // SectionAlignment    @0x20
  w.append_le32(kSectionSize);                                    // FileAlignment       @0x24
  w.append_zeros(12);     // Major/MinorOS + Major/MinorImage + Major/MinorSubsystem     @0x28
  w.append_le32(0u);      // Win32VersionValue                                           @0x34
  w.append_le32(kSectionSize * 4u);  // SizeOfImage                                       @0x38
  w.append_le32(kSectionSize);       // SizeOfHeaders                                     @0x3C
  w.append_le32(0u);       // CheckSum                                                   @0x40
  w.append_zeros(4);       // Subsystem/DllCharacteristics                               @0x44
  w.append_zeros(16);      // Stack/Heap reserve+commit                                  @0x48
  w.append_le32(0u);       // LoaderFlags                                                @0x58
  w.append_le32(16u);      // NumberOfRvaAndSizes                                        @0x5C
  const auto data_directory_offset = w.size();  // @0x60
  for (int i = 0; i < 16; ++i) {
    w.append_le32(0u);  // VirtualAddress
    w.append_le32(0u);  // Size
  }
  assert(w.size() == optional_header_offset + 0xE0u);

  const auto section_table_offset = w.size();
  const auto make_section = [&](const char* name, std::uint32_t rva, std::uint32_t vsize,
                               std::uint32_t raw_ptr, std::uint32_t raw_size,
                               std::uint32_t characteristics) {
    std::array<std::byte, 8> name_field{};
    for (std::size_t i = 0; i < 8 && name[i] != '\0'; ++i) name_field[i] = static_cast<std::byte>(name[i]);
    w.append_bytes(name_field);
    w.append_le32(vsize);
    w.append_le32(rva);
    w.append_le32(raw_size);
    w.append_le32(raw_ptr);
    w.append_zeros(12);  // relocs/linenumbers pointers+counts
    w.append_le32(characteristics);
  };
  // Raw file layout, each region a full 64 KiB apart (see kSectionSize):
  // header | .text | .rdata | .data
  make_section(".text", kSectionSize, kSectionSize, kSectionSize, kSectionSize, 0x60000020u);         // R+X
  make_section(".rdata", kSectionSize * 2u, kSectionSize, kSectionSize * 2u, kSectionSize, 0x40000040u);  // R
  make_section(".data", kSectionSize * 3u, kSectionSize, kSectionSize * 3u, kSectionSize, 0xC0000040u);   // R+W
  while (w.size() < kSectionSize) w.append_zeros(1);

  const auto text_offset = w.size();
  assert(text_offset == kSectionSize);
  std::vector<std::byte> text(kSectionSize, std::byte{0});
  text[0] = std::byte{0x4Eu};
  text[1] = std::byte{0x80u};
  text[2] = std::byte{0x00u};
  text[3] = std::byte{0x20u};  // blr, arbitrary but distinctive PPC-looking bytes.
  w.append_bytes(text);

  std::uint32_t export_rva = 0u;
  std::vector<std::byte> rdata(kSectionSize, std::byte{0});
  if (with_export) {
    // A minimal export directory + one exported function, placed inside
    // .rdata (RVA kSectionSize*2 .. kSectionSize*3).
    ByteWriter rd;
    const auto dir_offset = rd.size();
    rd.append_le32(0u);   // Characteristics                @0x00
    rd.append_le32(0u);   // TimeDateStamp                  @0x04
    rd.append_le32(0u);   // MajorVersion/MinorVersion       @0x08
    rd.append_le32(0u);   // Name (RVA to DLL name, unused)  @0x0C
    rd.append_le32(1u);   // Base (ordinal base)             @0x10
    rd.append_le32(1u);   // NumberOfFunctions               @0x14
    rd.append_le32(1u);   // NumberOfNames
    const auto functions_field = rd.append_le32(0u);  // AddressOfFunctions (patched below)
    const auto names_field = rd.append_le32(0u);       // AddressOfNames
    const auto ordinals_field = rd.append_le32(0u);     // AddressOfNameOrdinals
    (void)dir_offset;

    const auto functions_offset = rd.size();
    rd.append_le32(kSectionSize);  // function RVA -> entry of .text
    const auto names_offset = rd.size();
    const auto names_entry_field = rd.append_le32(0u);  // patched once the string's offset is known.
    const auto ordinals_offset = rd.size();
    rd.append_le16(0u);
    const auto name_string_offset = rd.size();
    rd.append_string("XexTestExport");

    auto rd_bytes = rd.take();
    const auto patch_le32_early = [&](std::size_t off, std::uint32_t v) {
      rd_bytes[off + 0] = static_cast<std::byte>(v & 0xFFu);
      rd_bytes[off + 1] = static_cast<std::byte>((v >> 8) & 0xFFu);
      rd_bytes[off + 2] = static_cast<std::byte>((v >> 16) & 0xFFu);
      rd_bytes[off + 3] = static_cast<std::byte>((v >> 24) & 0xFFu);
    };
    patch_le32_early(names_entry_field, kSectionSize * 2u + static_cast<std::uint32_t>(name_string_offset));
    // Patch AddressOfFunctions/AddressOfNames/AddressOfNameOrdinals (each an
    // RVA = kSectionSize*2 + local offset within .rdata).
    patch_le32_early(functions_field, kSectionSize * 2u + static_cast<std::uint32_t>(functions_offset));
    patch_le32_early(names_field, kSectionSize * 2u + static_cast<std::uint32_t>(names_offset));
    patch_le32_early(ordinals_field, kSectionSize * 2u + static_cast<std::uint32_t>(ordinals_offset));

    std::copy(rd_bytes.begin(), rd_bytes.end(), rdata.begin());
    export_rva = kSectionSize * 2u;
    (void)export_rva;

    // Patch the optional header's export data directory entry (index 0).
    // (data_directory_offset was captured before section headers were
    // appended, still valid since we only wrote into separate buffers.)
  }
  w.append_bytes(rdata);

  std::vector<std::byte> data(kSectionSize, std::byte{0xCD});
  w.append_bytes(data);

  auto bytes = w.take();
  if (with_export) {
    const auto put_le32 = [&](std::size_t off, std::uint32_t v) {
      bytes[off + 0] = static_cast<std::byte>(v & 0xFFu);
      bytes[off + 1] = static_cast<std::byte>((v >> 8) & 0xFFu);
      bytes[off + 2] = static_cast<std::byte>((v >> 16) & 0xFFu);
      bytes[off + 3] = static_cast<std::byte>((v >> 24) & 0xFFu);
    };
    put_le32(data_directory_offset + 0, kSectionSize * 2u);  // Export directory VirtualAddress
    put_le32(data_directory_offset + 4, 0x1Cu);              // Export directory Size (approximate)
  }

  PeBuildResult result;
  result.bytes = std::move(bytes);
  result.entry_point_rva = kSectionSize;
  result.text_rva = kSectionSize;
  result.data_rva = kSectionSize * 3u;
  (void)entry_point_field_offset;
  return result;
}

// ---------------------------------------------------------------------------
// Assembles a full XEX2 file around a plaintext PE body, with security info,
// page descriptors, execution info, original-pe-name, entry point, and (for
// XEX_COMPRESSION_NONE/encryption-less callers) the body appended directly.
// Callers that need Basic/Normal/Delta compression or encryption post-process
// the returned bytes via transform_body() below, which also patches the
// file-format-info header fields to match.
// ---------------------------------------------------------------------------
struct XexBuildOptions {
  std::uint32_t title_id{0x41415858u};
  std::uint32_t media_id{0x12345678u};
  std::uint32_t module_flags{0x00000001u};  // XEX_MODULE_TITLE
  bool with_import_libraries{true};
  bool with_native_tls{true};
  // XEX_HEADER_DEFAULT_STACK_SIZE (inline); zero omits the header.
  std::uint32_t default_stack_size{0};
  bool with_delta_patch_descriptor{false};
  std::array<std::byte, 20> delta_base_signature_digest{};
  std::uint32_t delta_source_version{0};
  std::uint32_t delta_target_version{0};
  // XEXP header-region delta fields (xex2_opt_delta_patch_descriptor
  // offsets 0x30-0x48); zero (the default) means "no header content
  // changed", matching the pre-existing full/delta-image-only patch tests.
  std::uint32_t delta_size_of_target_headers{0};
  std::uint32_t delta_headers_source_offset{0};
  std::uint32_t delta_headers_source_size{0};
  std::uint32_t delta_headers_target_offset{0};
  // XEXP image-region delta fields (xex2_opt_delta_patch_descriptor offsets
  // 0x40-0x48): one optional whole-region splice applied before the
  // XEX_COMPRESSION_DELTA body's own xex2_delta_patch record chain runs -
  // zero (the default) means "no whole-region splice".
  std::uint32_t delta_image_source_offset{0};
  std::uint32_t delta_image_source_size{0};
  std::uint32_t delta_image_target_offset{0};
  // The embedded xex2_delta_patch record's raw on-disk bytes (12-byte fixed
  // header [old_addr, new_addr, uncompressed_len, compressed_len] optionally
  // followed by compressed_len bytes of payload). Defaults to an all-zero
  // 12-byte record, which parse_delta_patch_descriptor()/
  // reconstruct_patch_header_bytes() treat as "no embedded header patch".
  std::vector<std::byte> header_patch_record_bytes{std::vector<std::byte>(12u, std::byte{0})};
  // When true, emits a real 'XEX1' file with XEX1's own (structurally
  // different, not merely differently-tagged) security_info layout - see
  // kXex1SecurityInfoFixedSize in xex_loader.cpp.
  bool use_xex1_format{false};
};

struct XexBuildResult {
  std::vector<std::byte> file;   // Complete XEX2 (compression=None, encryption=None).
  std::size_t header_size{};
  std::size_t file_format_info_offset{};  // Offset of the FileFormatInfo struct in `file`.
  std::size_t execution_info_offset{};    // Offset of the XexExecutionInfo struct in `file`.
  std::vector<std::byte> plain_body;      // The exact plaintext body (== file[header_size..]).
};

inline XexBuildResult build_base_xex(const PeBuildResult& pe, const XexBuildOptions& opts) {
  ByteWriter header;
  header.append_bytes(std::array<std::byte, 4>{
      std::byte{'X'}, std::byte{'E'}, std::byte{'X'}, opts.use_xex1_format ? std::byte{'1'} : std::byte{'2'}});
  header.append_be32(opts.module_flags);
  const auto header_size_field = header.append_be32(0u);  // patched at the end.
  header.append_be32(0u);                                 // reserved
  const auto security_offset_field = header.append_be32(0u);
  std::uint32_t opt_count = 3u;  // execution info, original pe name, file format info
  if (opts.with_import_libraries) opt_count += 1u;
  if (opts.with_native_tls) opt_count += 1u;
  if (opts.default_stack_size != 0u) opt_count += 1u;
  if (opts.with_delta_patch_descriptor) opt_count += 1u;
  opt_count += 1u;  // entry point (inline)
  header.append_be32(opt_count);

  // Optional header table: reserve slots, patch key/offset pairs once every
  // structure's location is known.
  const auto opt_table_offset = header.size();
  for (std::uint32_t i = 0; i < opt_count; ++i) {
    header.append_be32(0u);
    header.append_be32(0u);
  }
  std::size_t slot = 0;
  const auto set_slot = [&](std::uint32_t key, std::uint32_t value_or_offset) {
    header.put_be32(opt_table_offset + slot * 8u, key);
    header.put_be32(opt_table_offset + slot * 8u + 4u, value_or_offset);
    ++slot;
  };

  // Entry point (inline).
  set_slot(kEntryPointKey, kImageBase + pe.entry_point_rva);
  if (opts.default_stack_size != 0u) set_slot(kDefaultStackSizeKey, opts.default_stack_size);

  // Execution info.
  const auto execution_info_offset = header.size();
  header.append_be32(opts.media_id);
  header.append_be32(0x00010001u);  // version 1.1.0.0-ish
  header.append_be32(0x00010001u);  // base_version
  header.append_be32(opts.title_id);
  header.append_zeros(4);  // platform/executable_table/disc_number/disc_count
  header.append_be32(0u);  // savegame_id
  set_slot(kExecutionInfoKey, static_cast<std::uint32_t>(execution_info_offset));

  // Original PE name.
  const auto pe_name_offset = header.size();
  const std::string pe_name = "default.exe";
  const auto record_size_field = header.append_be32(static_cast<std::uint32_t>(4u + pe_name.size() + 1u));
  header.append_string(pe_name);
  header.align(4);
  (void)record_size_field;
  set_slot(kOriginalPeNameKey, static_cast<std::uint32_t>(pe_name_offset));

  // Native TLS.
  if (opts.with_native_tls) {
    const auto tls_offset = header.size();
    header.append_be32(1u);       // slot
    header.append_be32(0x3000u);  // raw_data_address (RVA-ish, unused by tests)
    header.append_be32(0x100u);   // data_size
    header.append_be32(0x100u);   // raw_data_size
    set_slot(kTlsInfoKey, static_cast<std::uint32_t>(tls_offset));
  }

  // Import libraries: one library "xboxkrnl.exe" with two imports.
  if (opts.with_import_libraries) {
    const auto import_libraries_offset = header.size();
    const auto table_size_field = header.append_be32(0u);
    const std::string lib_name = "xboxkrnl.exe";
    const auto string_table_size_field = header.append_be32(0u);
    header.append_be32(1u);  // string_table.count
    const auto string_table_start = header.size();
    header.append_string(lib_name);
    header.align(4);
    const auto string_table_size = header.size() - string_table_start;

    const auto library_offset = header.size();
    const auto library_size_field = header.append_be32(0u);
    header.append_zeros(20);  // next_import_digest
    header.append_be32(0xFFFFFFFFu);  // id
    header.append_be32(0u);           // version
    header.append_be32(0u);           // version_min
    header.append_be16(0u);           // name_index
    header.append_be16(2u);           // count
    // Two import_table entries: guest addresses inside .data (RW, safe to
    // hold a 4-byte placeholder value without affecting code/behaviour).
    const auto entry0_addr = kImageBase + pe.data_rva + 0x10u;
    const auto entry1_addr = kImageBase + pe.data_rva + 0x14u;
    header.append_be32(entry0_addr);
    header.append_be32(entry1_addr);
    const auto library_size = header.size() - library_offset;
    header.put_be32(library_size_field, static_cast<std::uint32_t>(library_size));

    const auto table_size = header.size() - import_libraries_offset;
    header.put_be32(table_size_field, static_cast<std::uint32_t>(table_size));
    header.put_be32(string_table_size_field, static_cast<std::uint32_t>(string_table_size));
    set_slot(kImportLibrariesKey, static_cast<std::uint32_t>(import_libraries_offset));

    // The placeholder values at entry0_addr/entry1_addr (ordinal | attrs<<16)
    // are patched into the PE body by the caller via patch_import_placeholders().
  }

  // File format info (compression=None, encryption=None by default; callers
  // needing another combination patch these fields via transform_body()).
  const auto file_format_info_offset = header.size();
  header.append_be32(8u);  // info_size (no compression_info payload yet)
  header.append_be16(0u);  // encryption = None
  header.append_be16(0u);  // compression = None
  set_slot(kFileFormatInfoKey, static_cast<std::uint32_t>(file_format_info_offset));

  // Delta patch descriptor (title-update tests only). Real on-disk layout:
  // 0x4C bytes of fixed fields, immediately followed by the embedded
  // xex2_delta_patch record's raw bytes (12-byte fixed header + variable
  // payload) - see reconstruct_patch_header_bytes() in xex_loader.cpp.
  if (opts.with_delta_patch_descriptor) {
    const auto delta_offset = header.size();
    header.append_be32(0x4Cu);  // size
    header.append_be32(opts.delta_target_version);
    header.append_be32(opts.delta_source_version);
    header.append_bytes(opts.delta_base_signature_digest);
    header.append_zeros(16u);  // image_key_source (unused by these tests)
    header.append_be32(opts.delta_size_of_target_headers);
    header.append_be32(opts.delta_headers_source_offset);
    header.append_be32(opts.delta_headers_source_size);
    header.append_be32(opts.delta_headers_target_offset);
    header.append_be32(opts.delta_image_source_offset);
    header.append_be32(opts.delta_image_source_size);
    header.append_be32(opts.delta_image_target_offset);
    header.append_bytes(opts.header_patch_record_bytes);
    set_slot(kDeltaPatchDescriptorKey, static_cast<std::uint32_t>(delta_offset));
  }

  assert(slot == opt_count);

  // Security info, right after the optional structures, 16-byte aligned.
  header.align(16);
  const auto security_offset = header.size();
  header.put_be32(security_offset_field, static_cast<std::uint32_t>(security_offset));
  const auto page_size = kPageSize4KB;
  const auto image_size = static_cast<std::uint32_t>(pe.bytes.size());
  const auto page_count = (image_size + page_size - 1u) / page_size;

  const auto security_fixed_size = opts.use_xex1_format ? 0x168u : 0x184u;
  header.append_be32(static_cast<std::uint32_t>(security_fixed_size + page_count * 24u));  // security header_size
  header.append_be32(image_size);

  // security_info.section_digest ("ImageHash") is a real, verified
  // SHA1(first_page_bytes, first_page_descriptor_entry) integrity digest
  // (see verify_first_page_digest() in xex_loader.cpp) - the loader now
  // rejects a body whose decrypted/decompressed effective image doesn't
  // match it, so fixtures must supply the genuine value rather than zeros.
  // The first page-descriptor run below is always page_count=1,
  // type=ReadOnlyData(3) (page index 0 is never the .text range).
  const auto first_page_digest = [&] {
    constexpr std::uint32_t kFirstDescriptorValue = (1u << 4u) | 3u;
    std::array<std::byte, 24> descriptor_bytes{};
    descriptor_bytes[0] = static_cast<std::byte>((kFirstDescriptorValue >> 24) & 0xFFu);
    descriptor_bytes[1] = static_cast<std::byte>((kFirstDescriptorValue >> 16) & 0xFFu);
    descriptor_bytes[2] = static_cast<std::byte>((kFirstDescriptorValue >> 8) & 0xFFu);
    descriptor_bytes[3] = static_cast<std::byte>(kFirstDescriptorValue & 0xFFu);
    const auto first_run_bytes = std::min<std::size_t>(kPageSize4KB, pe.bytes.size());
    std::vector<std::byte> hashed(pe.bytes.begin(), pe.bytes.begin() + first_run_bytes);
    hashed.insert(hashed.end(), descriptor_bytes.begin(), descriptor_bytes.end());
    return xbox::crypto::sha1(hashed);
  }();

  if (opts.use_xex1_format) {
    // xex1::SecurityInfo layout (0x168 bytes total): see
    // kXex1SecurityInfoFixedSize in xex_loader.cpp.
    header.append_zeros(0x100u);      // Signature (rsa_signature)
    header.append_zeros(20u);         // ImportDigest (import_table_digest)
    header.append_bytes(first_page_digest);  // ImageHash (section_digest)
    header.append_be32(kImageBase);   // LoadAddress
    header.append_zeros(16u);         // ImageKey (encrypted_image_key)
    header.append_zeros(16u);         // MediaID (xgd2_media_id)
    header.append_be32(0u);           // GameRegion (region)
    header.append_be32(0x10000000u);  // ImageFlags: 4KB pages
    header.append_be32(0u);           // RootImportAddress
    header.append_be32(0xFFFFFFFFu);  // AllowedMediaTypes
    header.append_be32(page_count);   // PageDescriptorCount
  } else {
    header.append_zeros(0x100u);  // rsa_signature
    header.append_zeros(4u);      // unk_108 (unused reserved field)
    header.append_be32(0x10000000u);  // image_flags: 4KB pages
    header.append_be32(kImageBase);   // load_address
    header.append_bytes(first_page_digest);  // section_digest
    header.append_be32(2u);           // import_table_count
    header.append_zeros(20u);         // import_table_digest
    header.append_zeros(16u);         // xgd2_media_id
    header.append_zeros(16u);         // encrypted_image_key (patched by transform_body() if encrypted)
    header.append_be32(0u);           // export_table
    header.append_zeros(20u);         // header_digest
    header.append_be32(0u);           // region
    header.append_be32(0xFFFFFFFFu);  // allowed_media_types
    header.append_be32(page_count);   // page_descriptor_count
  }
  // One page-descriptor run per 4 KiB page. Each kSectionSize (64 KiB)
  // section spans kSectionSize/kPageSize4KB = 16 descriptor pages, in order:
  // [0,16)=headers, [16,32)=.text, [32,48)=.rdata, [48,64)=.data. The .text
  // range is marked Code; every other page - notably .data, which the PE
  // header itself characterizes as R+W - is marked ReadOnlyData,
  // deliberately diverging from the PE section characteristics so tests can
  // prove page descriptors win.
  constexpr std::uint32_t kPagesPerSection = kSectionSize / kPageSize4KB;
  for (std::uint32_t i = 0; i < page_count; ++i) {
    const bool is_text_page = i >= kPagesPerSection && i < kPagesPerSection * 2u;
    const auto type = is_text_page ? 1u /*code*/ : 3u /*read-only data*/;
    header.append_be32((1u << 4u) | type);  // page_count=1, type=type
    header.append_zeros(20u);               // data_digest
  }

  header.align(16);
  const auto final_header_size = header.size();
  header.put_be32(header_size_field, static_cast<std::uint32_t>(final_header_size));

  auto header_bytes = header.take();
  XexBuildResult result;
  result.header_size = final_header_size;
  result.file_format_info_offset = file_format_info_offset;
  result.execution_info_offset = execution_info_offset;
  result.plain_body = pe.bytes;
  result.file = header_bytes;
  result.file.insert(result.file.end(), pe.bytes.begin(), pe.bytes.end());
  return result;
}

// Patches two native-import placeholder values directly into the PE body.
// Native XEX import records encode the ordinal in bits 0..15, attributes in
// bits 16..23 and the record type in bits 24..31 (0=address/variable,
// 1=function thunk). Existing fixtures default both records to type 0; tests
// that exercise function-import pairing explicitly request a type-1 second
// record with the same ordinal.
inline void patch_import_placeholders(std::vector<std::byte>& pe_bytes, std::uint32_t data_rva,
                              std::uint16_t ordinal0, std::uint16_t ordinal1,
                              std::uint8_t record_type0 = 0u,
                              std::uint8_t record_type1 = 0u) {
  const auto patch_be32 = [&](std::size_t file_offset, std::uint32_t value) {
    pe_bytes[file_offset + 0] = static_cast<std::byte>((value >> 24) & 0xFFu);
    pe_bytes[file_offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFu);
    pe_bytes[file_offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFu);
    pe_bytes[file_offset + 3] = static_cast<std::byte>(value & 0xFFu);
  };
  const auto encoded = [](std::uint16_t ordinal, std::uint8_t attributes,
                          std::uint8_t record_type) {
    return static_cast<std::uint32_t>(ordinal) |
           (static_cast<std::uint32_t>(attributes) << 16u) |
           (static_cast<std::uint32_t>(record_type) << 24u);
  };
  // .data section's raw file offset equals its RVA offset in build_pe_body().
  patch_be32(data_rva + 0x10u, encoded(ordinal0, 0u, record_type0));
  patch_be32(data_rva + 0x14u, encoded(ordinal1, 7u, record_type1));
}

// Wraps a raw LZX bitstream in the XEX xex2_compressed_block_info container:
// one block whose SHA1-hashed payload is the chunk-length-prefixed region
// (2-byte chunk size + chunk bytes + 2-byte zero terminator), preceded by the
// block's own 4-byte size + 20-byte hash header, followed by the block-chain
// terminator (block_size=0). If `corrupt_hash` is set, the stored digest is
// perturbed so the loader must reject the block.
inline std::vector<std::byte> build_compressed_block_chain(std::span<const std::byte> lzx_stream,
                                                    bool corrupt_hash = false) {
  // Each container "chunk" is length-prefixed with a 16-bit field, so chunks
  // over 0xFFFF bytes must be split; degather_compressed_blocks() just
  // concatenates chunk payloads back together, so any split point is safe.
  constexpr std::size_t kMaxChunkBytes = 0x8000u;
  ByteWriter payload_writer;
  for (std::size_t offset = 0; offset < lzx_stream.size();) {
    const auto chunk_size = std::min(kMaxChunkBytes, lzx_stream.size() - offset);
    payload_writer.append_be16(static_cast<std::uint16_t>(chunk_size));
    payload_writer.append_bytes(lzx_stream.subspan(offset, chunk_size));
    offset += chunk_size;
  }
  payload_writer.append_be16(0u);  // end-of-block chunk terminator.
  const auto payload = payload_writer.take();

  auto digest = xenon::xbox::crypto::sha1(payload);
  if (corrupt_hash) digest[0] ^= std::byte{0xFF};

  ByteWriter chain;
  chain.append_be32(static_cast<std::uint32_t>(24u + payload.size()));
  chain.append_bytes(digest);
  chain.append_bytes(payload);
  chain.append_be32(0u);  // block-chain terminator.
  return chain.take();
}

// Wraps a raw xex2_delta_patch record-chain byte sequence in the same
// xex2_compressed_block_info outer container as build_compressed_block_chain()
// above, but *without* the inner 2-byte-length-prefixed chunk-table framing:
// XEX_COMPRESSION_DELTA's block payload is the record chain directly (see
// apply_image_delta()/walk_compressed_block_chain() in xex_loader.cpp),
// unlike XEX_COMPRESSION_NORMAL's chunk-table-wrapped continuous bitstream.
inline std::vector<std::byte> build_delta_block_chain(std::span<const std::byte> records,
                                               bool corrupt_hash = false) {
  auto digest = xenon::xbox::crypto::sha1(records);
  if (corrupt_hash) digest[0] ^= std::byte{0xFF};

  ByteWriter chain;
  chain.append_be32(static_cast<std::uint32_t>(24u + records.size()));
  chain.append_bytes(digest);
  chain.append_bytes(records);
  chain.append_be32(0u);  // block-chain terminator.
  return chain.take();
}

// Builds one xex2_delta_patch record's raw on-disk bytes: a 12-byte fixed
// header (old_addr, new_addr, uncompressed_len, compressed_len) optionally
// followed by compressed_len bytes of payload.
inline std::vector<std::byte> build_delta_patch_record(std::uint32_t old_addr, std::uint32_t new_addr,
                                                std::uint16_t uncompressed_len, std::uint16_t compressed_len,
                                                std::span<const std::byte> payload = {}) {
  ByteWriter w;
  w.append_be32(old_addr);
  w.append_be32(new_addr);
  w.append_be16(uncompressed_len);
  w.append_be16(compressed_len);
  w.append_bytes(payload);
  return w.take();
}


// Rebuilds `fixture`'s header with a new FileFormatInfo body (the bytes
// immediately after the FileFormatInfo key/offset slot's 8-byte legacy
// struct - i.e. info_size/encryption/compression/[window_size]),
// adjusting header_size/security_offset for the resulting size delta.
// Shared by every test below that needs a non-default encryption/
// compression combination (mirrors the inline pattern the pre-existing
// compression tests each hand-rolled).
inline std::vector<std::byte> rebuild_header_with_file_format_info(const XexBuildResult& fixture,
                                                                    std::span<const std::byte> ffi_body) {
  ByteWriter header;
  header.append_bytes(std::span<const std::byte>(fixture.file.data(), fixture.file_format_info_offset));
  header.append_bytes(ffi_body);
  const auto tail_start = fixture.file_format_info_offset + 8u;  // Original FFI struct was 8 bytes.
  header.append_bytes(std::span<const std::byte>(fixture.file.data() + tail_start,
                                                 fixture.header_size - tail_start));
  auto header_bytes = header.take();
  const auto size_delta = header_bytes.size() - fixture.header_size;

  const auto new_header_size = static_cast<std::uint32_t>(header_bytes.size());
  header_bytes[8] = static_cast<std::byte>((new_header_size >> 24) & 0xFFu);
  header_bytes[9] = static_cast<std::byte>((new_header_size >> 16) & 0xFFu);
  header_bytes[10] = static_cast<std::byte>((new_header_size >> 8) & 0xFFu);
  header_bytes[11] = static_cast<std::byte>(new_header_size & 0xFFu);

  const auto orig_security_offset =
      static_cast<std::uint32_t>(fixture.file[0x10]) << 24 | static_cast<std::uint32_t>(fixture.file[0x11]) << 16 |
      static_cast<std::uint32_t>(fixture.file[0x12]) << 8 | static_cast<std::uint32_t>(fixture.file[0x13]);
  const auto new_security_offset = orig_security_offset + static_cast<std::uint32_t>(size_delta);
  header_bytes[0x10] = static_cast<std::byte>((new_security_offset >> 24) & 0xFFu);
  header_bytes[0x11] = static_cast<std::byte>((new_security_offset >> 16) & 0xFFu);
  header_bytes[0x12] = static_cast<std::byte>((new_security_offset >> 8) & 0xFFu);
  header_bytes[0x13] = static_cast<std::byte>(new_security_offset & 0xFFu);
  return header_bytes;
}

}  // namespace xenon::test::xex
