// Negative-path coverage for the guest import/export dispatch boundary this
// session's closure pass was asked to prove: unknown library, known
// library/missing ordinal, a malformed native import table entry, a call
// target that is neither compiled code nor a valid import thunk, and a
// missing/incompatible native game module - plus an import-identity
// (ordinal-collision) regression for core::ExportRegistry, and a
// KernelProcess/KernelMemory/guest-stack rollback proof for a failure inside
// XenonSession::create_guest_process().
//
// Per the task prompt's own wording ("Using recompiled guest code OR the
// production import-resolution boundary, test: ..."), these exercise the
// production import-resolution boundary directly - XenonSession::call(),
// XenonSession::load_game()'s resolve_xex_imports() diagnostic, and
// xbox::parse_xex_image() - rather than re-deriving a full recompiled-PPC
// build pipeline for every negative case. The positive, full
// XEX -> Recomp Driver -> compiled module -> KernelProcess -> KernelThread
// round trip (including its own codegen-assertion negative checks: an
// import thunk or invalid-PPC address must never become a discovered/
// compiled function) is proven separately in guest_export_abi_tests.cpp;
// this file complements it rather than duplicating it.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/cpu/state.hpp"
#include "xenon/kernel/exception.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xex_loader.hpp"

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
constexpr std::uint32_t kTextRva = 0x10000u;
constexpr std::uint32_t kDataRva = 0x20000u;

struct ImportSpec {
  std::string library;
  std::uint32_t ordinal;
};

// Builds a minimal, structurally-valid XEX2 image (one executable .text page
// - a single `blr`, never actually reached by any of these tests, which
// stop short of start()/execution - and one .data page) whose native
// XEX_HEADER_IMPORT_LIBRARIES table lists exactly `imports`, one library
// record per entry, each pointing at its own data placeholder word (ordinal
// only, matching the real on-disk format - see xex_loader.cpp's
// parse_native_import_libraries()). `tls_data_size`, when nonzero, adds an
// XEX_HEADER_TLS_INFO optional header advertising that much per-thread TLS
// - deliberately used to force a deterministic, out-of-any-real-memory-
// pressure allocation failure in XenonSession::create_guest_process().
std::vector<std::byte> make_minimal_xex(const std::vector<ImportSpec>& imports,
                                        std::uint32_t tls_data_size = 0) {
  constexpr std::size_t header = 0x300;
  // Optional header entries occupy file offsets [0x18, 0x18 + 8*N) - up to
  // two here (imports + TLS) - so security must start clear of 0x28, unlike
  // guest_export_abi_tests.cpp's single-entry fixture which could safely use
  // 0x20. XexSecurityInfo's own fixed region is 0x184 bytes (up to
  // page_descriptor_count - see xex_loader.cpp's kSecurityInfoFixedSize), so
  // import_table must start clear of security + 0x184, and tls_info_offset
  // clear of import_table's own (up to ~0x9C-byte, for two import
  // libraries) span.
  constexpr std::size_t security = 0x40;
  constexpr std::size_t import_table = 0x1D0;
  constexpr std::size_t tls_info_offset = 0x290;  // only used if tls_data_size != 0
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = 0x600;
  constexpr std::size_t data_raw = kDataRva;
  constexpr std::size_t file_size = header + data_raw + 0x40;

  std::vector<std::byte> bytes(file_size, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));

  const std::uint32_t optional_header_count =
      (imports.empty() ? 0u : 1u) + (tls_data_size != 0 ? 1u : 0u);
  be32(bytes, 0x14, optional_header_count);

  std::size_t entry_index = 0;
  if (!imports.empty()) {
    be32(bytes, 0x18 + entry_index * 8, 0x000103FFu);  // kHeaderImportLibraries
    be32(bytes, 0x18 + entry_index * 8 + 4, static_cast<std::uint32_t>(import_table));
    ++entry_index;
  }
  if (tls_data_size != 0) {
    be32(bytes, 0x18 + entry_index * 8, 0x00020104u);  // kHeaderTlsInfo
    be32(bytes, 0x18 + entry_index * 8 + 4, static_cast<std::uint32_t>(tls_info_offset));
    ++entry_index;
    // XexTls: slot, raw_data_start, data_size, raw_data_size (all BE u32).
    // raw_data_size == 0 -> no template copy attempted, only the allocation
    // itself (sized from data_size) needs to fail.
    be32(bytes, tls_info_offset + 0x00, 0);
    be32(bytes, tls_info_offset + 0x04, 0);
    be32(bytes, tls_info_offset + 0x08, tls_data_size);
    be32(bytes, tls_info_offset + 0x0C, 0);
  }

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  if (!imports.empty()) {
    const std::size_t string_table_offset = import_table + 0xC;
    std::vector<std::uint32_t> name_offsets;
    std::size_t cursor = string_table_offset;
    for (const auto& spec : imports) {
      name_offsets.push_back(static_cast<std::uint32_t>(cursor - string_table_offset));
      write_str(bytes, cursor, spec.library.c_str());
      std::size_t advance = spec.library.size() + 1;
      advance = (advance + 3u) & ~std::size_t{3u};
      cursor += advance;
    }
    const std::uint32_t string_table_size = static_cast<std::uint32_t>(cursor - string_table_offset);

    std::size_t lib_offset = cursor;
    std::size_t data_cursor = 0x28;  // relative to data_raw, right after .data's implicit header room
    for (std::size_t i = 0; i < imports.size(); ++i) {
      const std::uint32_t thunk_rva = kDataRva + static_cast<std::uint32_t>(data_cursor);
      be32(bytes, lib_offset + 0x00, 0x2C);  // library_size = 0x28 + 4*1
      be16(bytes, lib_offset + 0x24, static_cast<std::uint16_t>(i));
      be16(bytes, lib_offset + 0x26, 1);
      be32(bytes, lib_offset + 0x28, kLoadAddress + thunk_rva);
      be32(bytes, header + thunk_rva, imports[i].ordinal);  // placeholder word at the thunk address
      lib_offset += 0x2C;
      data_cursor += 4;
    }

    const auto table_size = static_cast<std::uint32_t>(0xC + string_table_size + 0x2C * imports.size());
    be32(bytes, import_table + 0x0, table_size);
    be32(bytes, import_table + 0x4, string_table_size);
    be32(bytes, import_table + 0x8, static_cast<std::uint32_t>(imports.size()));
  }

  // PE headers.
  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 2);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);
  le32(bytes, optional + 0x1C, kLoadAddress);
  le32(bytes, optional + 0x38, kDataRva + 0x10000u);

  write_str(bytes, section, ".text");
  le32(bytes, section + 4, 0x10);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, 0x10);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);
  be32(bytes, header + text_raw, 0x4E800020u);  // blr - never actually reached by these tests

  const std::size_t data_section = section + 0x28;
  write_str(bytes, data_section, ".data");
  le32(bytes, data_section + 4, 0x40);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, 0x40);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040u);

  return bytes;
}

