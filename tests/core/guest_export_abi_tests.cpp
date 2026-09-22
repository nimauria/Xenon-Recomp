// Proves the real "recompiled PPC guest code -> import call -> CPU V2
// generated code -> external-call bridge -> core::ExportRegistry -> active
// XenonSession subsystem" path end to end, through the actual production
// pipeline (XEX Loader V2 -> Recomp Driver -> CPU V2 codegen -> a compiled,
// loaded native module -> KernelProcess -> KernelThread), not a direct C++
// call into ExportRegistry.
//
// Mechanism this exercises (see src/core/session.cpp's
// XenonSession::call()): a guest `bl <address>` whose target is not a
// discovered/compiled function already falls back to
// RuntimeServices::call(), which CPU V2's generated code always calls for
// exactly this case (Op::Call in backend_cpp_aot.cpp). XenonSession::call()
// now checks that target against loaded_xex_->image.imports[i].guest_thunk
// (the guest address XEX Loader V2 already resolved from the native
// XEX_HEADER_IMPORT_LIBRARIES import table - see
// xex_loader.cpp's parse_native_import_libraries()) and, on a match, calls
// external_call(module, ordinal, state, memory) - reaching the exact same
// core::ExportRegistry every other subsystem's exports already use. No
// codegen or analysis change was needed: an import's callable guest_thunk
// address is a type-1 native import record (record type in the top byte,
// ordinal in the low 16 bits), never
// real PPC instructions a function could be discovered/compiled at, so a
// call to one always falls into this path for every title.
//
// The three imports proven here (covering all three system library classes -
// xboxkrnl, xam, and Audio V1's xboxkrnl-registered export surface):
//   - "xboxkrnl" library, XAudioGetUnderrunCount ordinal (0x35A): audio
//     exports are registered under the "xboxkrnl" library string (see
//     src/audio/exports.cpp) - deterministic (always 0 with no render
//     client registered), zero arguments, verifies the return-value path.
//   - "xam" library, XamInputGetCapabilities ordinal (0x190): verifies
//     argument marshalling (r3/r4/r5), a deterministic non-success XResult
//     with an explicit "null" input driver, and - by pre-filling the guest
//     output buffer with a sentinel and confirming it is untouched - the
//     "no write on failure" invariant, proving guest memory writes really
//     do (or do not) happen through this path rather than just "some
//     callback ran".
//   - "xboxkrnl" library, NtCreateFile ordinal (0x00D2): the structured ABI
//     call. Exercises a real guest OBJECT_ATTRIBUTES (root_directory/name/
//     attributes) pointing at a real ANSI_STRING (length/max_length/buffer)
//     over real path bytes, a real IO_STATUS_BLOCK output, a real output
//     Handle write, and - because NtCreateFile takes 9 parameters - a real
//     PPC ABI stack-passed argument (create_options, written to and read
//     back from r1+0x54) in addition to r3..r10. Routed through the exact
//     same production GuestIoBridge/IoFacade/KernelIoManager every other
//     xboxkrnl file-I/O caller uses (see xboxkrnl_io_exports.cpp's
//     NtCreateFile_thunk); no filesystem/content is mounted, so the call
//     deterministically does not succeed, which is the point - it proves a
//     real NTSTATUS came back from real production code, not a hard-coded
//     success.
//
// Also proven: a genuine LOCAL guest-to-guest `bl` (entry calls
// local_add_one, a second, independently discovered/compiled function in
// the same module - not an import) actually executes its callee. This
// closure pass found that Op::Call's codegen previously never tried the
// local CPU V2 compiled registry at all - only CallIndirect/linked
// BranchIndirect did - so a direct call to another local function always
// fell to runtime.call(), found nothing (there is no CPU V1 code_cache_
// registration for a native-extension-loaded title), wasn't a recognized
// import either, and used to return a non-terminal Branch that Op::Call's
// own `if(rr.terminal())` check silently swallowed: the callee simply never
// ran. See backend_cpp_aot.cpp's Op::Call fix.

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "xenon/core/session.hpp"
#include "xenon/recomp/driver.hpp"

// This test's session config requests enable_audio=true and expects the real
// Audio V1 XAudioGetUnderrunCount export to actually run - see the "Critical
// lesson" this session's closure pass fixed for real once already (Vulkan/
// D3D12 silently compiled out of xenon_core because a CMakeLists.txt block
// went missing). Fail loudly at compile time rather than silently building an
// audio-less test that would then fail its own assert() with a message that
// doesn't explain why: XENON_HAS_AUDIO is a PUBLIC compile definition on
// xenon_core (see CMakeLists.txt), so linking it PRIVATE still transitively
// defines this for the test's own translation unit.
#if !defined(XENON_HAS_AUDIO)
#error "guest_export_abi_tests requires xenon_core built with XENON_HAS_AUDIO (real Audio V1)"
#endif

