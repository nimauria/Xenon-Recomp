#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_loader.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace {

using namespace xenon;

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

PeBuildResult build_pe_body(std::uint32_t image_base, bool with_export) {
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

XexBuildResult build_base_xex(const PeBuildResult& pe, const XexBuildOptions& opts) {
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
void patch_import_placeholders(std::vector<std::byte>& pe_bytes, std::uint32_t data_rva,
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
std::vector<std::byte> build_compressed_block_chain(std::span<const std::byte> lzx_stream,
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
std::vector<std::byte> build_delta_block_chain(std::span<const std::byte> records,
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
std::vector<std::byte> build_delta_patch_record(std::uint32_t old_addr, std::uint32_t new_addr,
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

}  // namespace

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

namespace {

void test_malformed_xex() {
  std::vector<std::byte> bad = {std::byte{'B'}, std::byte{'A'}, std::byte{'D'}};
  xbox::XexImage image{};
  std::string error;
  assert(!xbox::parse_xex_image(bad, image, &error));
  assert(!error.empty());
}

void test_invalid_header_size() {
  std::vector<std::byte> bytes(0x18u, std::byte{0});
  bytes[0] = std::byte{'X'};
  bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'};
  bytes[3] = std::byte{'2'};
  bytes[8] = std::byte{0xFF};
  xbox::XexImage image{};
  std::string error;
  assert(!xbox::parse_xex_image(bytes, image, &error));
}

void test_missing_security_info_rejected() {
  std::vector<std::byte> bytes(0x20u, std::byte{0});
  bytes[0] = std::byte{'X'};
  bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'};
  bytes[3] = std::byte{'2'};
  bytes[8] = std::byte{0x20};  // header_size = 0x20, security_offset = 0 (invalid)
  xbox::XexImage image{};
  std::string error;
  assert(!xbox::parse_xex_image(bytes, image, &error));
  assert(!error.empty());
}

XexBuildResult make_uncompressed_fixture(bool with_export = true) {
  auto pe = build_pe_body(kImageBase, with_export);
  patch_import_placeholders(pe.bytes, pe.data_rva, /*ordinal0=*/0x0042u, /*ordinal1=*/0x0099u);
  XexBuildOptions opts{};
  return build_base_xex(pe, opts);
}

void test_pe_section_virtual_size_uses_field_after_full_eight_byte_name() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);

  // IMAGE_SECTION_HEADER is 40 bytes. Name occupies bytes [0, 8), then
  // Misc.VirtualSize is the DWORD at +0x08. An exactly-eight-character
  // section name catches regressions where VirtualSize is accidentally read
  // from +0x04: the last four name bytes of ".XBMOVIE" spell "OVIE" and
  // decode little-endian as 0x4549564F.
  constexpr std::size_t kFirstSectionHeaderOffset = 0x178u;
  constexpr std::uint32_t kExpectedVirtualSize = 0x00012345u;
  constexpr std::array<char, 8> kEightByteName{'.', 'X', 'B', 'M', 'O', 'V', 'I', 'E'};
  for (std::size_t i = 0; i < kEightByteName.size(); ++i) {
    pe.bytes[kFirstSectionHeaderOffset + i] = static_cast<std::byte>(kEightByteName[i]);
  }
  const auto put_le32 = [&](std::size_t offset, std::uint32_t value) {
    pe.bytes[offset + 0u] = static_cast<std::byte>(value & 0xFFu);
    pe.bytes[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xFFu);
    pe.bytes[offset + 2u] = static_cast<std::byte>((value >> 16u) & 0xFFu);
    pe.bytes[offset + 3u] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  };
  put_le32(kFirstSectionHeaderOffset + 0x08u, kExpectedVirtualSize);

  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);

  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(!image.sections.empty());
  assert(image.sections.front().name == ".XBMOVIE");
  assert(image.sections.front().virtual_size == kExpectedVirtualSize);
  assert(image.sections.front().virtual_size != 0x4549564Fu);
}

void test_loaded_image_rva_semantics_ignore_raw_pointer() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/true);
  constexpr std::uint32_t kSectionRva = 0x00090000u;
  constexpr std::uint32_t kRawPointer = 0x0008C800u;
  constexpr std::uint32_t kEntryRva = 0x001F5ED0u;
  constexpr std::uint32_t kVirtualSize = 0x00340C6Cu;
  constexpr std::uint32_t kRawSize = 0x00340E00u;
  constexpr std::size_t kFirstSectionHeaderOffset = 0x178u;
  constexpr std::size_t kAddressOfEntryPointOffset = 0xA8u;

  pe.bytes.resize(0x00400000u, std::byte{0});
  const auto put_le32 = [&](std::size_t offset, std::uint32_t value) {
    pe.bytes[offset + 0u] = static_cast<std::byte>(value & 0xFFu);
    pe.bytes[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xFFu);
    pe.bytes[offset + 2u] = static_cast<std::byte>((value >> 16u) & 0xFFu);
    pe.bytes[offset + 3u] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  };
  put_le32(kFirstSectionHeaderOffset + 0x08u, kVirtualSize);
  put_le32(kFirstSectionHeaderOffset + 0x0Cu, kSectionRva);
  put_le32(kFirstSectionHeaderOffset + 0x10u, kRawSize);
  put_le32(kFirstSectionHeaderOffset + 0x14u, kRawPointer);
  put_le32(kAddressOfEntryPointOffset, kEntryRva);
  pe.entry_point_rva = kEntryRva;

  const auto put_be32 = [&](std::size_t offset, std::uint32_t value) {
    pe.bytes[offset + 0u] = static_cast<std::byte>((value >> 24u) & 0xFFu);
    pe.bytes[offset + 1u] = static_cast<std::byte>((value >> 16u) & 0xFFu);
    pe.bytes[offset + 2u] = static_cast<std::byte>((value >> 8u) & 0xFFu);
    pe.bytes[offset + 3u] = static_cast<std::byte>(value & 0xFFu);
  };
  const auto entry_raw_offset = static_cast<std::size_t>(kRawPointer) +
                                (kEntryRva - kSectionRva);
  put_be32(entry_raw_offset, 0x7C003C0Eu);
  put_be32(kEntryRva, 0x7D8802A6u);

  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);

  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(image.entry_point == kImageBase + kEntryRva);
  assert(std::to_integer<unsigned char>(image.effective_image[entry_raw_offset]) == 0x7C);
  assert(std::to_integer<unsigned char>(image.effective_image[entry_raw_offset + 1u]) == 0x00);
  assert(std::to_integer<unsigned char>(image.effective_image[entry_raw_offset + 2u]) == 0x3C);
  assert(std::to_integer<unsigned char>(image.effective_image[entry_raw_offset + 3u]) == 0x0E);
  assert(image.effective_image[kEntryRva] == std::byte{0x7D});
  assert(image.sections.front().raw_pointer == kRawPointer);
  const auto entry_delta = static_cast<std::size_t>(kEntryRva - kSectionRva);
  assert(image.sections.front().bytes.size() > entry_delta + 3u);
  assert(std::to_integer<unsigned char>(image.sections.front().bytes[entry_delta]) == 0x7D);
  assert(std::to_integer<unsigned char>(image.sections.front().bytes[entry_delta + 1u]) == 0x88);
  assert(std::to_integer<unsigned char>(image.sections.front().bytes[entry_delta + 2u]) == 0x02);
  assert(std::to_integer<unsigned char>(image.sections.front().bytes[entry_delta + 3u]) == 0xA6);
  assert(std::to_integer<unsigned char>(image.effective_image[kEntryRva]) == 0x7D);
  assert(std::to_integer<unsigned char>(image.effective_image[kEntryRva + 1u]) == 0x88);
  assert(std::to_integer<unsigned char>(image.effective_image[kEntryRva + 2u]) == 0x02);
  assert(std::to_integer<unsigned char>(image.effective_image[kEntryRva + 3u]) == 0xA6);
}

void test_relocation_entries_are_inline_in_directory() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  constexpr std::uint32_t kRelocationRva = 0x00050000u;
  constexpr std::uint32_t kTargetPageRva = 0x00090000u;
  pe.bytes.resize(0x00060000u, std::byte{0});
  const auto put_le32 = [&](std::size_t offset, std::uint32_t value) {
    pe.bytes[offset + 0u] = static_cast<std::byte>(value & 0xFFu);
    pe.bytes[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xFFu);
    pe.bytes[offset + 2u] = static_cast<std::byte>((value >> 16u) & 0xFFu);
    pe.bytes[offset + 3u] = static_cast<std::byte>((value >> 24u) & 0xFFu);
  };
  const auto put_le16 = [&](std::size_t offset, std::uint16_t value) {
    pe.bytes[offset + 0u] = static_cast<std::byte>(value & 0xFFu);
    pe.bytes[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xFFu);
  };
  constexpr std::size_t kDataDirectoryOffset = 0xF8u;
  put_le32(kDataDirectoryOffset + 5u * 8u, kRelocationRva);
  put_le32(kDataDirectoryOffset + 5u * 8u + 4u, 12u);
  put_le32(kRelocationRva + 0u, kTargetPageRva);
  put_le32(kRelocationRva + 4u, 12u);
  put_le16(kRelocationRva + 8u, 0x3123u);
  put_le16(kRelocationRva + 10u, 0u);

  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);
  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(image.relocations.size() == 1u);
  assert(image.relocations.front().virtual_address == kTargetPageRva);
  assert(image.relocations.front().entries.size() == 1u);
  assert(image.relocations.front().entries.front() == 0x30123u);
}

