// Integration coverage for "Title Update Runtime Integration": proves that a
// selected title update actually reaches XenonSession::load_game()'s normal
// production path (xbox::apply_title_update(), not XEX Loader V2 called
// directly - see docs/runtime/RUNTIME_SESSION.md), that the resulting *effective*
// image (not the base image) is what gets mapped into guest memory, that its
// identity (xbox::XexEffectiveIdentity) is exposed and differs from the base
// image's own identity, that malformed/incompatible updates fail explicitly
// with no partial guest-process state left behind, and that a native
// extension's declared effective-executable-revision compatibility is
// actually enforced before it is allowed to bind.
//
// Fixtures are minimal, structurally-valid, uncompressed/unencrypted XEX2
// images built by make_identity_xex() below (module_flags + an
// XEX_HEADER_EXECUTION_INFO optional header + one distinguishing content
// byte) - a "full patch" (XEX_MODULE_PATCH_FULL) title update needs no LZX
// delta framing at all, so this proves the real apply_title_update() patch-
// validation/application path without the extra fixture complexity a delta
// patch would need (that framing is already covered directly against XEX
// Loader V2 in tests/xbox/xex_loader_tests.cpp).

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/core/session.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xex_loader.hpp"

#ifndef XENON_TITLE_UPDATE_TEST_MODULE_PATH
#error "XENON_TITLE_UPDATE_TEST_MODULE_PATH must be defined by the build (see CMakeLists.txt)"
#endif

namespace {

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
// Guest address of the one content byte make_identity_xex() varies between
// fixtures - reading it back after load_game() proves the ACTUAL mapped
// guest memory reflects the effective (possibly title-update-patched) image,
// not merely that a computed identity hash differs.
constexpr std::uint32_t kMarkerAddress = kLoadAddress + kTextRva + 4u;

struct ExecInfoSpec {
  std::uint32_t title_id;
  std::uint32_t media_id;
  std::uint32_t version;       // xbox::XexVersion::value
  std::uint32_t base_version;  // xbox::XexVersion::value
};

// Builds a minimal, structurally-valid, uncompressed/unencrypted XEX2 image
// with a real XEX_HEADER_EXECUTION_INFO optional header (so title/media/
// version identity is genuinely parsed, not left at the zero default) and a
// one-byte `content_marker` written into an otherwise-fixed .text section, so
// two images built from different (module_flags, exec, content_marker)
// values are byte-identical everywhere except that one marker - making the
// resulting effective_image hash differ if and only if the marker differs.
// Layout mirrors tests/core/guest_import_negative_tests.cpp's proven-working
// make_minimal_xex() (same header/security/PE offset scheme), minus the
// imports/TLS optional headers this file's tests do not need.
std::vector<std::byte> make_identity_xex(std::uint32_t module_flags, const ExecInfoSpec& exec,
                                         std::byte content_marker) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x40;                  // fixed 0x184 bytes -> ends 0x1C4
  constexpr std::size_t execution_info_offset = 0x1D0;     // 0x18 bytes -> ends 0x1E8, clear of security
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
  be32(bytes, 4, module_flags);
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));
  be32(bytes, 0x14, 1u);  // one optional header: execution info
  be32(bytes, 0x18, 0x00040006u);  // kHeaderExecutionInfo
  be32(bytes, 0x1C, static_cast<std::uint32_t>(execution_info_offset));

  be32(bytes, security + 0x000, 0x184u);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  // xex2_opt_execution_info: media_id, version, base_version, title_id (all
  // BE32), then platform/executable_table/disc_number/disc_count bytes and
  // a savegame_id BE32 - all left zero, unused by these tests.
  be32(bytes, execution_info_offset + 0x00, exec.media_id);
  be32(bytes, execution_info_offset + 0x04, exec.version);
  be32(bytes, execution_info_offset + 0x08, exec.base_version);
  be32(bytes, execution_info_offset + 0x0C, exec.title_id);

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
  // docs/xbox/XEX_LOADER_V2.md: the loader reads every section's bytes by
  // RVA from the effective image, never by PE PointerToRawData (retained
  // only as metadata) - so both the (unreached) blr and, critically, the
  // content_marker the tests below actually read back through guest memory
  // must be written at the RVA-based file offset (header + kTextRva), not
  // at header + text_raw. Writing them at text_raw put the marker where
  // effective_image never looks, so mapped guest memory always read 0
  // regardless of content_marker's value.
  be32(bytes, header + kTextRva, 0x4E800020u);  // blr - never actually reached by these tests
  bytes[header + kTextRva + 4] = content_marker;

  const std::size_t data_section = section + 0x28;
  write_str(bytes, data_section, ".data");
  le32(bytes, data_section + 4, 0x40);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, 0x40);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040u);

  return bytes;
}

