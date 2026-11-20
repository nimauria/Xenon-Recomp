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

#include "../support/test_xex.hpp"

namespace {

using namespace xenon;
using namespace xenon::test::xex;

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

std::uint32_t read_file_be32(const std::vector<std::byte>& file, std::size_t offset) {
  return std::to_integer<std::uint32_t>(file[offset]) << 24 | std::to_integer<std::uint32_t>(file[offset + 1]) << 16 |
         std::to_integer<std::uint32_t>(file[offset + 2]) << 8 | std::to_integer<std::uint32_t>(file[offset + 3]);
}

void write_file_be32(std::vector<std::byte>& file, std::size_t offset, std::uint32_t value) {
  for (int b = 0; b < 4; ++b) file[offset + b] = static_cast<std::byte>(value >> (24 - 8 * b));
}

// XEN-008 (found by tests/security/input_fuzz_tests.cpp): image_size is
// header data that every decompression path sizes its output from. An image
// larger than the console's memory is rejected before anything is allocated
// (a 264 KiB file used to allocate 2 GiB), and Basic compression's zero runs
// cannot expand past it either.
void test_image_size_beyond_console_memory_rejected() {
  for (const std::uint16_t compression : {std::uint16_t{0}, std::uint16_t{2}}) {
    auto fixture = make_uncompressed_fixture();
    auto file = fixture.file;
    if (compression == 2u) {
      ByteWriter ffi;
      ffi.append_be32(12u);
      ffi.append_be16(0u);
      ffi.append_be16(2u);
      ffi.append_be32(1u << 17);
      auto header = rebuild_header_with_file_format_info(fixture, ffi.data());
      const auto chain = build_compressed_block_chain(xbox::lzx::encode_uncompressed(fixture.plain_body, 17u));
      file = header;
      file.insert(file.end(), chain.begin(), chain.end());
    }
    const auto security = read_file_be32(file, 0x10);
    write_file_be32(file, security + 4u, 0x82000000u);
    xbox::XexImage image{};
    std::string error;
    assert(!xbox::parse_xex_image(file, image, &error));
    assert(error.find("memory") != std::string::npos);
  }
  // The import string table's declared name count is bounded by the table's
  // size (a count of 2^32-1 reserved 137 GB of names before any was read).
  {
    auto fixture = make_uncompressed_fixture();
    xbox::XexImage reference{};
    assert(xbox::parse_xex_image(fixture.file, reference));
    auto file = fixture.file;
    const auto count = read_file_be32(file, 0x14);
    bool patched = false;
    for (std::uint32_t i = 0; i < count; ++i) {
      if (read_file_be32(file, 0x18u + i * 8u) == kImportLibrariesKey) {
        write_file_be32(file, read_file_be32(file, 0x18u + i * 8u + 4u) + 8u, 0xFFFFFFFFu);
        patched = true;
      }
    }
    assert(patched);
    xbox::XexImage image{};
    std::string error;
    assert(xbox::parse_xex_image(file, image, &error));
    assert(image.imports.size() == reference.imports.size());
  }
  {
    auto fixture = make_uncompressed_fixture();
    ByteWriter ffi;
    ffi.append_be32(8u + 3u * 8u);
    ffi.append_be16(0u);
    ffi.append_be16(1u);  // Basic
    for (int i = 0; i < 3; ++i) {
      ffi.append_be32(0u);
      ffi.append_be32(0xFFFFFFF0u);  // zero run
    }
    auto file = rebuild_header_with_file_format_info(fixture, ffi.data());
    file.insert(file.end(), fixture.plain_body.begin(), fixture.plain_body.end());
    xbox::XexImage image{};
    std::string error;
    assert(!xbox::parse_xex_image(file, image, &error));
    assert(!error.empty());
  }
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

// The exception directory (.pdata) of an Xbox 360 image: 8-byte big-endian
// entries, not the 12-byte little-endian {begin, end, unwind} records of
// x64 images. Reading AC6's 65,304-byte directory the x64 way produced 2,739
// "functions" such as 0x82000982..0x86120040 that seeded analysis; read the
// Xbox way it holds 8,163 functions, 42 with exception handlers.
std::uint32_t pdata_word(std::uint32_t prolog, std::uint32_t instructions, bool handler) {
  return (prolog & 0xFFu) | ((instructions & 0x3FFFFFu) << 8u) | (1u << 30u) |
         (handler ? 1u << 31u : 0u);
}

PeBuildResult pe_with_exception_directory(std::uint16_t machine,
                                          const std::vector<std::uint32_t>& words) {
  auto pe = build_pe_body(kImageBase, /*with_export=*/false);
  const auto put_le = [&](std::size_t offset, std::uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
      pe.bytes[offset + i] = static_cast<std::byte>((value >> (8u * i)) & 0xFFu);
  };
  const auto put_be32 = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i)
      pe.bytes[offset + i] = static_cast<std::byte>((value >> (24u - 8u * i)) & 0xFFu);
  };
  constexpr std::size_t kMachineOffset = 0x84u;
  constexpr std::size_t kDataDirectoryOffset = 0xF8u;
  constexpr std::uint32_t kPdataRva = 0x00020800u;  // inside .rdata
  put_le(kMachineOffset, machine, 2u);
  put_le(kDataDirectoryOffset + 3u * 8u, kPdataRva, 4u);
  put_le(kDataDirectoryOffset + 3u * 8u + 4u, static_cast<std::uint32_t>(words.size() * 4u), 4u);
  for (std::size_t i = 0; i < words.size(); ++i) put_be32(kPdataRva + i * 4u, words[i]);
  // The handled function at .text+0x100 has its handler address and scope
  // table in the 8 bytes in front of it: words that decode as lwz.
  put_be32(0x000100F8u, kImageBase + 0x00010040u);
  put_be32(0x000100FCu, kImageBase + 0x00020000u);
  return pe;
}