void test_parse_and_load_uncompressed_unencrypted() {
  auto fixture = make_uncompressed_fixture();

  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(image.format == xbox::XexFormat::Xex2);
  assert(image.title_id == 0x41415858u);
  assert(image.media_id == 0x12345678u);
  assert(image.image_base == kImageBase);
  assert(image.entry_point == kImageBase + kSectionSize);
  assert(image.original_pe_name == "default.exe");
  assert(image.tls.has_value());
  assert(image.tls->slot == 1u);
  assert(!image.sections.empty());
  assert(!image.exports.empty());
  assert(image.exports.front().name == "XexTestExport");

  // Native import-library parsing: two imports from xboxkrnl.exe with the
  // ordinals patched into the image at those guest addresses.
  assert(image.imports.size() == 2u);
  bool saw_42 = false, saw_99 = false;
  for (const auto& imp : image.imports) {
    assert(imp.module == "xboxkrnl.exe");
    if (imp.ordinal == 0x0042u) saw_42 = true;
    if (imp.ordinal == 0x0099u) saw_99 = true;
  }
  assert(saw_42 && saw_99);

  // Page descriptors: first page is Code (RX), the rest ReadOnlyData (R),
  // deliberately different from the PE section characteristics - this
  // proves page-descriptor protection wins.
  const auto& text_section = image.sections.front();
  assert(text_section.executable);
  assert(!text_section.writable);

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());
  xbox::LoadedXex loaded{};
  const bool load_ok = xbox::load_xex(address_space, fixture.file, loaded, memory::kXex64KBase, &error);
  assert(load_ok && error.empty());
  assert(loaded.loaded);
  assert(!loaded.executable_ranges.empty());
  assert(address_space.read8(kImageBase + kSectionSize) == 0x4Eu);
}

// map_xex_image() is load_xex()'s mapping half, factored out so a caller
// with an already-parsed/patched XexImage (XenonSession, after
// apply_title_update()) can map it without re-serializing back to file
// bytes. This proves the refactor is behavior-preserving: parsing then
// mapping separately must produce the same result as load_xex()'s single
// call.
void test_native_function_import_pair_is_classified_without_rewriting_address_slot() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  constexpr std::uint16_t kOrdinal = 0x0042u;
  patch_import_placeholders(pe.bytes, pe.data_rva, kOrdinal, kOrdinal,
                            /*record_type0=*/0u, /*record_type1=*/1u);
  XexBuildOptions opts{};
  auto fixture = build_base_xex(pe, opts);

  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(image.imports.size() == 2u);

  const auto address_slot = kImageBase + pe.data_rva + 0x10u;
  const auto callable_thunk = kImageBase + pe.data_rva + 0x14u;
  const auto* slot = static_cast<const xbox::XexImport*>(nullptr);
  const auto* thunk = static_cast<const xbox::XexImport*>(nullptr);
  for (const auto& import : image.imports) {
    assert(import.module == "xboxkrnl.exe");
    assert(import.ordinal == kOrdinal);
    if (import.kind == xbox::XexImportKind::FunctionAddress) slot = &import;
    if (import.kind == xbox::XexImportKind::FunctionThunk) thunk = &import;
  }
  assert(slot != nullptr && thunk != nullptr);
  assert(slot->guest_thunk == address_slot);
  assert(thunk->guest_thunk == callable_thunk);
  assert(!slot->callable());
  assert(thunk->callable());

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());
  xbox::LoadedXex loaded{};
  assert(xbox::map_xex_image(address_space, image, loaded, memory::kXex64KBase, &error));
  assert(error.empty());

  // Mapping preserves the native import records. The type-0 half of a kernel
  // function import is not a callable function pointer; only the type-1 thunk
  // is dispatched as an external call.
  assert(address_space.read32_be(address_slot) ==
         static_cast<std::uint32_t>(kOrdinal));
  // The callable thunk likewise remains encoded; execution is intercepted by
  // RuntimeServices::call rather than interpreted as PPC code.
  assert(address_space.read32_be(callable_thunk) ==
         (0x01000000u | (7u << 16u) | static_cast<std::uint32_t>(kOrdinal)));
}

void test_map_xex_image_rounds_section_size_up_to_memory_page() {
  xbox::XexImage image{};
  xbox::XexSection section{};
  section.name = ".unaligned";
  section.virtual_address = memory::kXex64KBase + 0x10000u + 0x1234u;
  section.virtual_size = 0x173A4u;
  section.raw_size = 0x17000u;
  section.protect = memory::kReadExecute;
  section.executable = true;
  section.readable = true;
  section.bytes.assign(section.raw_size, std::byte{0x5A});
  image.sections.push_back(section);

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());

  xbox::LoadedXex loaded{};
  std::string error;
  assert(xbox::map_xex_image(address_space, image, loaded,
                             memory::kXex64KBase, &error));
  assert(error.empty());
  assert(loaded.loaded);
  assert(loaded.mapped_sections.size() == 1u);
  assert(loaded.executable_ranges.size() == 1u);

  const auto expected_base = memory::kXex64KBase + 0x10000u;
  constexpr std::uint32_t expected_size = 0x20000u;
  const auto mapping = address_space.query(section.virtual_address);
  assert(mapping.has_value());
  assert(mapping->state == memory::PageState::Committed);
  assert(mapping->allocation_base == expected_base);
  assert(mapping->allocation_size == expected_size);
  assert(loaded.executable_ranges.front().begin == expected_base);
  assert(loaded.executable_ranges.front().end == expected_base + expected_size);
  assert(address_space.read8(section.virtual_address) == 0x5Au);
  assert(address_space.read8(section.virtual_address + section.raw_size - 1u) == 0x5Au);
}

void test_map_xex_image_merges_sections_that_share_allocation_pages() {
  xbox::XexImage image{};

  xbox::XexSection text{};
  text.name = ".text";
  text.virtual_address = memory::kXex64KBase + 0x10000u;
  text.virtual_size = 0x18000u;
  text.raw_size = 0x18000u;
  text.protect = memory::kReadExecute;
  text.executable = true;
  text.readable = true;
  text.bytes.assign(text.raw_size, std::byte{0x11});
  image.sections.push_back(text);

  // Distinct byte range, but its 64 KiB-aligned mapping shares the 0x20000
  // allocation page with .text. This mirrors real XEX layouts such as a
  // .pdata section beginning part-way through the final .text allocation
  // page. Mapping sections independently would reject the second reserve.
  xbox::XexSection pdata{};
  pdata.name = ".pdata";
  pdata.virtual_address = memory::kXex64KBase + 0x29E00u;
  pdata.virtual_size = 0x6174u;
  pdata.raw_size = 0x10000u;
  pdata.protect = memory::Protect::Read;
  pdata.readable = true;
  pdata.bytes.assign(pdata.raw_size, std::byte{0x22});
  image.sections.push_back(pdata);

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());

  xbox::LoadedXex loaded{};
  std::string error;
  assert(xbox::map_xex_image(address_space, image, loaded,
                             memory::kXex64KBase, &error));
  assert(error.empty());
  assert(loaded.loaded);
  assert(loaded.mapped_sections.size() == 2u);

  const auto text_mapping = address_space.query(text.virtual_address);
  const auto pdata_mapping = address_space.query(pdata.virtual_address);
  assert(text_mapping.has_value());
  assert(pdata_mapping.has_value());
  assert(text_mapping->allocation_base == memory::kXex64KBase + 0x10000u);
  assert(pdata_mapping->allocation_base == text_mapping->allocation_base);
  assert(text_mapping->allocation_size == 0x30000u);
  assert(pdata_mapping->allocation_size == 0x30000u);

  // Both sections survive at their actual byte addresses even though their
  // backing allocation pages overlap.
  assert(address_space.read8(text.virtual_address) == 0x11u);
  assert(address_space.read8(text.virtual_address + text.raw_size - 1u) == 0x11u);
  assert(address_space.read8(pdata.virtual_address) == 0x22u);
  assert(address_space.read8(pdata.virtual_address + pdata.raw_size - 1u) == 0x22u);

  // The shared page must carry the union required by the sections occupying
  // it, while the final .pdata-only page remains read-only.
  const auto shared_page = address_space.query(memory::kXex64KBase + 0x20000u);
  const auto pdata_only_page = address_space.query(memory::kXex64KBase + 0x30000u);
  assert(shared_page.has_value());
  assert(pdata_only_page.has_value());
  assert(memory::has(shared_page->current_protect, memory::Protect::Read));
  assert(memory::has(shared_page->current_protect, memory::Protect::Execute));
  assert(!memory::has(shared_page->current_protect, memory::Protect::Write));
  assert(pdata_only_page->current_protect == memory::Protect::Read);
}