constexpr std::uint32_t kTitleId = 0x4D530001u;
constexpr std::uint32_t kMediaId = 0x00112233u;
constexpr std::uint32_t kOtherTitleId = 0x4D539999u;
constexpr std::uint32_t kOtherMediaId = 0x00999999u;
constexpr std::byte kBaseMarker{0xAAu};
constexpr std::byte kUpdateMarker{0xBBu};

std::vector<std::byte> make_base_xex() {
  return make_identity_xex(0u, {kTitleId, kMediaId, 0x00000001u, 0u}, kBaseMarker);
}

std::vector<std::byte> make_valid_update_xex() {
  return make_identity_xex(xenon::xbox::module_flags::kModulePatch | xenon::xbox::module_flags::kPatchFull,
                           {kTitleId, kMediaId, 0x00000002u, 0x00000001u}, kUpdateMarker);
}

xenon::xbox::XexEffectiveIdentity reference_identity(const std::vector<std::byte>& xex_bytes,
                                                     const std::vector<std::byte>* update_bytes) {
  xenon::xbox::XexImage base{};
  std::string error;
  assert(xenon::xbox::parse_xex_image(xex_bytes, base, &error) && "reference base parse must succeed");
  if (!update_bytes) return xenon::xbox::compute_effective_identity(base);
  xenon::xbox::XexImage patched{};
  assert(xenon::xbox::apply_title_update(base, *update_bytes, patched, &error) &&
         "reference title-update application must succeed");
  return xenon::xbox::compute_effective_identity(base, &patched);
}

xenon::core::SessionConfig make_config() {
  xenon::core::SessionConfig config{};
  config.enable_logging = false;
  config.enable_input = true;
  config.input_drivers = {"null"};
  config.enable_audio = false;
  return config;
}

// --- Section 16/17: base-only launch keeps the base image's own identity ---
void test_base_only_launch() {
  const auto base_bytes = make_base_xex();
  const auto expected = reference_identity(base_bytes, nullptr);

  xenon::core::XenonSession session;
  auto init_result = session.initialize(make_config());
  assert(init_result.success);

  auto load_result = session.load_game(base_bytes, "base_only");
  assert(load_result.success && "a base-only launch (no title update) must still succeed");

  const auto& identity = session.effective_identity();
  assert(identity.has_value());
  assert(!identity->title_update_applied);
  assert(identity->title_id == kTitleId);
  assert(identity->effective_image_hash == expected.effective_image_hash);

  std::array<std::byte, 1> marker{};
  session.memory()->read_bytes(kMarkerAddress, marker);
  assert(marker[0] == kBaseMarker && "mapped guest memory must contain the base image's own bytes");

  // Part 12 of the AC6 Runtime Readiness pass ("Title Update fidelity"):
  // capability_report()'s "titleUpdate" section - Base SHA1 must equal
  // Effective SHA1 when no update was applied.
  {
    const auto report = session.capability_report();
    const auto* title_update = report.find("sections")->find("titleUpdate");
    assert(title_update != nullptr && title_update->is_object());
    assert(title_update->get_bool("titleUpdateApplied") == false);
    assert(title_update->get_string("baseSha1") == title_update->get_string("effectiveSha1"));
    assert(title_update->get_string("tuIdentity") == "none");
  }

  session.shutdown();
  std::cout << "  [ok] base-only launch: no title update required, effective identity == base identity\n";
}