// --- Item 4: import identity must not collide across libraries -------------
void test_export_registry_no_cross_library_collision() {
  xenon::core::ExportRegistry registry;
  int last_called = 0;  // 1=xboxkrnl, 2=xam, 3=audio(also under "xboxkrnl" library string)

  auto register_probe = [&](const std::string& library, std::uint32_t ordinal, int tag) {
    xenon::core::ExportDescriptor descriptor{};
    descriptor.library = library;
    descriptor.ordinal = ordinal;
    descriptor.name = "SharedOrdinalProbe";
    descriptor.requirement = xenon::core::ExportRequirement::Required;
    descriptor.handler = [&last_called, tag](xenon::core::ExportCallContext&) {
      last_called = tag;
      return true;
    };
    assert(registry.register_export(std::move(descriptor)));
  };

  // The SAME ordinal (5) registered under three different libraries - this
  // is exactly the shape ExportRegistry must never resolve across.
  register_probe("xboxkrnl", 5u, 1);
  register_probe("xam", 5u, 2);
  register_probe("some_other_module", 5u, 3);

  xenon::memory::AddressSpace memory(xenon::memory::GuestTranslationMode::Auto);
  assert(memory.initialize());
  xenon::cpu::CpuState state{};

  {
    xenon::core::ExportCallContext ctx{state, memory, 0, 0};
    const auto result = registry.invoke("xboxkrnl", 5u, ctx);
    assert(result.handled && last_called == 1 && "ordinal 5 under xboxkrnl must resolve to the xboxkrnl handler");
  }
  last_called = 0;
  {
    xenon::core::ExportCallContext ctx{state, memory, 0, 0};
    const auto result = registry.invoke("xam", 5u, ctx);
    assert(result.handled && last_called == 2 && "ordinal 5 under xam must resolve to the xam handler, not xboxkrnl's");
  }
  last_called = 0;
  {
    xenon::core::ExportCallContext ctx{state, memory, 0, 0};
    const auto result = registry.invoke("some_other_module", 5u, ctx);
    assert(result.handled && last_called == 3);
  }

  // Also by name, since register_export() populates both maps.
  last_called = 0;
  {
    xenon::core::ExportCallContext ctx{state, memory, 0, 0};
    const auto result = registry.invoke("xboxkrnl", "SharedOrdinalProbe", ctx);
    assert(result.handled && last_called == 1);
  }

  std::cout << "  [ok] ExportRegistry: identical ordinal/name under different libraries never cross-resolves\n";
}