void test_map_xex_image_rolls_back_partial_mapping_failure() {
  xbox::XexImage image{};

  xbox::XexSection first{};
  first.name = ".first";
  first.virtual_address = memory::kXex64KBase + 0x10000u;
  first.virtual_size = 0x10000u;
  first.raw_size = 0x10000u;
  first.protect = memory::kReadWrite;
  first.readable = true;
  first.writable = true;
  first.bytes.assign(first.raw_size, std::byte{0xA5});
  image.sections.push_back(first);

  xbox::XexSection conflicting{};
  conflicting.name = ".conflict";
  conflicting.virtual_address = memory::kXex64KBase + 0x40000u;
  conflicting.virtual_size = 0x10000u;
  conflicting.raw_size = 0x10000u;
  conflicting.protect = memory::Protect::Read;
  conflicting.readable = true;
  conflicting.bytes.assign(conflicting.raw_size, std::byte{0xCC});
  image.sections.push_back(conflicting);

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());

  // Pre-existing guest allocation forces the second merged image allocation
  // to fail. The mapper must release only allocations it created earlier in
  // this call and must leave this external reservation untouched.
  assert(address_space.reserve_fixed(conflicting.virtual_address, 0x10000u,
                                     memory::Protect::Read));

  xbox::LoadedXex loaded{};
  std::string error;
  assert(!xbox::map_xex_image(address_space, image, loaded,
                              memory::kXex64KBase, &error));
  assert(!error.empty());
  assert(error.find(".conflict") != std::string::npos);
  assert(error.find("reserve") != std::string::npos);
  assert(!loaded.loaded);
  assert(loaded.mapped_sections.empty());
  assert(loaded.executable_ranges.empty());

  const auto first_mapping = address_space.query(first.virtual_address);
  assert(first_mapping.has_value());
  assert(first_mapping->state == memory::PageState::Free);

  const auto conflict_mapping = address_space.query(conflicting.virtual_address);
  assert(conflict_mapping.has_value());
  assert(conflict_mapping->state == memory::PageState::Reserved);
}

void test_map_xex_image_matches_load_xex() {
  auto fixture = make_uncompressed_fixture();

  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());

  memory::AddressSpace direct_space(memory::GuestTranslationMode::Compact);
  assert(direct_space.initialize());
  xbox::LoadedXex direct_loaded{};
  assert(xbox::load_xex(direct_space, fixture.file, direct_loaded, memory::kXex64KBase, &error));

  memory::AddressSpace mapped_space(memory::GuestTranslationMode::Compact);
  assert(mapped_space.initialize());
  xbox::LoadedXex mapped_loaded{};
  assert(xbox::map_xex_image(mapped_space, image, mapped_loaded, memory::kXex64KBase, &error));

  assert(direct_loaded.loaded && mapped_loaded.loaded);
  assert(direct_loaded.image_base == mapped_loaded.image_base);
  assert(direct_loaded.mapped_sections.size() == mapped_loaded.mapped_sections.size());
  assert(direct_loaded.executable_ranges.size() == mapped_loaded.executable_ranges.size());
  assert(mapped_space.read8(kImageBase + kSectionSize) == direct_space.read8(kImageBase + kSectionSize));
}

// compute_effective_identity()/compute_effective_image_hash(): the "which
// exact executable" identity XenonSession publishes and native-extension
// compatibility gating relies on (see src/core/session.cpp) must actually
// differ once a title update changes the effective image, and match
// whichever image (base or patched) it was computed from.
void test_effective_identity_reflects_title_update() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));

  const auto base_only = xbox::compute_effective_identity(base_image);
  assert(!base_only.title_update_applied);
  assert(base_only.effective_image_hash == xbox::compute_effective_image_hash(base_image));

  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  for (std::size_t i = patch_pe.data_rva; i < patch_pe.data_rva + 0x1000u; ++i) {
    patch_pe.bytes[i] = std::byte{0xEE};
  }
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;  // TITLE | MODULE_PATCH | PATCH_FULL
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  assert(xbox::apply_title_update(base_image, patch_fixture.file, patched, &error));

  const auto patched_identity = xbox::compute_effective_identity(base_image, &patched);
  assert(patched_identity.title_update_applied);
  assert(patched_identity.effective_image_hash != base_only.effective_image_hash &&
        "a title update that changes .data must change the effective identity hash");
  assert(patched_identity.effective_image_hash == xbox::compute_effective_image_hash(patched));
  assert(!xbox::format_effective_image_hash(patched_identity.effective_image_hash).empty());
}

// XEX1's security_info has a genuinely different on-disk byte layout from
// XEX2 (different field order, no header_digest/export_table/
// import_table_count fields, RootImportAddress in their place - see
// kXex1SecurityInfoFixedSize in xex_loader.cpp) - this proves parse_xex_image()
// actually decodes that layout correctly end to end through the production
// loader, not merely that it shares code with the XEX2 path.
void test_parse_and_load_xex1_format() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/true);
  patch_import_placeholders(pe.bytes, pe.data_rva, /*ordinal0=*/0x0042u, /*ordinal1=*/0x0099u);
  XexBuildOptions opts{};
  opts.use_xex1_format = true;
  opts.title_id = 0x58455831u;  // Distinct from the XEX2 fixtures' title ID.
  opts.media_id = 0xAABBCCDDu;
  auto fixture = build_base_xex(pe, opts);

  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(image.format == xbox::XexFormat::Xex1);
  assert(image.title_id == 0x58455831u);
  assert(image.media_id == 0xAABBCCDDu);
  assert(image.image_base == kImageBase);
  assert(image.security.load_address == kImageBase);
  assert(!image.security.page_descriptors.empty());
  assert(!image.sections.empty());
  assert(!image.exports.empty());
  assert(image.exports.front().name == "XexTestExport");
  assert(image.imports.size() == 2u);

  const auto& text_section = image.sections.front();
  assert(text_section.executable);
  assert(!text_section.writable);

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());
  xbox::LoadedXex loaded{};
  const bool load_ok = xbox::load_xex(address_space, fixture.file, loaded, memory::kXex64KBase, &error);
  assert(load_ok && error.empty());
  assert(loaded.loaded);
  assert(!loaded.executable_ranges.empty());
  assert(address_space.read8(kImageBase + kSectionSize) == 0x4Eu);
}

// The XEX1 security_info's fixed portion (0x168 bytes) is smaller than
// XEX2's (0x184 bytes); a XEX1 file whose declared header_size only leaves
// room for the XEX2-sized region but not the full XEX1 region would, if the
// loader used the wrong format's size unconditionally, either falsely
// accept a truncated XEX1 security info or falsely reject a valid one - it
// must actually branch on format, not just accept "whichever fits".
void test_xex1_security_info_uses_its_own_smaller_layout() {
  auto fixture = make_uncompressed_fixture();  // XEX2 fixture (0x184-byte security info).
  xbox::XexImage xex2_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, xex2_image, &error) && error.empty());

  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions xex1_opts{};
  xex1_opts.use_xex1_format = true;
  xex1_opts.with_import_libraries = false;
  auto xex1_fixture = build_base_xex(pe, xex1_opts);
  xbox::XexImage xex1_image{};
  assert(xbox::parse_xex_image(xex1_fixture.file, xex1_image, &error) && error.empty());
  // A XEX1 security_info is genuinely smaller on disk than a XEX2 one for
  // an equivalent page-descriptor count - proves the loader isn't just
  // reusing the XEX2 byte offsets/size under a different tag.
  assert(xex1_image.security.header_size < xex2_image.security.header_size);
}

void test_page_descriptors_can_downgrade_writable_section() {
  // .data (RVA 0x3000) is characterized R+W in the PE header, but the
  // fixture's page-descriptor run marks every page past the first as
  // ReadOnlyData - so the *effective* protection for that section must be
  // read-only, not writable.
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error));
  bool found_data_section = false;
  for (const auto& section : image.sections) {
    if (section.name == ".data") {
      found_data_section = true;
      assert(!section.writable);
    }
  }
  assert(found_data_section);
}