namespace {

void be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 8);
  bytes[offset + 1] = static_cast<std::byte>(value);
}

void be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 24);
  bytes[offset + 1] = static_cast<std::byte>(value >> 16);
  bytes[offset + 2] = static_cast<std::byte>(value >> 8);
  bytes[offset + 3] = static_cast<std::byte>(value);
}

void le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
  bytes[offset + 2] = static_cast<std::byte>(value >> 16);
  bytes[offset + 3] = static_cast<std::byte>(value >> 24);
}

void write_str(std::vector<std::byte>& bytes, std::size_t offset, const char* text) {
  std::size_t i = 0;
  for (; text[i] != '\0'; ++i) bytes[offset + i] = static_cast<std::byte>(text[i]);
  bytes[offset + i] = std::byte{0};
}

constexpr std::uint32_t kLoadAddress = 0x80000000u;
// XenonSession/XEX Loader V2 map guest addresses in [kXex64KBase, kXex64KEnd)
// (which kLoadAddress falls in) using 64 KiB large pages (see
// xex_loader.cpp's xex_page_size_for()/memory::kLargePageSize), so two
// sections closer together than 0x10000 would round down to the same page
// and collide on the second reserve_fixed() call - real retail XEX section
// RVAs are always spaced at least this far apart for the same reason.
constexpr std::uint32_t kTextRva = 0x10000u;
constexpr std::uint32_t kDataRva = 0x20000u;
constexpr std::uint32_t kOutCapsRva = kDataRva + 0x00u;             // 20 bytes
constexpr std::uint32_t kResultUnderrunRva = kDataRva + 0x14u;      // 4 bytes
constexpr std::uint32_t kResultXamRva = kDataRva + 0x18u;           // 4 bytes
constexpr std::uint32_t kThunkAudioRva = kDataRva + 0x1Cu;          // 4 bytes (xboxkrnl:XAudioGetUnderrunCount)
constexpr std::uint32_t kThunkXamRva = kDataRva + 0x20u;            // 4 bytes (xam:XamInputGetCapabilities)
constexpr std::uint32_t kThunkCreateFileRva = kDataRva + 0x24u;     // 4 bytes (xboxkrnl:NtCreateFile)
constexpr std::uint32_t kResultLocalRva = kDataRva + 0x28u;         // 4 bytes (local guest-to-guest call proof)
constexpr std::uint32_t kHandleOutRva = kDataRva + 0x2Cu;           // 4 bytes
constexpr std::uint32_t kObjectAttributesRva = kDataRva + 0x30u;    // 12 bytes: root_directory,name,attributes
constexpr std::uint32_t kAnsiStringRva = kDataRva + 0x3Cu;          // 8 bytes: length,max_length,buffer
constexpr std::uint32_t kPathBytesRva = kDataRva + 0x44u;           // 16 bytes: "nonexistent.bin\0"
constexpr std::uint32_t kIoStatusBlockRva = kDataRva + 0x54u;       // 8 bytes: status,information
constexpr std::uint32_t kResultCreateFileRva = kDataRva + 0x5Cu;    // 4 bytes

constexpr std::uint32_t kAudioUnderrunOrdinal = 0x35Au;
constexpr std::uint32_t kXamInputCapabilitiesOrdinal = 0x190u;
constexpr std::uint32_t kNtCreateFileOrdinal = 0x00D2u;