xbox::XexImage parse_with_exception_directory(std::uint16_t machine,
                                              const std::vector<std::uint32_t>& words) {
  XexBuildOptions opts{};
  opts.with_import_libraries = false;
  auto fixture = build_base_xex(pe_with_exception_directory(machine, words), opts);
  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  return image;
}

void test_exception_directory_uses_the_xbox_360_layout() {
  const std::uint32_t text = kImageBase + 0x00010000u;
  const std::vector<std::uint32_t> words = {
      // Out of order on purpose: the loader orders entries by address.
      text + 0x100u, pdata_word(3u, 10u, true),   // handled function, 0x100..0x128
      text + 0x000u, pdata_word(2u, 4u, false),   // 0x000..0x010
      0u, 0u,                                     // padding
      text + 0x202u, pdata_word(1u, 4u, false),   // unaligned: not a function
      kImageBase - 0x100u, pdata_word(1u, 4u, false),  // before the image
      kImageBase + 0x00090000u, pdata_word(1u, 4u, false),  // past the image
      text + 0x300u, pdata_word(1u, 0u, false),   // empty
      text + 0x008u, pdata_word(1u, 4u, false),   // overlaps the first: corrupt
      text + 0x12Cu, pdata_word(1u, 2u, true),    // handler words overlap 0x100..0x128
  };
  const auto image = parse_with_exception_directory(0x01F2u, words);
  assert(image.function_metadata.size() == 2u);
  const auto& first = image.function_metadata[0];
  assert(first.valid && first.begin == text && first.end == text + 0x10u);
  assert(first.prolog_instructions == 2u && !first.exception_handler);
  assert(first.code_start() == first.begin);
  const auto& handled = image.function_metadata[1];
  assert(handled.begin == text + 0x100u && handled.end == text + 0x128u);
  assert(handled.prolog_instructions == 3u && handled.exception_handler);
  assert(handled.unwind_data == pdata_word(3u, 10u, true));
  assert(handled.handler_address == text + 0x40u);
  assert(handled.handler_data == kImageBase + 0x00020000u);
  assert(handled.code_start() == text + 0xF8u);

  assert(xbox::function_metadata_at(image, text) == &first);
  assert(xbox::function_metadata_at(image, text + 4u) == nullptr);
  assert(xbox::function_metadata_containing(image, text + 0xCu) == &first);
  assert(xbox::function_metadata_containing(image, text + 0x10u) == nullptr);
  assert(xbox::function_metadata_containing(image, text + 0x124u) == &handled);
  assert(xbox::next_function_metadata_after(image, text) == &handled);
  assert(xbox::next_function_metadata_after(image, text + 0xF7u) == &handled);
  assert(xbox::next_function_metadata_after(image, text + 0xF8u) == nullptr);
  assert(xbox::next_function_metadata_after(image, kImageBase) == &first);
  assert(!xbox::is_exception_handler_data(image, text + 0xF4u));
  assert(xbox::is_exception_handler_data(image, text + 0xF8u));
  assert(xbox::is_exception_handler_data(image, text + 0xFCu));
  assert(!xbox::is_exception_handler_data(image, text + 0x100u));
  assert(!xbox::is_exception_handler_data(image, text + 0x8u));
}