void test_basic_compression_round_trip() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);

  // Split the plaintext body into two "data" runs separated by a zero-run
  // that XEX_COMPRESSION_BASIC reconstructs rather than stores.
  const auto& plain = fixture.plain_body;
  const std::uint32_t first_half = static_cast<std::uint32_t>(plain.size() / 2u);
  const std::uint32_t second_half = static_cast<std::uint32_t>(plain.size()) - first_half;

  // Rebuild the header with a basic-compression info block describing one
  // (data_size, zero_size) pair per half (zero_size=0, since the fixture
  // body has no real zero-run to elide - this still exercises the block-
  // table-in-header / body-reassembly path end to end).
  ByteWriter header;
  header.append_bytes(std::span<const std::byte>(fixture.file.data(), fixture.file_format_info_offset));
  const auto ffi_offset = header.size();
  header.append_be32(8u + 16u);  // info_size: header + 2 blocks * 8 bytes
  header.append_be16(0u);        // encryption = None
  header.append_be16(1u);        // compression = Basic
  header.append_be32(first_half);
  header.append_be32(0u);
  header.append_be32(second_half);
  header.append_be32(0u);
  const auto tail_start = fixture.file_format_info_offset + 8u;  // original FFI struct was 8 bytes.
  header.append_bytes(std::span<const std::byte>(fixture.file.data() + tail_start,
                                                 fixture.header_size - tail_start));
  auto header_bytes = header.take();
  const auto size_delta = header_bytes.size() - fixture.header_size;

  // Patch header_size (+0x08) and security_offset (+0x10) for the growth.
  const auto orig_header_size = fixture.header_size;
  const auto orig_security_offset =
      static_cast<std::uint32_t>(fixture.file[0x10]) << 24 | static_cast<std::uint32_t>(fixture.file[0x11]) << 16 |
      static_cast<std::uint32_t>(fixture.file[0x12]) << 8 | static_cast<std::uint32_t>(fixture.file[0x13]);
  (void)orig_header_size;
  const auto new_security_offset = orig_security_offset + static_cast<std::uint32_t>(size_delta);
  header_bytes[8] = fixture.file[8];
  header_bytes[9] = fixture.file[9];
  header_bytes[10] = fixture.file[10];
  header_bytes[11] = fixture.file[11];
  const auto new_header_size = static_cast<std::uint32_t>(header_bytes.size());
  header_bytes[8] = static_cast<std::byte>((new_header_size >> 24) & 0xFFu);
  header_bytes[9] = static_cast<std::byte>((new_header_size >> 16) & 0xFFu);
  header_bytes[10] = static_cast<std::byte>((new_header_size >> 8) & 0xFFu);
  header_bytes[11] = static_cast<std::byte>(new_header_size & 0xFFu);
  header_bytes[0x10] = static_cast<std::byte>((new_security_offset >> 24) & 0xFFu);
  header_bytes[0x11] = static_cast<std::byte>((new_security_offset >> 16) & 0xFFu);
  header_bytes[0x12] = static_cast<std::byte>((new_security_offset >> 8) & 0xFFu);
  header_bytes[0x13] = static_cast<std::byte>(new_security_offset & 0xFFu);

  // Also patch the file-format-info optional-header-table offset entry, and
  // every offset-based optional header entry after it, by `size_delta`.
  // Simplification: the fixture places FileFormatInfo last among
  // offset-based structures before security info, so only its own table
  // entry and the security_offset (already handled) need adjusting; find
  // and patch the FFI table entry by scanning for its key.
  const auto opt_count =
      static_cast<std::uint32_t>(fixture.file[0x14]) << 24 | static_cast<std::uint32_t>(fixture.file[0x15]) << 16 |
      static_cast<std::uint32_t>(fixture.file[0x16]) << 8 | static_cast<std::uint32_t>(fixture.file[0x17]);
  for (std::uint32_t i = 0; i < opt_count; ++i) {
    const auto entry_offset = 0x18u + i * 8u;
    const auto key = static_cast<std::uint32_t>(header_bytes[entry_offset]) << 24 |
                     static_cast<std::uint32_t>(header_bytes[entry_offset + 1]) << 16 |
                     static_cast<std::uint32_t>(header_bytes[entry_offset + 2]) << 8 |
                     static_cast<std::uint32_t>(header_bytes[entry_offset + 3]);
    if (key == kFileFormatInfoKey) {
      header_bytes[entry_offset + 4] = static_cast<std::byte>((ffi_offset >> 24) & 0xFFu);
      header_bytes[entry_offset + 5] = static_cast<std::byte>((ffi_offset >> 16) & 0xFFu);
      header_bytes[entry_offset + 6] = static_cast<std::byte>((ffi_offset >> 8) & 0xFFu);
      header_bytes[entry_offset + 7] = static_cast<std::byte>(ffi_offset & 0xFFu);
    }
  }

  std::vector<std::byte> file = header_bytes;
  file.insert(file.end(), plain.begin(), plain.end());

  xbox::XexImage image{};
  std::string error;
  const bool ok = xbox::parse_xex_image(file, image, &error);
  assert(ok && error.empty());
  assert(image.compression_type == xbox::XexCompressionType::Basic);
  assert(image.effective_image.size() == plain.size());
  assert(image.effective_image == plain);
}

void test_normal_compression_round_trip() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);

  // LZX-"compress" the body as a spec-legal uncompressed-block LZX stream
  // (real bitstream, real block framing - just not entropy-coded), then wrap
  // it in the XEX compressed-block-info chain (size+SHA1 header) the real
  // format requires.
  constexpr std::uint32_t kWindowBits = 17;
  const auto lzx_stream = xbox::lzx::encode_uncompressed(fixture.plain_body, kWindowBits);
  auto compressed_body = build_compressed_block_chain(lzx_stream);

  ByteWriter header;
  header.append_bytes(std::span<const std::byte>(fixture.file.data(), fixture.file_format_info_offset));
  const auto ffi_offset = header.size();
  header.append_be32(12u);  // info_size: 8-byte header + 4-byte window_size
  header.append_be16(0u);   // encryption = None
  header.append_be16(2u);   // compression = Normal
  header.append_be32(1u << kWindowBits);
  const auto tail_start = fixture.file_format_info_offset + 8u;
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

  const auto opt_count =
      static_cast<std::uint32_t>(fixture.file[0x14]) << 24 | static_cast<std::uint32_t>(fixture.file[0x15]) << 16 |
      static_cast<std::uint32_t>(fixture.file[0x16]) << 8 | static_cast<std::uint32_t>(fixture.file[0x17]);
  for (std::uint32_t i = 0; i < opt_count; ++i) {
    const auto entry_offset = 0x18u + i * 8u;
    const auto key = static_cast<std::uint32_t>(header_bytes[entry_offset]) << 24 |
                     static_cast<std::uint32_t>(header_bytes[entry_offset + 1]) << 16 |
                     static_cast<std::uint32_t>(header_bytes[entry_offset + 2]) << 8 |
                     static_cast<std::uint32_t>(header_bytes[entry_offset + 3]);
    if (key == kFileFormatInfoKey) {
      header_bytes[entry_offset + 4] = static_cast<std::byte>((ffi_offset >> 24) & 0xFFu);
      header_bytes[entry_offset + 5] = static_cast<std::byte>((ffi_offset >> 16) & 0xFFu);
      header_bytes[entry_offset + 6] = static_cast<std::byte>((ffi_offset >> 8) & 0xFFu);
      header_bytes[entry_offset + 7] = static_cast<std::byte>(ffi_offset & 0xFFu);
    }
  }

  std::vector<std::byte> file = header_bytes;
  file.insert(file.end(), compressed_body.begin(), compressed_body.end());

  xbox::XexImage image{};
  std::string error;
  const bool ok = xbox::parse_xex_image(file, image, &error);
  assert(ok && error.empty());
  assert(image.compression_type == xbox::XexCompressionType::Normal);
  assert(image.effective_image.size() == fixture.plain_body.size());
  assert(image.effective_image == fixture.plain_body);
}