// --- Section 16/17/19: a valid title update is actually applied end-to-end -
void test_valid_title_update_applied() {
  const auto base_bytes = make_base_xex();
  const auto update_bytes = make_valid_update_xex();
  const auto base_identity = reference_identity(base_bytes, nullptr);
  const auto expected = reference_identity(base_bytes, &update_bytes);
  // Section 13 (source immutability): snapshot the exact input bytes before
  // load_game() so their std::vector storage can be compared byte-for-byte
  // afterward - passing them as std::span<const std::byte> already prevents
  // XenonSession from writing through them, but this proves it directly
  // rather than relying on the type system alone.
  const auto base_bytes_snapshot = base_bytes;
  const auto update_bytes_snapshot = update_bytes;

  xenon::core::XenonSession session;
  auto init_result = session.initialize(make_config());
  assert(init_result.success);

  auto load_result = session.load_game(base_bytes, "with_title_update", update_bytes);
  assert(load_result.success && "a valid title update must be applied and load successfully");
  assert(base_bytes == base_bytes_snapshot &&
         "the base XEX's own bytes must remain completely immutable through load_game()");
  assert(update_bytes == update_bytes_snapshot &&
         "the title update's own bytes must remain completely immutable through load_game()");

  const auto& identity = session.effective_identity();
  assert(identity.has_value());
  assert(identity->title_update_applied);
  assert(identity->effective_image_hash == expected.effective_image_hash);
  assert(identity->effective_image_hash != base_identity.effective_image_hash &&
         "the effective (patched) image must hash differently from the base image");
  assert(identity->base_version.value == 0x00000001u);
  assert(identity->effective_version.value == 0x00000002u);

  // The strongest proof available without a full recompiled-code round trip
  // (see this file's header comment): the guest memory XenonSession actually
  // mapped for execution contains the TITLE UPDATE's byte, not the base
  // image's - i.e. xbox::apply_title_update()'s result, not the base XEX,
  // is what reached the production load_game() -> map_xex_image() path.
  std::array<std::byte, 1> marker{};
  session.memory()->read_bytes(kMarkerAddress, marker);
  assert(marker[0] == kUpdateMarker &&
         "mapped guest memory must contain the TITLE UPDATE's bytes, not the base image's");

  // Part 12 of the AC6 Runtime Readiness pass: Base SHA1 must be the BASE
  // image's own hash (distinct from Effective SHA1) even though what
  // actually ran is the patched image - no base/TU cross-contamination.
  {
    const auto report = session.capability_report();
    const auto* title_update = report.find("sections")->find("titleUpdate");
    assert(title_update != nullptr && title_update->is_object());
    assert(title_update->get_bool("titleUpdateApplied") == true);
    const auto base_sha1 = title_update->get_string("baseSha1");
    const auto effective_sha1 = title_update->get_string("effectiveSha1");
    assert(base_sha1 != effective_sha1);
    assert(base_sha1 == xenon::xbox::format_effective_image_hash(base_identity.effective_image_hash));
    assert(effective_sha1 == xenon::xbox::format_effective_image_hash(expected.effective_image_hash));
    assert(title_update->get_string("tuIdentity") == "1.0.0.0+2.0.0.0");
    assert(title_update->get_string("effectiveVersion") == "2.0.0.0");
  }

  session.shutdown();
  std::cout << "  [ok] valid title update: apply_title_update() result is what gets mapped and run, "
               "effective identity differs from the base image's\n";
}

// --- Section 12/16/21: malformed title update fails explicitly, no partial
// guest-process state, session remains reusable -----------------------------
void test_malformed_title_update_rejected() {
  const auto base_bytes = make_base_xex();
  const std::vector<std::byte> malformed_update(4, std::byte{0});  // far too small to contain a header

  xenon::core::XenonSession session;
  auto init_result = session.initialize(make_config());
  assert(init_result.success);

  auto load_result = session.load_game(base_bytes, "malformed_tu", malformed_update);
  assert(!load_result.success && "a malformed title update must fail load_game(), never fall back silently");
  assert(!session.effective_identity().has_value() &&
         "no effective identity may be published for a failed title-update application");
  assert(session.kernel_process() == nullptr &&
         "a title-update failure must leave no partially-created guest process");
  assert(session.loaded_xex() == nullptr && "no XEX may be reported loaded after a failed title-update apply");

  // Reusability: same contract guest_import_negative_tests.cpp already
  // proves for other load_game() failures - a title-update failure must not
  // wedge the session.
  session.shutdown();
  assert(!session.is_initialized());
  auto reinit_result = session.initialize(make_config());
  assert(reinit_result.success);
  auto recovered = session.load_game(base_bytes, "malformed_tu_recovered");
  assert(recovered.success && "the session must load a normal base XEX cleanly after a title-update failure");
  session.shutdown();

  std::cout << "  [ok] malformed title update: load_game() fails explicitly with no partial guest-process "
               "state, and the session remains reusable\n";
}

// --- Section 16: wrong title ID must be rejected, never silently accepted -
void test_wrong_title_id_rejected() {
  const auto base_bytes = make_base_xex();
  const auto wrong_title_update =
      make_identity_xex(xenon::xbox::module_flags::kModulePatch | xenon::xbox::module_flags::kPatchFull,
                        {kOtherTitleId, kMediaId, 0x00000002u, 0x00000001u}, kUpdateMarker);

  xenon::core::XenonSession session;
  assert(session.initialize(make_config()).success);
  auto load_result = session.load_game(base_bytes, "wrong_title", wrong_title_update);
  assert(!load_result.success && "a title update whose title ID does not match the base image must be rejected");
  assert(session.kernel_process() == nullptr);
  session.shutdown();

  std::cout << "  [ok] title update with mismatched title ID is rejected\n";
}