void test_exception_directory_of_another_machine_is_not_guessed() {
  // An x64-layout directory (12-byte records) under a non-PowerPC machine
  // yields nothing rather than misread entries.
  const auto image = parse_with_exception_directory(
      0x8664u, {0x00010000u, 0x00010010u, 0x00000000u});
  assert(image.function_metadata.empty());
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

// Regression (AC6): the image's XEX_HEADER_DEFAULT_STACK_SIZE (256 KiB in
// AC6) sizes the main thread and every ExCreateThread(stack_size = 0). Xenon
// used a fixed 1 MiB, which spent the headroom AC6 leaves after sizing its
// heap from MmQueryStatistics and crashed it on a failed allocation.
void test_default_stack_size_header_sizes_default_thread_stacks() {
  auto pe = build_pe_body(kImageBase, true);
  patch_import_placeholders(pe.bytes, pe.data_rva, 0x0042u, 0x0099u);
  XexBuildOptions opts{};
  opts.default_stack_size = 0x40000u;
  const auto fixture = build_base_xex(pe, opts);
  xbox::XexImage image{};
  std::string error;
  assert(xbox::parse_xex_image(fixture.file, image, &error) && error.empty());
  assert(image.default_stack_size == 0x40000u);
  assert(xbox::default_thread_stack_size(image) == 0x40000u);
  // The other optional headers are unaffected by the extra table entry.
  assert(image.entry_point == kImageBase + kSectionSize);
  assert(image.tls.has_value() && image.tls->slot == 1u);

  // A size that is not a page multiple is rounded up to a whole page.
  image.default_stack_size = 0x10001u;
  assert(xbox::default_thread_stack_size(image) == 0x11000u);

  // Without the header the kernel fallback applies.
  const auto plain = make_uncompressed_fixture();
  xbox::XexImage plain_image{};
  assert(xbox::parse_xex_image(plain.file, plain_image, &error));
  assert(plain_image.default_stack_size == 0u);
  assert(xbox::default_thread_stack_size(plain_image) == xbox::kFallbackThreadStackSize);
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
// compatibility gating relies on (see src/core/session/lifecycle/native_extension.cpp) must actually
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
  test_exception_directory_uses_the_xbox_360_layout();
  test_exception_directory_of_another_machine_is_not_guessed();
  test_malformed_xex();
  test_invalid_header_size();
  test_missing_security_info_rejected();
  test_image_size_beyond_console_memory_rejected();
  test_pe_section_virtual_size_uses_field_after_full_eight_byte_name();
  test_loaded_image_rva_semantics_ignore_raw_pointer();
  test_relocation_entries_are_inline_in_directory();
  test_parse_and_load_uncompressed_unencrypted();
  test_default_stack_size_header_sizes_default_thread_stacks();
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