// Rebuilds `fixture`'s header with a new FileFormatInfo body (the bytes
// immediately after the FileFormatInfo key/offset slot's 8-byte legacy
// struct - i.e. info_size/encryption/compression/[window_size]),
// adjusting header_size/security_offset for the resulting size delta.
// Shared by every test below that needs a non-default encryption/
// compression combination (mirrors the inline pattern the pre-existing
// compression tests each hand-rolled).
std::vector<std::byte> rebuild_header_with_file_format_info(const XexBuildResult& fixture,
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

// A full retail-shaped pipeline test: encrypted (AES-128-CBC) +
// Normal-compressed (real container framing over *genuine canonical-
// Huffman-coded* LZX blocks, alternating VERBATIM/ALIGNED block types, not
// the spec-legal-but-trivial "uncompressed block" encoding the other
// compression tests use) body, decoded through the public load_xex() entry
// point - header/security info/page descriptors/encryption/compression/PE
// parsing/imports/TLS/exports/section permissions/Memory V2 mapping all in
// one pass, exercising the same public API surface production code calls.
// The decoder is validated against data this test's Huffman *encoder*
// produced independently of decode()'s own internals (separate code path,
// no shared logic beyond the format constants both must agree on) - see
// encode_literal_huffman_block() in xex_lzx.cpp.
void test_retail_shaped_pipeline_encrypted_huffman_normal_compression() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/true);
  patch_import_placeholders(pe.bytes, pe.data_rva, /*ordinal0=*/0x0042u, /*ordinal1=*/0x0099u);
  XexBuildOptions opts{};
  auto fixture = build_base_xex(pe, opts);

  constexpr std::uint32_t kWindowBits = 17u;
  // Split the plaintext body into real, independently-Huffman-encoded LZX
  // blocks, each <= 0x8000 bytes so no single block straddles the LZX
  // chunk-realignment boundary decode() enforces every 32 KiB of output;
  // alternate VERBATIM/ALIGNED block types across chunks so both are
  // exercised in one fixture.
  std::vector<std::byte> lzx_stream;
  {
    constexpr std::size_t kChunk = 0x8000u;
    bool aligned = false;
    for (std::size_t offset = 0; offset < fixture.plain_body.size(); offset += kChunk) {
      const auto len = std::min(kChunk, fixture.plain_body.size() - offset);
      const auto block = xbox::lzx::encode_literal_huffman_block(
          std::span<const std::byte>(fixture.plain_body).subspan(offset, len), kWindowBits, aligned,
          /*first_block=*/offset == 0u);
      lzx_stream.insert(lzx_stream.end(), block.begin(), block.end());
      aligned = !aligned;
    }
  }
  auto compressed_body = build_compressed_block_chain(lzx_stream);

  // AES-128-CBC-encrypt the compressed body (zero IV, matching XEX's
  // encryption scheme) under a per-title image key that is itself wrapped
  // with the real, public retail key - exactly the on-disk representation
  // decompress_body()'s retail-key attempt must unwrap and decrypt.
  if (compressed_body.size() % 16u != 0u) {
    compressed_body.resize(compressed_body.size() + (16u - compressed_body.size() % 16u), std::byte{0});
  }
  xbox::crypto::AesKey image_key{};
  for (std::size_t i = 0; i < image_key.size(); ++i) image_key[i] = static_cast<std::byte>(i * 7u + 1u);
  xbox::crypto::AesBlock encrypted_image_key{};
  xbox::crypto::aes128_encrypt_block(xbox::crypto::retail_key(), image_key, encrypted_image_key);
  std::vector<std::byte> encrypted_body(compressed_body.size());
  xbox::crypto::aes128_cbc_encrypt(image_key, compressed_body, encrypted_body);

  ByteWriter ffi;
  ffi.append_be32(12u);  // info_size: 8-byte header + 4-byte window_size
  ffi.append_be16(1u);   // encryption = Normal
  ffi.append_be16(2u);   // compression = Normal
  ffi.append_be32(1u << kWindowBits);
  auto header_bytes = rebuild_header_with_file_format_info(fixture, ffi.data());

  // Patch security_info.encrypted_image_key (XEX2 offset 0x150 from
  // security_offset, which has already been shifted for the FFI size delta
  // by rebuild_header_with_file_format_info()).
  const auto security_offset = static_cast<std::uint32_t>(header_bytes[0x10]) << 24 |
                               static_cast<std::uint32_t>(header_bytes[0x11]) << 16 |
                               static_cast<std::uint32_t>(header_bytes[0x12]) << 8 |
                               static_cast<std::uint32_t>(header_bytes[0x13]);
  const auto key_field_offset = static_cast<std::size_t>(security_offset) + 0x150u;
  for (std::size_t i = 0; i < 16u; ++i) header_bytes[key_field_offset + i] = encrypted_image_key[i];

  std::vector<std::byte> file = header_bytes;
  file.insert(file.end(), encrypted_body.begin(), encrypted_body.end());

  xbox::XexImage image{};
  std::string error;
  const bool parsed = xbox::parse_xex_image(file, image, &error);
  assert(parsed && error.empty());
  assert(image.encryption_type == xbox::XexEncryptionType::Normal);
  assert(image.compression_type == xbox::XexCompressionType::Normal);
  assert(image.effective_image.size() == fixture.plain_body.size());
  assert(image.effective_image == fixture.plain_body);
  assert(!image.exports.empty());
  assert(image.exports.front().name == "XexTestExport");
  assert(image.imports.size() == 2u);
  assert(image.tls.has_value());

  memory::AddressSpace address_space(memory::GuestTranslationMode::Compact);
  assert(address_space.initialize());
  xbox::LoadedXex loaded{};
  const bool loaded_ok = xbox::load_xex(address_space, file, loaded, memory::kXex64KBase, &error);
  assert(loaded_ok && error.empty());
  assert(loaded.loaded);
  assert(!loaded.executable_ranges.empty());
  // .text's first bytes (the distinctive PPC-looking marker build_pe_body()
  // writes) must have survived decrypt+decompress+PE-parse+Memory V2 mapping
  // bit-for-bit.
  assert(address_space.read8(kImageBase + kSectionSize) == 0x4Eu);
  assert(address_space.read8(kImageBase + kSectionSize + 1u) == 0x80u);
  // The .text section is Code (R+X) per page descriptors, matching the
  // pre-existing page-descriptor-wins-over-PE-characteristics behaviour.
  bool found_text = false;
  for (const auto& section : loaded.mapped_sections) {
    if (section.name == ".text") {
      found_text = true;
      assert(section.executable);
      assert(!section.writable);
    }
  }
  assert(found_text);
}

// Proves a wrong decryption key cannot be silently accepted for
// XEX_COMPRESSION_NONE (which - unlike Normal/Delta's incidental per-block
// SHA1 checks - previously had *no* integrity validation of its own): wrap
// the image key with neither the retail nor the devkit key, so both of
// decompress_body()'s attempts decrypt to garbage of the structurally
// "plausible" (correctly-sized) but wrong effective image. Real
// cryptographic validation (security_info.section_digest, see
// verify_first_page_digest() in xex_loader.cpp) must reject this
// deterministically, not rely on garbage happening to fail PE parsing.
void test_wrong_key_rejected_even_for_none_compression() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);

  auto plain_body = fixture.plain_body;
  if (plain_body.size() % 16u != 0u) {
    plain_body.resize(plain_body.size() + (16u - plain_body.size() % 16u), std::byte{0});
  }

  // Neither the real retail key nor the all-zero devkit key.
  xbox::crypto::AesKey wrong_key{};
  for (std::size_t i = 0; i < wrong_key.size(); ++i) wrong_key[i] = static_cast<std::byte>(0xA5u ^ i);
  xbox::crypto::AesKey image_key{};
  for (std::size_t i = 0; i < image_key.size(); ++i) image_key[i] = static_cast<std::byte>(i + 1u);
  xbox::crypto::AesBlock encrypted_image_key{};
  xbox::crypto::aes128_encrypt_block(wrong_key, image_key, encrypted_image_key);
  std::vector<std::byte> encrypted_body(plain_body.size());
  xbox::crypto::aes128_cbc_encrypt(image_key, plain_body, encrypted_body);

  ByteWriter ffi;
  ffi.append_be32(8u);  // info_size
  ffi.append_be16(1u);  // encryption = Normal
  ffi.append_be16(0u);  // compression = None
  auto header_bytes = rebuild_header_with_file_format_info(fixture, ffi.data());

  const auto security_offset = static_cast<std::uint32_t>(header_bytes[0x10]) << 24 |
                               static_cast<std::uint32_t>(header_bytes[0x11]) << 16 |
                               static_cast<std::uint32_t>(header_bytes[0x12]) << 8 |
                               static_cast<std::uint32_t>(header_bytes[0x13]);
  const auto key_field_offset = static_cast<std::size_t>(security_offset) + 0x150u;
  for (std::size_t i = 0; i < 16u; ++i) header_bytes[key_field_offset + i] = encrypted_image_key[i];

  std::vector<std::byte> file = header_bytes;
  file.insert(file.end(), encrypted_body.begin(), encrypted_body.end());

  xbox::XexImage image{};
  std::string error;
  const bool ok = xbox::parse_xex_image(file, image, &error);
  assert(!ok);
  assert(!error.empty());
  assert(error.find("digest") != std::string::npos);
}

void test_normal_compression_rejects_corrupt_block_hash() {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe, opts);
  constexpr std::uint32_t kWindowBits = 17;
  const auto lzx_stream = xbox::lzx::encode_uncompressed(fixture.plain_body, kWindowBits);
  auto compressed_body = build_compressed_block_chain(lzx_stream, /*corrupt_hash=*/true);

  ByteWriter header;
  header.append_bytes(std::span<const std::byte>(fixture.file.data(), fixture.file_format_info_offset));
  header.append_be32(12u);
  header.append_be16(0u);
  header.append_be16(2u);
  header.append_be32(1u << kWindowBits);
  const auto tail_start = fixture.file_format_info_offset + 8u;
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

  std::vector<std::byte> file = header_bytes;
  file.insert(file.end(), compressed_body.begin(), compressed_body.end());

  xbox::XexImage image{};
  std::string error;
  const bool ok = xbox::parse_xex_image(file, image, &error);
  assert(!ok);
  assert(!error.empty());
  assert(error.find("SHA1") != std::string::npos || error.find("LZX") != std::string::npos);
}