// --- Item 3(c): a malformed native import table entry must not fabricate
// a plausible-looking ordinal-0 import ------------------------------------
void test_malformed_import_entry_is_skipped() {
  // thunk address 1 is far below image_base (kLoadAddress) - a corrupt/
  // truncated table, not a real Xbox 360 XEX. Before this pass's xex_loader
  // fix, this silently produced an XexImport{ordinal=0, attributes=0} for
  // "totally_bogus_lib" instead of being rejected.
  const auto bytes = make_minimal_xex({{"totally_bogus_lib", 0 /* unused: thunk itself is bad */}});
  // Corrupt the one import's thunk address in place: re-derive its offset
  // exactly as make_minimal_xex() laid it out (single import, data_cursor
  // starts at 0x28) and overwrite it with an out-of-image address.
  auto corrupted = bytes;
  constexpr std::size_t import_table = 0x1D0;  // must match make_minimal_xex()'s own constant
  constexpr std::size_t string_table_offset = import_table + 0xC;
  const std::size_t lib_offset = string_table_offset + 20u;  // "totally_bogus_lib\0" padded to 20
  be32(corrupted, lib_offset + 0x28, 1u);  // thunk address 1 - nowhere near image_base

  xenon::xbox::XexImage image{};
  std::string error;
  const bool parsed = xenon::xbox::parse_xex_image(corrupted, image, &error);
  assert(parsed && "a malformed import entry must not fail the whole image parse");
  assert(image.imports.empty() &&
         "an import whose thunk address cannot be resolved must be skipped, not fabricated "
         "as a plausible-looking ordinal-0 import");

  std::cout << "  [ok] xex_loader: malformed import table entry (out-of-range thunk) is skipped, not fabricated\n";
}

// --- Items 3(a)/3(b): unknown library / known library, missing ordinal,
// surfaced through the real load_game() diagnostic -------------------------
void test_unresolved_import_diagnostics() {
  const auto bytes = make_minimal_xex({
      {"totally_bogus_lib", 1u},  // 3(a): library XenonSession has never heard of
      {"xboxkrnl", 0xFFFFu},      // 3(b): real library, ordinal nothing registers
  });

  xenon::core::XenonSession session;
  xenon::core::SessionConfig config{};
  config.enable_logging = false;
  config.enable_input = true;
  config.input_drivers = {"null"};
  config.enable_audio = false;
  const auto init_result = session.initialize(config);
  assert(init_result.success);

  const auto load_result = session.load_game(bytes, "unresolved_import_diagnostics");
  assert(load_result.success && "structurally valid XEX load must still succeed even with unresolvable imports");

  bool found_unknown_library = false, found_missing_ordinal = false;
  for (const auto& unresolved : session.unresolved_imports()) {
    if (unresolved.library == "totally_bogus_lib" && unresolved.ordinal == 1u) found_unknown_library = true;
    if (unresolved.library == "xboxkrnl" && unresolved.ordinal == 0xFFFFu) found_missing_ordinal = true;
  }
  assert(found_unknown_library && "an entirely unknown library must be reported as an unresolved import");
  assert(found_missing_ordinal &&
         "a real library with an ordinal nothing registers must be reported as unresolved too");

  // The production import-resolution boundary itself (XenonSession::call()):
  // a `bl` to either thunk must be an explicit, terminal failure - never a
  // silently swallowed non-terminal result that lets the guest continue
  // with a stale r3 as though the call had quietly succeeded.
  auto* memory = session.memory();
  assert(memory != nullptr);
  xenon::cpu::CpuState state{};
  for (const auto& import : session.loaded_xex()->image.imports) {
    const auto result = session.call(import.guest_thunk, state, *memory);
    assert(result.terminal() && result.reason == xenon::cpu::FlowReason::Trap &&
           "a call to a recognized-but-unimplemented import must be a terminal, diagnosable failure");
    assert(result.detail == static_cast<std::uint32_t>(xenon::kernel::ExceptionCode::ProcedureNotFound));
  }

  // And a target that is neither compiled code nor recognized as any
  // import at all (item 3's fourth negative case) must fail the exact same
  // way, not merely "the recognized-import path was diagnosable."
  const auto bogus_target = kLoadAddress + kTextRva + 0x9000u;  // inside .text's guest range, no function there
  const auto bogus_result = session.call(bogus_target, state, *memory);
  assert(bogus_result.terminal() && bogus_result.reason == xenon::cpu::FlowReason::Trap &&
         "a call target that is neither compiled guest code nor a known import must be a terminal failure");

  session.shutdown();
  std::cout << "  [ok] XenonSession: unknown library, missing ordinal, and unresolvable call targets "
               "are all explicit, terminal failures - never silent\n";
}