// --- Section 16: wrong media ID must be rejected ---------------------------
void test_wrong_media_id_rejected() {
  const auto base_bytes = make_base_xex();
  const auto wrong_media_update =
      make_identity_xex(xenon::xbox::module_flags::kModulePatch | xenon::xbox::module_flags::kPatchFull,
                        {kTitleId, kOtherMediaId, 0x00000002u, 0x00000001u}, kUpdateMarker);

  xenon::core::XenonSession session;
  assert(session.initialize(make_config()).success);
  auto load_result = session.load_game(base_bytes, "wrong_media", wrong_media_update);
  assert(!load_result.success && "a title update whose media ID does not match the base image must be rejected");
  assert(session.kernel_process() == nullptr);
  session.shutdown();

  std::cout << "  [ok] title update with mismatched media ID is rejected\n";
}

// --- Section 7/18: native-extension effective-revision compatibility gating
void set_declared_revisions(const std::string& value) {
#if defined(_WIN32)
  ::_putenv_s("XENON_TEST_SUPPORTED_REVISIONS", value.c_str());
#else
  ::setenv("XENON_TEST_SUPPORTED_REVISIONS", value.c_str(), 1);
#endif
}

void test_module_compatibility_gating() {
  const auto base_bytes = make_base_xex();
  const auto update_bytes = make_valid_update_xex();
  const auto base_identity = reference_identity(base_bytes, nullptr);
  const auto update_identity = reference_identity(base_bytes, &update_bytes);
  const auto base_hash_hex = xenon::xbox::format_effective_image_hash(base_identity.effective_image_hash);
  const auto update_hash_hex = xenon::xbox::format_effective_image_hash(update_identity.effective_image_hash);

  auto config = make_config();
  config.native_extension_path = XENON_TITLE_UPDATE_TEST_MODULE_PATH;

  // (a) module declares the base revision, launch is base-only -> compatible.
  {
    set_declared_revisions(base_hash_hex);
    xenon::core::XenonSession session;
    assert(session.initialize(config).success);
    assert(session.load_game(base_bytes, "compat_base_base").success);
    assert(session.native_extension_bound() &&
           "a module declaring the base revision must bind against a base-only launch");
    session.shutdown();
  }

  // (b) module declares the base revision only, launch has a title update ->
  // reject (section 7's first required failure example).
  {
    set_declared_revisions(base_hash_hex);
    xenon::core::XenonSession session;
    assert(session.initialize(config).success);
    assert(session.load_game(base_bytes, "compat_base_tu", update_bytes).success &&
           "load_game() itself still succeeds structurally - compatibility is a bind-time concern");
    assert(!session.native_extension_bound() &&
           "a module compiled for the base revision must be rejected against a title-update launch");
    assert(!session.native_extension_error().empty());
    session.shutdown();
  }

  // (c) module declares the title-update revision, launch has that title
  // update -> compatible.
  {
    set_declared_revisions(update_hash_hex);
    xenon::core::XenonSession session;
    assert(session.initialize(config).success);
    assert(session.load_game(base_bytes, "compat_tu_tu", update_bytes).success);
    assert(session.native_extension_bound() &&
           "a module declaring the title-update revision must bind against that title-update launch");
    session.shutdown();
  }

  // (d) module declares the title-update revision only, launch is base-only
  // -> reject (section 7's second required failure example).
  {
    set_declared_revisions(update_hash_hex);
    xenon::core::XenonSession session;
    assert(session.initialize(config).success);
    assert(session.load_game(base_bytes, "compat_tu_base").success);
    assert(!session.native_extension_bound() &&
           "a module compiled for a title-update revision must be rejected against a base-only launch");
    session.shutdown();
  }

  // (e) module declares an unrelated hash -> reject regardless of launch.
  {
    set_declared_revisions(std::string(40, 'f'));
    xenon::core::XenonSession session;
    assert(session.initialize(config).success);
    assert(session.load_game(base_bytes, "compat_wrong_hash").success);
    assert(!session.native_extension_bound());
    session.shutdown();
  }

  // (f) module declares nothing (back-compat: no identity metadata means
  // "not checked", never a rejection) -> compatible regardless of launch.
  {
    set_declared_revisions("");
    xenon::core::XenonSession session;
    assert(session.initialize(config).success);
    assert(session.load_game(base_bytes, "compat_no_declaration", update_bytes).success);
    assert(session.native_extension_bound() &&
           "a module that declares no supported revisions must not be identity-checked");
    session.shutdown();
  }

  std::cout << "  [ok] native-extension effective-revision compatibility: matching revisions bind, "
               "mismatched/undeclared-vs-required revisions are rejected explicitly, and modules "
               "declaring nothing remain back-compat\n";
}

}  // namespace

int main() {
  std::cout << "Testing title-update runtime integration (Content Services -> XEX Loader V2 -> "
               "XenonSession)...\n";
  test_base_only_launch();
  test_valid_title_update_applied();
  test_malformed_title_update_rejected();
  test_wrong_title_id_rejected();
  test_wrong_media_id_rejected();
  test_module_compatibility_gating();
  std::cout << "All tests passed!\n";
  return 0;
}