void test_title_update_full_patch_replaces_image() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));

  // Build a "full patch" update: same title/media ID, a distinctly different
  // .data fill byte so the effective image is verifiably different.
  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  for (std::size_t i = patch_pe.data_rva; i < patch_pe.data_rva + 0x1000u; ++i) {
    patch_pe.bytes[i] = std::byte{0xEE};
  }
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;  // TITLE | MODULE_PATCH | PATCH_FULL
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_fixture.file, patched, &error);
  assert(ok && error.empty());
  assert(patched.effective_image.size() == patch_pe.bytes.size());
  assert(patched.effective_image[patch_pe.data_rva] == std::byte{0xEE});
  // The base image's own bytes must be completely untouched.
  assert(base_image.effective_image[patch_pe.data_rva] != std::byte{0xEE});
}

void test_title_update_rejects_mismatched_title() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));

  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;
  patch_opts.title_id = base_image.title_id + 1u;  // Deliberately wrong.
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_fixture.file, patched, &error);
  assert(!ok);
  assert(!error.empty());
}

void test_title_update_rejects_non_patch_payload() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));

  // A well-formed XEX that simply never set XEX_MODULE_MODULE_PATCH.
  auto other_fixture = make_uncompressed_fixture();
  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, other_fixture.file, patched, &error);
  assert(!ok);
  assert(!error.empty());
}

// Rewrites an existing XexBuildResult's opt-header-table entry for `key` by
// adding `size_delta` to its offset value - shared post-processing for
// tests that grow the header (e.g. via rebuild_header_with_file_format_info())
// while also using an optional header (like the delta-patch descriptor)
// whose own bytes live in the shifted "tail" region.
void shift_opt_header_table_entry(const XexBuildResult& fixture, std::vector<std::byte>& header_bytes,
                                 std::uint32_t key, std::int64_t size_delta) {
  const auto opt_count = static_cast<std::uint32_t>(fixture.file[0x14]) << 24 |
                         static_cast<std::uint32_t>(fixture.file[0x15]) << 16 |
                         static_cast<std::uint32_t>(fixture.file[0x16]) << 8 |
                         static_cast<std::uint32_t>(fixture.file[0x17]);
  for (std::uint32_t i = 0; i < opt_count; ++i) {
    const auto entry_offset = 0x18u + i * 8u;
    const auto entry_key = static_cast<std::uint32_t>(header_bytes[entry_offset]) << 24 |
                           static_cast<std::uint32_t>(header_bytes[entry_offset + 1]) << 16 |
                           static_cast<std::uint32_t>(header_bytes[entry_offset + 2]) << 8 |
                           static_cast<std::uint32_t>(header_bytes[entry_offset + 3]);
    if (entry_key != key) continue;
    const auto old_value = static_cast<std::uint32_t>(header_bytes[entry_offset + 4]) << 24 |
                           static_cast<std::uint32_t>(header_bytes[entry_offset + 5]) << 16 |
                           static_cast<std::uint32_t>(header_bytes[entry_offset + 6]) << 8 |
                           static_cast<std::uint32_t>(header_bytes[entry_offset + 7]);
    const auto new_value = static_cast<std::uint32_t>(static_cast<std::int64_t>(old_value) + size_delta);
    header_bytes[entry_offset + 4] = static_cast<std::byte>((new_value >> 24) & 0xFFu);
    header_bytes[entry_offset + 5] = static_cast<std::byte>((new_value >> 16) & 0xFFu);
    header_bytes[entry_offset + 6] = static_cast<std::byte>((new_value >> 8) & 0xFFu);
    header_bytes[entry_offset + 7] = static_cast<std::byte>(new_value & 0xFFu);
  }
}

// Proves the real XEXP image/data delta (XEX_COMPRESSION_DELTA): a
// format-accurate fixture with a descriptor-level whole-region splice
// (delta_image_source/target_offset/size) plus a xex2_delta_patch record
// chain containing all three record kinds (fill, copy, and a real
// LZXDELTA-compressed literal write), applied through the public
// apply_title_update() path and verified byte-for-byte against the base
// image with exactly those edits applied - not merely that *something*
// round-trips.
void test_title_update_delta_patch_reconstructs_with_real_record_framing() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));
  assert(base_image.effective_image.size() > 0x30000u + kPageSize4KB);

  // The patch's own on-disk PE body is never used as image content for
  // XEX_COMPRESSION_DELTA (the record chain reconstructs the image from the
  // *base*'s effective_image instead) - only its size (for image_size)
  // matters. It must use the same with_export setting as the base fixture
  // (make_uncompressed_fixture() uses true) so its first page is
  // byte-identical to the base's - none of this test's edits touch page 0,
  // so security_info.section_digest (computed from the patch's own PE body)
  // must describe that unchanged first page correctly.
  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/true);

  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000040u;  // TITLE | MODULE_PATCH | PATCH_DELTA
  patch_opts.with_delta_patch_descriptor = true;
  patch_opts.delta_base_signature_digest = xbox::crypto::sha1(base_image.security.rsa_signature);
  patch_opts.delta_source_version = base_image.execution_info.version.value;
  // Descriptor-level whole-region splice: copy .data (filled with 0xCD by
  // build_pe_body()) over [0x1000, 0x2000) - the "copy/reference region".
  // (0x1000..0x2000 - i.e. page index 1 - is padding zero bytes past the PE
  // headers/section table in the base fixture, safe to overwrite without
  // corrupting anything try_parse_pe_sections() reads.)
  patch_opts.delta_image_source_offset = 0x30000u;
  patch_opts.delta_image_source_size = 0x1000u;
  patch_opts.delta_image_target_offset = 0x1000u;
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  constexpr std::uint32_t kWindowBits = 17u;
  constexpr std::array<std::byte, 16> kMarker = {
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}, std::byte{0x01}, std::byte{0x02},
      std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
      std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}};
  const auto marker_payload = xbox::lzx::encode_uncompressed(kMarker, kWindowBits);

  // The record chain: (1) fill part of .data ([0x32000, 0x33000), still
  // page-index >0) with zero (was 0xCD), (2) a real LZX-compressed literal
  // write of kMarker at [0x2000, 0x2010) - the "LZXDELTA-compressed
  // region". (The "copy/reference region" is exercised via the
  // descriptor-level splice above, a second, independent copy mechanism
  // the real format also provides.)
  ByteWriter records_writer;
  records_writer.append_bytes(build_delta_patch_record(
      /*old_addr=*/0u, /*new_addr=*/0x32000u, /*uncompressed_len=*/static_cast<std::uint16_t>(kPageSize4KB),
      /*compressed_len=*/0u));
  records_writer.append_bytes(build_delta_patch_record(/*old_addr=*/0x2000u, /*new_addr=*/0x2000u,
                                                        /*uncompressed_len=*/static_cast<std::uint16_t>(kMarker.size()),
                                                        static_cast<std::uint16_t>(marker_payload.size()),
                                                        marker_payload));
  auto records = records_writer.take();
  auto compressed_body = build_delta_block_chain(records);

  ByteWriter ffi;
  ffi.append_be32(12u);  // info_size: 8-byte header + 4-byte window_size
  ffi.append_be16(0u);   // encryption = None
  ffi.append_be16(3u);   // compression = Delta
  ffi.append_be32(1u << kWindowBits);
  auto header_bytes = rebuild_header_with_file_format_info(patch_fixture, ffi.data());
  const auto size_delta =
      static_cast<std::int64_t>(header_bytes.size()) - static_cast<std::int64_t>(patch_fixture.header_size);
  shift_opt_header_table_entry(patch_fixture, header_bytes, kDeltaPatchDescriptorKey, size_delta);

  std::vector<std::byte> patch_file = header_bytes;
  patch_file.insert(patch_file.end(), compressed_body.begin(), compressed_body.end());

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_file, patched, &error);
  assert(ok && error.empty());

  auto expected = base_image.effective_image;
  std::copy(base_image.effective_image.begin() + 0x30000, base_image.effective_image.begin() + 0x30000 + 0x1000,
           expected.begin() + 0x1000);
  std::copy(kMarker.begin(), kMarker.end(), expected.begin() + 0x2000);
  std::fill(expected.begin() + 0x32000, expected.begin() + 0x33000, std::byte{0});

  assert(patched.effective_image.size() == expected.size());
  assert(patched.effective_image == expected);
  // Confirm the fill/copy/LZX edits actually changed something observable
  // relative to the untouched base image (i.e. this isn't accidentally
  // passing because nothing changed).
  assert(patched.effective_image[0x32000] != base_image.effective_image[0x32000]);
  assert(patched.effective_image[0x1000] != base_image.effective_image[0x1000]);
  assert(patched.effective_image[0x2000] != base_image.effective_image[0x2000]);
}