// --- Item 3(e): missing/incompatible native game module -------------------
void test_missing_native_module_fails_explicitly() {
  const auto bytes = make_minimal_xex({});

  xenon::core::XenonSession session;
  xenon::core::SessionConfig config{};
  config.enable_logging = false;
  config.enable_input = true;
  config.input_drivers = {"null"};
  config.native_extension_path = "Z:/this/path/definitely/does/not/exist/xenon_game_module.dll";
  const auto init_result = session.initialize(config);
  assert(init_result.success);

  const auto load_result = session.load_game(bytes, "missing_native_module");
  assert(load_result.success &&
         "load_game() must still succeed structurally - a missing native module is a startup-time "
         "concern, not a load-time one (see load_native_extension())");
  assert(!session.native_extension_bound() && "a nonexistent native module path must never report as bound");
  assert(!session.native_extension_error().empty() && "the specific failure must be diagnosable, not silent");

  const auto start_result = session.start();
  assert(!start_result.success &&
         "start() must explicitly refuse to run without a bound native extension, never pretend to execute");

  session.shutdown();
  std::cout << "  [ok] XenonSession: a missing/incompatible native game module fails start() explicitly\n";
}

// --- Item 5: KernelProcess/KernelMemory/guest-stack rollback --------------
void test_guest_process_creation_rollback() {
  // A TLS block this large can never be satisfied by Memory V2's guest
  // address space, regardless of host machine memory pressure - a fully
  // deterministic, portable way to force setup_guest_thread_tls_context()
  // to fail *after* create_guest_process() has already allocated the main
  // thread's guest stack and constructed kernel_process_/kernel_memory_.
  constexpr std::uint32_t kImpossibleTlsSize = 0xF0000000u;
  const auto oversized_tls_bytes = make_minimal_xex({}, kImpossibleTlsSize);

  xenon::core::XenonSession session;
  xenon::core::SessionConfig config{};
  config.enable_logging = false;
  config.enable_input = true;
  config.input_drivers = {"null"};
  auto init_result = session.initialize(config);
  assert(init_result.success);

  auto load_result = session.load_game(oversized_tls_bytes, "rollback_test");
  assert(!load_result.success && "an unsatisfiable TLS allocation must fail load_game(), not silently proceed");
  assert(session.kernel_process() == nullptr &&
         "create_guest_process() must roll back its own KernelProcess/KernelMemory on a later failure, "
         "not leave a half-initialized process with a registered module but no valid TLS");

  // Reusability: the session must recover to a fully working state through
  // the documented shutdown() -> initialize() cycle, not stay permanently
  // wedged after a load_game() failure.
  session.shutdown();
  assert(!session.is_initialized());

  auto reinit_result = session.initialize(config);
  assert(reinit_result.success && "a session must be fully reusable after shutdown() following a load failure");

  const auto normal_bytes = make_minimal_xex({});
  auto reload_result = session.load_game(normal_bytes, "rollback_test_recovered");
  assert(reload_result.success && "a normal XEX must load cleanly on the same session object after recovery");
  assert(session.kernel_process() != nullptr);

  session.shutdown();
  std::cout << "  [ok] XenonSession::create_guest_process(): TLS allocation failure rolls back "
               "KernelProcess/KernelMemory/the guest stack, and the session remains reusable\n";
}

}  // namespace

int main() {
  std::cout << "Testing guest import negative paths and identity/rollback guarantees...\n";
  test_export_registry_no_cross_library_collision();
  test_malformed_import_entry_is_skipped();
  test_unresolved_import_diagnostics();
  test_missing_native_module_fails_explicitly();
  test_guest_process_creation_rollback();
  std::cout << "All tests passed!\n";
  return 0;
}