// Builds a synthetic, structurally-valid XEX2 title image whose entry point
// calls three real guest imports via genuine PPC `bl` instructions (matching
// exactly the byte formats XEX Loader V2 and the recomp driver already
// consume in production, not a test-only shortcut format), and one genuine
// LOCAL guest-to-guest call to another compiled function in the same module -
// proving Op::Call's codegen actually runs a locally compiled callee instead
// of silently falling through (see backend_cpp_aot.cpp's Op::Call fix: before
// it, a direct `bl` to a local function that wasn't a recognized import
// silently no-op'd, because nothing in this pipeline ever registers anything
// into the CPU-v1 ExecutableCodeCache XenonSession::call() used to consult).
std::vector<std::byte> make_import_calling_xex() {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t import_table = 0x1A4;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  constexpr std::size_t data_raw = kDataRva;  // effective-image-relative == RVA by choice
  constexpr std::size_t file_size = header + data_raw + 0x70;

  std::vector<std::byte> bytes(file_size, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));
  be32(bytes, 0x14, 1);  // optional_header_count

  // Optional header entry table (file offset 0x18, 8 bytes/entry): one
  // entry pointing at the native import-libraries table below.
  be32(bytes, 0x18, 0x000103FFu);  // kHeaderImportLibraries
  be32(bytes, 0x1C, static_cast<std::uint32_t>(import_table));

  // XexSecurityInfo (0x184 bytes): only header_size/image_size/load_address
  // matter for this compression=None/encryption=None fixture.
  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  // Native XEX_HEADER_IMPORT_LIBRARIES table (see
  // xex_loader.cpp:parse_native_import_libraries): table_size/
  // string_table_size/string_table_count header, then a NUL-terminated,
  // 4-byte-aligned library-name string table, then one variable-length
  // library record per module.
  const std::size_t string_table_offset = import_table + 0xC;
  write_str(bytes, string_table_offset, "xboxkrnl");       // 8 chars + NUL = 9, padded to 12
  write_str(bytes, string_table_offset + 12, "xam");        // 3 chars + NUL = 4
  constexpr std::uint32_t string_table_size = 12 + 4;

  // "xboxkrnl", name_index 0: two imports (audio underrun count, NtCreateFile)
  // in the SAME library record - exercises the native import table's
  // variable-length import-address-array format for import_count > 1, not
  // just the import_count == 1 case the previous pass proved.
  const std::size_t lib0 = string_table_offset + string_table_size;
  be32(bytes, lib0 + 0x00, 0x30);          // library_size = 0x28 + 4*2
  be16(bytes, lib0 + 0x24, 0);             // name_index
  be16(bytes, lib0 + 0x26, 2);             // import_count
  be32(bytes, lib0 + 0x28, kLoadAddress + kThunkAudioRva);       // import[0]
  be32(bytes, lib0 + 0x2C, kLoadAddress + kThunkCreateFileRva);  // import[1]

  const std::size_t lib1 = lib0 + 0x30;  // "xam", name_index 1
  be32(bytes, lib1 + 0x00, 0x2C);
  be16(bytes, lib1 + 0x24, 1);
  be16(bytes, lib1 + 0x26, 1);
  be32(bytes, lib1 + 0x28, kLoadAddress + kThunkXamRva);

  const std::uint32_t table_size =
      static_cast<std::uint32_t>(0xC + string_table_size + 0x30 + 0x2C);
  be32(bytes, import_table + 0x0, table_size);
  be32(bytes, import_table + 0x4, string_table_size);
  be32(bytes, import_table + 0x8, 2);

  // PE headers.
  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 2);           // NumberOfSections
  le16(bytes, coff + 0x10, 0xE0);     // SizeOfOptionalHeader
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);       // AddressOfEntryPoint
  le32(bytes, optional + 0x1C, kLoadAddress);   // ImageBase
  le32(bytes, optional + 0x38, kDataRva + 0x10000u);  // SizeOfImage

  // Section 0: .text, executable, containing the entry point and the local
  // helper function it calls (37 words total = 0x94 bytes; 0xA0 leaves a
  // little slack).
  write_str(bytes, section, ".text");
  le32(bytes, section + 4, 0xA0);           // VirtualSize
  le32(bytes, section + 0xC, kTextRva);     // VirtualAddress
  le32(bytes, section + 0x10, 0xA0);        // SizeOfRawData
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));  // PointerToRawData
  le32(bytes, section + 0x24, 0x60000020);  // CODE | MEM_EXECUTE | MEM_READ

  // Section 1: .data, read/write, NOT executable - covers the output
  // buffer/result slots and both import placeholder words. raw_pointer is
  // deliberately equal to its VirtualAddress (RVA-based layout) so the
  // bytes XEX Loader V2 maps into guest memory (via raw_pointer) are the
  // exact same bytes parse_native_import_libraries() reads (via RVA
  // indexing directly into the effective image) - both must agree on what
  // sits at each import's guest_thunk address.
  const std::size_t data_section = section + 0x28;
  write_str(bytes, data_section, ".data");
  le32(bytes, data_section + 4, 0x60);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, 0x60);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040);  // INITIALIZED_DATA | MEM_READ | MEM_WRITE

  // .data initial contents (file offset = header + data_raw + section-relative).
  const std::size_t data_file_base = header + data_raw;
  for (std::uint32_t i = 0; i < 20; ++i) {
    bytes[data_file_base + i] = std::byte{0xCC};  // out_caps sentinel
  }
  be32(bytes, data_file_base + 0x14, 0xDEADBEEFu);  // result_underrun sentinel
  be32(bytes, data_file_base + 0x18, 0xDEADBEEFu);  // result_xam sentinel
  be32(bytes, data_file_base + 0x1C, 0x01000000u | kAudioUnderrunOrdinal);
  be32(bytes, data_file_base + 0x20, 0x01000000u | kXamInputCapabilitiesOrdinal);
  be32(bytes, data_file_base + 0x24, 0x01000000u | kNtCreateFileOrdinal);
  be32(bytes, data_file_base + 0x28, 0xDEADBEEFu);  // result_local sentinel
  be32(bytes, data_file_base + 0x2C, 0xCCCCCCCCu);  // handle_out sentinel

  // ObjectAttributes (kObjectAttributesRva, +0x30): root_directory=0
  // (kInvalidHandle - takes GuestIoBridge's plain-path lookup branch, not
  // open_at()), name -> the ANSI_STRING below, attributes=0.
  be32(bytes, data_file_base + 0x30, 0);                       // root_directory
  be32(bytes, data_file_base + 0x34, kLoadAddress + kAnsiStringRva);  // name
  be32(bytes, data_file_base + 0x38, 0);                       // attributes

  // ANSI_STRING-shaped descriptor (kAnsiStringRva, +0x3C) - see
  // GuestIoBridge::read_ansi_string(): length/max_length are 16-bit BE, the
  // buffer field is a 32-bit BE guest pointer.
  constexpr char kPath[] = "nonexistent.bin";
  constexpr std::uint16_t kPathLength = sizeof(kPath) - 1u;  // exclude NUL
  be16(bytes, data_file_base + 0x3C, kPathLength);
  be16(bytes, data_file_base + 0x3E, kPathLength + 1u);
  be32(bytes, data_file_base + 0x40, kLoadAddress + kPathBytesRva);
  write_str(bytes, data_file_base + 0x44, kPath);  // kPathBytesRva, +0x44

  // io_status_block (kIoStatusBlockRva, +0x54): pre-fill with a sentinel so a
  // real write is distinguishable from "never touched".
  be32(bytes, data_file_base + 0x54, 0xCCCCCCCCu);
  be32(bytes, data_file_base + 0x58, 0xCCCCCCCCu);
  be32(bytes, data_file_base + 0x5C, 0xDEADBEEFu);  // result_create_file sentinel

  // .text: real PPC instructions (verified by hand against the P-series ISA
  // encoding - D-form addi/addis/ori/stw, I-form bl):
  //   0:  li   r3, 41
  //   1:  bl   local_add_one        (genuine LOCAL guest-to-guest call - not
  //                                  an import; proves Op::Call actually
  //                                  runs a locally compiled callee)
  //   2:  lis  r6, hi16(result_local)
  //   3:  ori  r6, r6, lo16(result_local)
  //   4:  stw  r3, 0(r6)
  //   5:  bl   xboxkrnl_thunk        (XAudioGetUnderrunCount, no args)
  //   6:  lis  r6, hi16(result_underrun)
  //   7:  ori  r6, r6, lo16(result_underrun)
  //   8:  stw  r3, 0(r6)
  //   9:  li   r3, 0                 ; user_index
  //  10:  li   r4, 0                 ; flags
  //  11:  lis  r5, hi16(out_caps)
  //  12:  ori  r5, r5, lo16(out_caps)
  //  13:  bl   xam_thunk             (XamInputGetCapabilities)
  //  14:  lis  r6, hi16(result_xam)
  //  15:  ori  r6, r6, lo16(result_xam)
  //  16:  stw  r3, 0(r6)
  //  17:  li   r11, 0                ; create_options (9th arg, on the stack)
  //  18:  stw  r11, 0x54(r1)
  //  19:  lis  r3, hi16(handle_out)
  //  20:  ori  r3, r3, lo16(handle_out)
  //  21:  li   r4, 0                 ; desired_access
  //  22:  lis  r5, hi16(object_attributes)
  //  23:  ori  r5, r5, lo16(object_attributes)
  //  24:  lis  r6, hi16(io_status_block)
  //  25:  ori  r6, r6, lo16(io_status_block)
  //  26:  li   r7, 0                 ; allocation_size ptr = NULL
  //  27:  li   r8, 0                 ; file_attributes
  //  28:  li   r9, 0                 ; share_access
  //  29:  li   r10, 1                ; creation_disposition = FILE_OPEN
  //  30:  bl   nt_create_file_thunk  (xboxkrnl:NtCreateFile)
  //  31:  lis  r6, hi16(result_create_file)
  //  32:  ori  r6, r6, lo16(result_create_file)
  //  33:  stw  r3, 0(r6)
  //  34:  blr
  //  -- local_add_one (a genuinely separate compiled function) --
  //  35:  addi r3, r3, 1
  //  36:  blr
  const std::uint32_t entry = kLoadAddress + kTextRva;
  const std::uint32_t audio_thunk = kLoadAddress + kThunkAudioRva;
  const std::uint32_t xam_thunk = kLoadAddress + kThunkXamRva;
  const std::uint32_t create_file_thunk = kLoadAddress + kThunkCreateFileRva;
  const std::uint32_t out_caps = kLoadAddress + kOutCapsRva;
  const std::uint32_t result_underrun = kLoadAddress + kResultUnderrunRva;
  const std::uint32_t result_xam = kLoadAddress + kResultXamRva;
  const std::uint32_t result_local = kLoadAddress + kResultLocalRva;
  const std::uint32_t handle_out = kLoadAddress + kHandleOutRva;
  const std::uint32_t object_attributes = kLoadAddress + kObjectAttributesRva;
  const std::uint32_t io_status_block = kLoadAddress + kIoStatusBlockRva;
  const std::uint32_t result_create_file = kLoadAddress + kResultCreateFileRva;
  const std::uint32_t local_add_one = entry + 35u * 4u;

  const auto hi16 = [](std::uint32_t v) { return static_cast<std::uint16_t>(v >> 16); };
  const auto lo16 = [](std::uint32_t v) { return static_cast<std::uint16_t>(v & 0xFFFFu); };
  const auto d_form = [](std::uint32_t opcode, std::uint32_t rd_or_rs, std::uint32_t ra,
                         std::uint16_t imm) {
    return (opcode << 26) | (rd_or_rs << 21) | (ra << 16) | imm;
  };
  const auto bl_rel = [](std::uint32_t from, std::uint32_t target) {
    const std::uint32_t delta = target - from;
    assert((delta & 0x3u) == 0 && "branch target must be word-aligned");
    return 0x48000001u | (delta & 0x03FFFFFCu);
  };

  std::array<std::uint32_t, 37> words{};
  words[0] = d_form(14, 3, 0, 41);                              // li r3,41
  words[1] = bl_rel(entry + 1 * 4u, local_add_one);
  words[2] = d_form(15, 6, 0, hi16(result_local));              // lis r6,hi16
  words[3] = d_form(24, 6, 6, lo16(result_local));              // ori r6,r6,lo16
  words[4] = d_form(36, 3, 6, 0);                               // stw r3,0(r6)
  words[5] = bl_rel(entry + 5 * 4u, audio_thunk);
  words[6] = d_form(15, 6, 0, hi16(result_underrun));           // lis r6,hi16
  words[7] = d_form(24, 6, 6, lo16(result_underrun));           // ori r6,r6,lo16
  words[8] = d_form(36, 3, 6, 0);                               // stw r3,0(r6)
  words[9] = d_form(14, 3, 0, 0);                               // li r3,0
  words[10] = d_form(14, 4, 0, 0);                              // li r4,0
  words[11] = d_form(15, 5, 0, hi16(out_caps));                 // lis r5,hi16
  words[12] = d_form(24, 5, 5, lo16(out_caps));                 // ori r5,r5,lo16
  words[13] = bl_rel(entry + 13 * 4u, xam_thunk);
  words[14] = d_form(15, 6, 0, hi16(result_xam));
  words[15] = d_form(24, 6, 6, lo16(result_xam));
  words[16] = d_form(36, 3, 6, 0);
  words[17] = d_form(14, 11, 0, 0);                             // li r11,0 (create_options)
  words[18] = d_form(36, 11, 1, 0x54);                          // stw r11,0x54(r1)
  words[19] = d_form(15, 3, 0, hi16(handle_out));               // lis r3,hi16
  words[20] = d_form(24, 3, 3, lo16(handle_out));               // ori r3,r3,lo16
  words[21] = d_form(14, 4, 0, 0);                              // li r4,0 (desired_access)
  words[22] = d_form(15, 5, 0, hi16(object_attributes));        // lis r5,hi16
  words[23] = d_form(24, 5, 5, lo16(object_attributes));        // ori r5,r5,lo16
  words[24] = d_form(15, 6, 0, hi16(io_status_block));          // lis r6,hi16
  words[25] = d_form(24, 6, 6, lo16(io_status_block));          // ori r6,r6,lo16
  words[26] = d_form(14, 7, 0, 0);                              // li r7,0 (allocation_size)
  words[27] = d_form(14, 8, 0, 0);                              // li r8,0 (file_attributes)
  words[28] = d_form(14, 9, 0, 0);                              // li r9,0 (share_access)
  words[29] = d_form(14, 10, 0, 1);                             // li r10,1 (creation_disposition=FILE_OPEN)
  words[30] = bl_rel(entry + 30 * 4u, create_file_thunk);
  words[31] = d_form(15, 6, 0, hi16(result_create_file));       // lis r6,hi16
  words[32] = d_form(24, 6, 6, lo16(result_create_file));       // ori r6,r6,lo16
  words[33] = d_form(36, 3, 6, 0);                              // stw r3,0(r6)
  words[34] = 0x4E800020u;                                      // blr
  words[35] = d_form(14, 3, 3, 1);                              // addi r3,r3,1  (local_add_one)
  words[36] = 0x4E800020u;                                      // blr

  const std::size_t text_file_base = header + text_raw;
  for (std::size_t i = 0; i < words.size(); ++i) {
    be32(bytes, text_file_base + i * 4u, words[i]);
  }

  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main() {
  std::cout << "Testing guest export ABI round trip...\n";

  const auto root = std::filesystem::temp_directory_path() / "xenon_guest_export_abi_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto input = root / "fixture.xex";
  const auto output = root / "generated";
  const auto xex_bytes = make_import_calling_xex();
  write_file(input, xex_bytes);

  // Step 1-3: XEX Loader V2 (via the driver) + Recomp Driver analysis +
  // codegen. Assert the import call sites lowered to the intended
  // external-call path (item 12: a codegen assertion, guarding against a
  // future optimizer treating a thunk as an ordinary guest branch) by
  // checking the entry function's IR references both import ordinals as
  // *unresolved calls*, not as discovered local functions.
  xenon::recomp::DriverOptions options;
  options.input = input;
  options.output = output;
  xenon::recomp::AnalysisReport report;
  std::string error;
  assert(xenon::recomp::load_and_analyze(options, report, error) && error.empty());
  assert(report.image.imports.size() == 3 && "all three imports should be parsed from the native import table");

  bool found_audio_import = false, found_xam_import = false, found_create_file_import = false;
  for (const auto& import : report.image.imports) {
    if (import.module == "xboxkrnl" && import.ordinal == kAudioUnderrunOrdinal) found_audio_import = true;
    if (import.module == "xam" && import.ordinal == kXamInputCapabilitiesOrdinal) found_xam_import = true;
    if (import.module == "xboxkrnl" && import.ordinal == kNtCreateFileOrdinal) found_create_file_import = true;
  }
  assert(found_audio_import && "xboxkrnl audio import should be parsed with the right ordinal");
  assert(found_xam_import && "xam import should be parsed with the right ordinal");
  assert(found_create_file_import &&
         "xboxkrnl NtCreateFile import should be parsed with the right ordinal (import_count > 1 case)");

  std::cout << xenon::recomp::format_report(report) << std::endl;
  bool entry_compiled = false, local_add_one_compiled = false;
  for (const auto& function : report.functions) {
    if (function.guest_start == kLoadAddress + kTextRva) {
      entry_compiled = true;
      assert(function.compiled && "entry function must compile despite branching to import thunks");
    }
    if (function.guest_start == kLoadAddress + kTextRva + 35u * 4u) {
      local_add_one_compiled = true;
      assert(function.compiled && "the locally-called helper function must compile");
    }
  }
  assert(entry_compiled && "entry function should have been discovered and compiled");
  assert(local_add_one_compiled &&
         "a genuine local guest-to-guest bl target should be discovered/compiled independently, "
         "distinct from an import thunk");

  // No import's guest_thunk should ever have become a real discovered
  // function - the codegen-assertion this test cares about: if a future
  // change made the analyzer try to treat a thunk address as a callable
  // guest function instead of falling through to the external-call bridge,
  // this would start failing (either the thunk shows up compiled, or the
  // whole analysis errors out trying to decode the placeholder word).
  for (const auto& function : report.functions) {
    assert(function.guest_start != kLoadAddress + kThunkAudioRva &&
           "an import thunk must never be treated as a discovered/compiled function");
    assert(function.guest_start != kLoadAddress + kThunkXamRva &&
           "an import thunk must never be treated as a discovered/compiled function");
    assert(function.guest_start != kLoadAddress + kThunkCreateFileRva &&
           "an import thunk must never be treated as a discovered/compiled function");
  }

  assert(xenon::recomp::generate_project(options, report, error) && error.empty());
  assert(std::filesystem::exists(output / "registry.cpp"));
  assert(std::filesystem::exists(output / "module_export.cpp"));
  assert(std::filesystem::exists(output / "CMakeLists.txt"));

#if defined(XENON_SOURCE_ROOT)
  // Step 4 (build pipeline): compile the generated project into the real,
  // canonical Xenon_BindCompiledRegistry-exporting SHARED module - the same
  // artifact form Project Gracemeria's build would produce, not a
  // test-only static-link shortcut.
  const auto build_dir = root / "build";
  const auto quote = [](const std::filesystem::path& value) {
    return std::string("\"") + value.string() + "\"";
  };
  const auto configure = "cmake -S " + quote(output) + " -B " + quote(build_dir) +
                         " -DXENON_RECOMP_ROOT=" + quote(XENON_SOURCE_ROOT);
  assert(std::system(configure.c_str()) == 0);
  const auto compile_cmd =
      "cmake --build " + quote(build_dir) + " --target xenon_game_module --config Debug";
  assert(std::system(compile_cmd.c_str()) == 0);

  // Also build the STATIC xenon_game target from the very same generated
  // shard files (see driver.cpp's generate_project(): xenon_game_module
  // wraps xenon_game rather than compiling the guest code twice). Building
  // both targets, not just the shared module, is the real regression test
  // for the driver.cpp namespace-mismatch bug this pass's predecessor found
  // and fixed - that bug was invisible for as long as ONLY a static archive
  // was ever produced, since archiving never needs to resolve the mismatched
  // symbol references.
  const auto compile_static_cmd =
      "cmake --build " + quote(build_dir) + " --target xenon_game --config Debug";
  assert(std::system(compile_static_cmd.c_str()) == 0);

  // Locate the built shared library (name/location varies by generator).
  std::filesystem::path module_path;
  for (const auto* candidate :
       {"xenon_game_module.dll", "Debug/xenon_game_module.dll", "libxenon_game_module.so"}) {
    auto path = build_dir / candidate;
    if (std::filesystem::exists(path)) {
      module_path = path;
      break;
    }
  }
  assert(!module_path.empty() && "built xenon_game_module shared library not found");

  // Step 5-8: real XenonSession -> KernelProcess -> KernelThread execution,
  // loading the just-built module exactly as a real title would.
  xenon::core::XenonSession session;
  xenon::core::SessionConfig config{};
  config.enable_logging = true;
  config.enable_graphics = false;
  config.enable_input = true;
  // Explicit "null" driver: makes XamInputGetCapabilities' result
  // deterministic (no real controller hardware needed/assumed) while still
  // being the REAL InputSystem/GuestInputBridge production code path.
  config.input_drivers = {"null"};
  // Must be true: audio exports (including XAudioGetUnderrunCount) are only
  // registered into export_registry_ when enable_audio is set - see
  // XenonSession::init_exports(). This still needs (and gets, via SDL2) a
  // real host audio backend; init_audio() fails session init outright if
  // one cannot be created, matching normal Play semantics.
  config.enable_audio = true;
  config.native_extension_path = module_path.string();

  auto init_result = session.initialize(config);
  assert(init_result.success && "session initialization should succeed");

  auto load_result = session.load_game(xex_bytes, "guest_export_abi_test");
  if (!load_result.success) std::cout << "load_game failed: " << load_result.message << "\n";
  assert(load_result.success);
  assert(session.kernel_process() != nullptr && "KernelProcess must exist after load_game()");
  // Preflight proof (resolve_xex_imports(), run inside load_game()) that
  // both imports are genuinely registered in the live export_registry_
  // *before* execution - not just present in the XEX's own import table.
  // This is what would catch enable_audio=false silently leaving
  // XAudioGetUnderrunCount unregistered (a real mistake caught while
  // writing this test): both the r3 result AND this list would need to
  // agree for the round trip to be a real, non-coincidental proof.
  for (const auto& unresolved : session.unresolved_imports()) {
    std::cout << "unexpectedly unresolved: " << unresolved.library << " ordinal " << unresolved.ordinal
              << std::endl;
  }
  assert(session.unresolved_imports().empty());
  if (!session.native_extension_bound()) {
    std::cout << "native extension failed to bind: " << session.native_extension_error() << "\n";
  }
  assert(session.native_extension_bound());

  auto start_result = session.start();
  if (!start_result.success) std::cout << "start() failed: " << start_result.message << "\n";
  assert(start_result.success);
  assert(session.main_thread() != nullptr && "start() must create a real KernelThread");

  // Step 14: deterministic termination - wait for the real KernelThread to
  // finish, then verify the process/session are still coherent.
  for (int i = 0; i < 200 && session.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(!session.execution_active() && "guest execution should have completed");
  if (!session.last_error().empty()) std::cout << "unexpected error: " << session.last_error() << "\n";
  assert(session.last_error().empty());

  // Step 3/15-16: verify the actual guest-visible ABI results, read back
  // through Memory V2 exactly as the guest program wrote them.
  auto* memory = session.memory();
  assert(memory != nullptr);

  const auto underrun_result = memory->read32_be(kLoadAddress + kResultUnderrunRva);
  assert(underrun_result != 0xDEADBEEFu && "XAudioGetUnderrunCount result was never written by the guest call");
  assert(underrun_result == 0u &&
         "XAudioGetUnderrunCount should deterministically report zero underruns with no render client");

  const auto xam_result = memory->read32_be(kLoadAddress + kResultXamRva);
  assert(xam_result != 0xDEADBEEFu && "XamInputGetCapabilities result was never written by the guest call");
  assert(xam_result != 0u &&
         "XamInputGetCapabilities must report a real (non-success) XResult with an explicit null input driver");

  // "No write on failure" invariant: GuestInputBridge::get_capabilities()
  // only writes the output struct on XResult::Success (see
  // src/input/xam_guest.cpp). Since the call above returned non-zero, the
  // 0xCC sentinel this fixture pre-filled the output buffer with must be
  // completely untouched - proving the guest memory write path is real and
  // correctly gated, not merely "some callback ran".
  std::array<std::byte, 20> out_caps_bytes{};
  memory->read_bytes(kLoadAddress + kOutCapsRva, out_caps_bytes);
  for (std::size_t i = 0; i < out_caps_bytes.size(); ++i) {
    assert(out_caps_bytes[i] == std::byte{0xCC} &&
           "output buffer must stay untouched when the export call did not succeed");
  }

  // Local guest-to-guest call proof (the Op::Call codegen fix): 41 -> 42
  // only if local_add_one genuinely ran with r3 passed through correctly.
  const auto local_result = memory->read32_be(kLoadAddress + kResultLocalRva);
  assert(local_result != 0xDEADBEEFu && "local_add_one's result was never written back");
  assert(local_result == 42u &&
         "a direct bl to another locally compiled function must actually execute it "
         "(41 + 1 == 42), not silently no-op");

  // Structured xboxkrnl ABI proof (NtCreateFile): real OBJECT_ATTRIBUTES +
  // ANSI_STRING + IO_STATUS_BLOCK marshalling, a guest stack-passed 9th
  // argument, a real Handle write, and a real NTSTATUS - all through the
  // production GuestIoBridge (no test-only simplified file API). No
  // filesystem/content is mounted, so this deterministically does not
  // succeed; what matters is that every field was genuinely marshalled.
  const auto handle_out_value = memory->read32_be(kLoadAddress + kHandleOutRva);
  assert(handle_out_value != 0xCCCCCCCCu &&
         "NtCreateFile must write the output handle even on failure (kInvalidHandle)");
  const auto iosb_status = memory->read32_be(kLoadAddress + kIoStatusBlockRva);
  assert(iosb_status != 0xCCCCCCCCu && "NtCreateFile must write a real IO_STATUS_BLOCK.Status");
  const auto create_file_result = memory->read32_be(kLoadAddress + kResultCreateFileRva);
  assert(create_file_result != 0xDEADBEEFu && "NtCreateFile's NTSTATUS result was never written back");
  assert(create_file_result == iosb_status &&
         "NtCreateFile's r3 return value must match IO_STATUS_BLOCK.Status");
  assert(create_file_result != 0u &&
         "NtCreateFile must deterministically fail (no filesystem/content mounted in this test), "
         "not report success it never performed");

  session.shutdown();
  assert(!session.is_initialized() && "session should be cleanly shut down");
#else
  std::cout << "  (skipping full build/execution stage: XENON_SOURCE_ROOT not defined for this "
               "target - analysis/codegen-assertion coverage above still ran)\n";
#endif

  std::filesystem::remove_all(root);
  std::cout << "All tests passed!\n";
  return 0;
}