// The base-signature digest and source-version checks in
// apply_title_update() must reject a delta patch built against the wrong
// base image version *before* any record is ever applied - and the base
// image's own bytes must remain untouched either way, since
// apply_title_update() never mutates its input.
void test_title_update_delta_patch_rejects_wrong_base_version() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));
  const auto base_image_snapshot = base_image.effective_image;

  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000040u;  // TITLE | MODULE_PATCH | PATCH_DELTA
  patch_opts.with_delta_patch_descriptor = true;
  patch_opts.delta_base_signature_digest = xbox::crypto::sha1(base_image.security.rsa_signature);
  // Deliberately wrong: the patch was built for a different source version
  // than what the base image actually declares.
  patch_opts.delta_source_version = base_image.execution_info.version.value ^ 0xFFu;
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  // A minimal, empty record chain (immediately hits the all-zero
  // terminator) - validation must reject before this is ever reached.
  std::array<std::byte, 12> empty_records{};
  auto compressed_body = build_delta_block_chain(empty_records);

  ByteWriter ffi;
  ffi.append_be32(12u);
  ffi.append_be16(0u);
  ffi.append_be16(3u);  // compression = Delta
  ffi.append_be32(1u << 17u);
  auto header_bytes = rebuild_header_with_file_format_info(patch_fixture, ffi.data());
  const auto size_delta =
      static_cast<std::int64_t>(header_bytes.size()) - static_cast<std::int64_t>(patch_fixture.header_size);
  shift_opt_header_table_entry(patch_fixture, header_bytes, kDeltaPatchDescriptorKey, size_delta);

  std::vector<std::byte> patch_file = header_bytes;
  patch_file.insert(patch_file.end(), compressed_body.begin(), compressed_body.end());

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_file, patched, &error);
  assert(!ok);
  assert(!error.empty());
  assert(base_image.effective_image == base_image_snapshot);
}

// Malformed/truncated xex2_delta_patch record chains (an oversized
// compressed_len that overruns the block, and a corrupt block hash) must be
// rejected cleanly through the full title-update path, never crash or
// silently produce a corrupted image.
void test_title_update_delta_patch_rejects_malformed_records() {
  auto fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, base_image, &error));

  const auto build_patch_file = [&](std::span<const std::byte> records, bool corrupt_hash) {
    auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
    XexBuildOptions patch_opts{};
    patch_opts.with_import_libraries = false;
    patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000040u;
    patch_opts.with_delta_patch_descriptor = true;
    patch_opts.delta_base_signature_digest = xbox::crypto::sha1(base_image.security.rsa_signature);
    patch_opts.delta_source_version = base_image.execution_info.version.value;
    auto patch_fixture = build_base_xex(patch_pe, patch_opts);
    auto compressed_body = build_delta_block_chain(records, corrupt_hash);

    ByteWriter ffi;
    ffi.append_be32(12u);
    ffi.append_be16(0u);
    ffi.append_be16(3u);
    ffi.append_be32(1u << 17u);
    auto header_bytes = rebuild_header_with_file_format_info(patch_fixture, ffi.data());
    const auto size_delta =
        static_cast<std::int64_t>(header_bytes.size()) - static_cast<std::int64_t>(patch_fixture.header_size);
    shift_opt_header_table_entry(patch_fixture, header_bytes, kDeltaPatchDescriptorKey, size_delta);

    std::vector<std::byte> patch_file = header_bytes;
    patch_file.insert(patch_file.end(), compressed_body.begin(), compressed_body.end());
    return patch_file;
  };

  // A record claiming a compressed_len far larger than the block actually
  // holds.
  {
    auto record = build_delta_patch_record(0u, 0u, 0x100u, 0xFFFFu);
    auto patch_file = build_patch_file(record, /*corrupt_hash=*/false);
    xbox::XexImage patched{};
    const bool ok = xbox::apply_title_update(base_image, patch_file, patched, &error);
    assert(!ok);
    assert(!error.empty());
  }

  // A well-formed record chain whose outer block hash has been corrupted.
  {
    auto record = build_delta_patch_record(0u, 0u, 0x100u, 0u);
    auto patch_file = build_patch_file(record, /*corrupt_hash=*/true);
    xbox::XexImage patched{};
    const bool ok = xbox::apply_title_update(base_image, patch_file, patched, &error);
    assert(!ok);
    assert(!error.empty());
    assert(error.find("SHA1") != std::string::npos);
  }
}

// Proves the real XEXP header-region delta (XEX_HEADER_DELTA_PATCH_DESCRIPTOR
// delta_headers_source_offset/size/target_offset + the embedded
// xex2_delta_patch record) is actually applied: the patch file's *own*
// on-disk header declares the wrong media_id, and only produces the correct
// effective header via (1) delta_headers_* splicing the base image's header
// bytes into the target buffer, then (2) the embedded record LZX-patching
// just the media_id field to a third, distinct value neither the base nor
// the patch's on-disk header ever declared on their own.
//
// Note: the patch's own on-disk execution_info (title_id/media_id) must
// already agree with the base image - real XEXP title updates always keep
// those identical between base and patch (they describe the same disc/
// media), and apply_title_update() validates them from the patch's own
// on-disk header *before* header-delta reconstruction runs, exactly like
// real hardware's is_patch()/title-ID checks. What this test proves is that
// the value downstream consumers actually see after reconstruction is the
// *record-patched* one, not simply whatever the shared base/patch on-disk
// value happened to be - i.e. that the splice+LZX-record pipeline actually
// ran, not that it was skipped and everything happened to already match.
void test_title_update_header_delta_reconstructs_media_id() {
  constexpr std::uint32_t kSharedOnDiskMediaId = 0x11112222u;    // Declared by both base and patch on-disk.
  constexpr std::uint32_t kReconstructedMediaId = 0x33334444u;   // Only reachable via the delta record.

  auto base_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions base_opts{};
  base_opts.with_import_libraries = false;
  base_opts.media_id = kSharedOnDiskMediaId;
  auto base_fixture = build_base_xex(base_pe, base_opts);

  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(base_fixture.file, base_image, &error) && error.empty());
  assert(base_image.media_id == kSharedOnDiskMediaId);
  assert(!base_image.header_bytes.empty());

  // The patch shares the base's exact header shape (same opts) so
  // delta_headers_target_offset lines up with delta_headers_source_offset -
  // real updates typically splice the whole unchanged-structure header
  // verbatim from the base and patch only the handful of fields that
  // actually differ, exactly like this.
  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;  // TITLE | MODULE_PATCH | PATCH_FULL
  patch_opts.media_id = kSharedOnDiskMediaId;
  patch_opts.with_delta_patch_descriptor = true;
  patch_opts.delta_base_signature_digest = xbox::crypto::sha1(base_image.security.rsa_signature);
  patch_opts.delta_source_version = base_image.execution_info.version.value;
  patch_opts.delta_headers_source_offset = 0u;
  patch_opts.delta_headers_source_size = static_cast<std::uint32_t>(base_fixture.header_size);
  patch_opts.delta_headers_target_offset = 0u;
  patch_opts.delta_size_of_target_headers = static_cast<std::uint32_t>(base_fixture.header_size);

  // The single embedded xex2_delta_patch record: LZX-uncompressed-block
  // literal payload overwriting just the 4-byte media_id field within
  // execution_info, in place (old_addr == new_addr).
  std::array<std::byte, 4> new_media_id_bytes{
      static_cast<std::byte>((kReconstructedMediaId >> 24) & 0xFFu),
      static_cast<std::byte>((kReconstructedMediaId >> 16) & 0xFFu),
      static_cast<std::byte>((kReconstructedMediaId >> 8) & 0xFFu),
      static_cast<std::byte>(kReconstructedMediaId & 0xFFu)};
  const auto record_payload = xbox::lzx::encode_uncompressed(new_media_id_bytes, /*window_bits=*/17u);
  const auto record_addr = static_cast<std::uint32_t>(base_fixture.execution_info_offset);
  ByteWriter record_writer;
  record_writer.append_be32(record_addr);  // old_addr
  record_writer.append_be32(record_addr);  // new_addr
  record_writer.append_be16(4u);           // uncompressed_len
  record_writer.append_be16(static_cast<std::uint16_t>(record_payload.size()));  // compressed_len
  record_writer.append_bytes(record_payload);
  patch_opts.header_patch_record_bytes = record_writer.take();

  // Since delta_headers_source_size covers the *entire* base header at
  // target_offset 0, the reconstructed header is wholesale base-structured -
  // the patch's own on-disk structural layout (which may differ, e.g. it
  // omits the import-libraries header) is irrelevant; only
  // base_fixture.execution_info_offset (used for the record's old/new_addr
  // above) matters as the coordinate space for the reconstructed buffer.
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_fixture.file, patched, &error);
  assert(ok && error.empty());
  assert(patched.media_id == kReconstructedMediaId);
  assert(patched.media_id != kSharedOnDiskMediaId);
  // Everything else the splice carried over from the base header should
  // still be intact (title ID lives right next to media_id and was not
  // touched by the record).
  assert(patched.title_id == base_image.title_id);
}

// A header-delta descriptor whose delta_headers_source_size overruns the
// base image's own header must be rejected cleanly, never read out of
// bounds.
void test_title_update_header_delta_rejects_out_of_range_source() {
  auto base_fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(base_fixture.file, base_image, &error));

  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;
  patch_opts.with_delta_patch_descriptor = true;
  patch_opts.delta_headers_source_offset = 0u;
  // Deliberately larger than the base header can possibly supply.
  patch_opts.delta_headers_source_size = static_cast<std::uint32_t>(base_image.header_bytes.size()) + 0x10000u;
  patch_opts.delta_headers_target_offset = 0u;
  patch_opts.delta_size_of_target_headers = patch_opts.delta_headers_source_size;
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_fixture.file, patched, &error);
  assert(!ok);
  assert(!error.empty());
}

// delta_headers_target_offset near UINT32_MAX, combined with a small
// size_of_target_headers of 0 (forcing the target_size fallback
// computation target_offset + source_size), must not silently wrap a
// 32-bit sum into an accepted small value - it must be rejected as
// out-of-range, not misinterpreted as a tiny valid target header.
void test_title_update_header_delta_rejects_overflowing_target_offset() {
  auto base_fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(base_fixture.file, base_image, &error));

  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;
  patch_opts.with_delta_patch_descriptor = true;
  patch_opts.delta_headers_source_offset = 0u;
  patch_opts.delta_headers_source_size = 0x10u;
  // 0xFFFFFFF0 + 0x10 wraps to exactly 0 in 32-bit arithmetic - if computed
  // naively in 32 bits this would (incorrectly) look like "target_size == 0",
  // itself rejected only by luck rather than by an explicit range check.
  patch_opts.delta_headers_target_offset = 0xFFFFFFF0u;
  patch_opts.delta_size_of_target_headers = 0u;  // Forces the fallback sum.
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_fixture.file, patched, &error);
  assert(!ok);
  assert(!error.empty());
}

// A header-delta descriptor whose embedded LZX record's compressed_len
// doesn't correspond to any real LZX bitstream (here: no payload bytes were
// ever written for it, so the decoder reads unrelated header content as if
// it were the compressed stream) must be rejected cleanly by the LZX
// decoder, never crash or silently produce a corrupt header.
void test_title_update_header_delta_rejects_malformed_record() {
  auto base_fixture = make_uncompressed_fixture();
  xbox::XexImage base_image{};
  std::string error;
  assert(xbox::parse_xex_image(base_fixture.file, base_image, &error));

  auto patch_pe = build_pe_body(kImageBase, /*with_export=*/false);
  XexBuildOptions patch_opts{};
  patch_opts.with_import_libraries = false;
  patch_opts.module_flags = 0x00000001u | 0x00000010u | 0x00000020u;
  patch_opts.with_delta_patch_descriptor = true;
  patch_opts.delta_headers_source_offset = 0u;
  patch_opts.delta_headers_source_size = static_cast<std::uint32_t>(base_fixture.header_size);
  patch_opts.delta_headers_target_offset = 0u;
  patch_opts.delta_size_of_target_headers = static_cast<std::uint32_t>(base_fixture.header_size);
  // Claims a 0x1000-byte LZX payload but no payload bytes are appended.
  ByteWriter record_writer;
  record_writer.append_be32(0u);
  record_writer.append_be32(0u);
  record_writer.append_be16(0x100u);
  record_writer.append_be16(0x1000u);
  patch_opts.header_patch_record_bytes = record_writer.take();
  auto patch_fixture = build_base_xex(patch_pe, patch_opts);

  xbox::XexImage patched{};
  const bool ok = xbox::apply_title_update(base_image, patch_fixture.file, patched, &error);
  assert(!ok);
  assert(!error.empty());
}

// A header-delta descriptor whose embedded record's compressed_len bytes
// are not physically present anywhere in the patch file (the file ends
// before the declared payload could possibly fit) must be rejected via the
// explicit bounds check, not an out-of-bounds read.
void test_title_update_header_delta_rejects_out_of_bounds_record_payload() {
  // A minimal, hand-built XEX2 header: base header (0x18) + one inline
  // entry-point optional header (0x8) + a XEX_HEADER_DELTA_PATCH_DESCRIPTOR
  // pointing past the end of the buffer for its record payload + a minimal
  // (invalid, but never reached) security info. This deliberately bypasses
  // build_base_xex()'s large fixture machinery to keep the file small enough
  // that "declares more payload than the file has" is unambiguous.
  ByteWriter header;
  header.append_bytes(std::array<std::byte, 4>{std::byte{'X'}, std::byte{'E'}, std::byte{'X'}, std::byte{'2'}});
  header.append_be32(0x00000011u);                    // module_flags: TITLE | MODULE_PATCH
  const auto header_size_field = header.append_be32(0u);
  header.append_be32(0u);                              // reserved
  const auto security_offset_field = header.append_be32(0u);
  header.append_be32(1u);                              // optional_header_count = 1 (delta descriptor)

  const auto opt_table_offset = header.size();
  header.append_be32(0u);  // key (patched below)
  header.append_be32(0u);  // offset (patched below)

  const auto delta_offset = header.size();
  header.append_be32(0x4Cu);  // size
  header.append_be32(0u);     // target_version
  header.append_be32(0u);     // source_version
  header.append_zeros(20u);   // digest_source
  header.append_zeros(16u);   // image_key_source
  header.append_be32(0x100u);  // size_of_target_headers
  header.append_be32(0u);      // delta_headers_source_offset
  header.append_be32(0x10u);   // delta_headers_source_size
  header.append_be32(0u);      // delta_headers_target_offset
  header.append_zeros(4u);     // delta_image_source_offset
  header.append_zeros(4u);     // delta_image_source_size
  header.append_zeros(4u);     // delta_image_target_offset
  header.append_be32(0u);      // record old_addr
  header.append_be32(0u);      // record new_addr
  header.append_be16(0x10u);   // record uncompressed_len
  header.append_be16(0xFFFFu);  // record compressed_len: far larger than the file could ever hold.
  // No payload bytes follow - the file simply ends here.

  header.put_be32(opt_table_offset, kDeltaPatchDescriptorKey);
  header.put_be32(opt_table_offset + 4u, static_cast<std::uint32_t>(delta_offset));

  const auto final_header_size = header.size();
  header.put_be32(header_size_field, static_cast<std::uint32_t>(final_header_size));
  header.put_be32(security_offset_field, static_cast<std::uint32_t>(final_header_size));
  (void)security_offset_field;

  auto patch_file = header.take();

  // A dummy base image: only header_bytes/effective_image/title_id are
  // consulted before header-delta reconstruction runs, so this need not be
  // a fully valid loadable image.
  xbox::XexImage base_image{};
  base_image.header_bytes.assign(0x100u, std::byte{0});
  base_image.effective_image.assign(0x10u, std::byte{0});
  base_image.title_id = 0u;

  xbox::XexImage patched{};
  std::string error;
  const bool ok = xbox::apply_title_update(base_image, patch_file, patched, &error);
  assert(!ok);
  assert(!error.empty());
}

void test_truncated_file_rejected_cleanly() {
  auto fixture = make_uncompressed_fixture();
  // Truncate right in the middle of the body: must fail, never crash/UB.
  fixture.file.resize(fixture.header_size + 10);
  xbox::XexImage image{};
  std::string error;
  const bool ok = xbox::parse_xex_image(fixture.file, image, &error);
  assert(!ok);
  assert(!error.empty());
}

}  // namespace

int main() {
  test_malformed_xex();
  test_invalid_header_size();
  test_missing_security_info_rejected();
  test_pe_section_virtual_size_uses_field_after_full_eight_byte_name();
  test_loaded_image_rva_semantics_ignore_raw_pointer();
  test_relocation_entries_are_inline_in_directory();
  test_parse_and_load_uncompressed_unencrypted();
  test_native_function_import_pair_is_classified_without_rewriting_address_slot();
  test_map_xex_image_rounds_section_size_up_to_memory_page();
  test_map_xex_image_merges_sections_that_share_allocation_pages();
  test_map_xex_image_rolls_back_partial_mapping_failure();
  test_map_xex_image_matches_load_xex();
  test_effective_identity_reflects_title_update();
  test_parse_and_load_xex1_format();
  test_xex1_security_info_uses_its_own_smaller_layout();
  test_page_descriptors_can_downgrade_writable_section();
  test_basic_compression_round_trip();
  test_normal_compression_round_trip();
  test_retail_shaped_pipeline_encrypted_huffman_normal_compression();
  test_wrong_key_rejected_even_for_none_compression();
  test_normal_compression_rejects_corrupt_block_hash();
  // NOTE: prior to this pass, none of the title-update (XEXP) tests below
  // were actually registered here - they existed as dead code that the test
  // binary never ran. Wiring them in is itself part of this fix.
  test_title_update_full_patch_replaces_image();
  test_title_update_rejects_mismatched_title();
  test_title_update_rejects_non_patch_payload();
  test_title_update_delta_patch_reconstructs_with_real_record_framing();
  test_title_update_delta_patch_rejects_wrong_base_version();
  test_title_update_delta_patch_rejects_malformed_records();
  test_title_update_header_delta_reconstructs_media_id();
  test_title_update_header_delta_rejects_out_of_range_source();
  test_title_update_header_delta_rejects_overflowing_target_offset();
  test_title_update_header_delta_rejects_malformed_record();
  test_title_update_header_delta_rejects_out_of_bounds_record_payload();
  test_truncated_file_rejected_cleanly();
  return 0;
}
