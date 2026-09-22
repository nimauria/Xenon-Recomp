// Proves Part 4 of the Gracemeria readiness pass: a guest audio
// render-driver callback executes with a real KernelProcess -> KernelThread
// -> KPCR/TLS identity (XenonSession::start_audio_guest_thread()/
// invoke_audio_callback()), not an isolated bare CpuState.
//
// Mirrors tests/core/guest_export_abi_tests.cpp's approach (a synthetic XEX
// built with real PPC encodings, run through the real XEX Loader V2 ->
// Recomp Driver -> CPU V2 codegen -> compiled native module -> XenonSession
// pipeline) rather than calling XenonSession internals directly, so this
// exercises the exact same production path a real title's audio callback
// would.
//
// The synthetic module has three guest functions:
//   - entry (main thread): captures its own r13 (KPCR pointer) to
//     kMainKpcrResultRva, then returns.
//   - audio_callback (registered as an Xbox render-driver callback via
//     AudioSystem::register_render_client()): captures its own r13 to
//     kAudioKpcrResultRva (must differ from the main thread's - proves the
//     audio callback thread has its own independent KPCR/TLS, not the main
//     thread's or none at all - see Part 4.5), makes a genuine LOCAL
//     guest-to-guest `bl` to local_helper (Part 4.7 - normal CPU V2 dispatch,
//     no special callback dispatcher), and calls a real xboxkrnl export
//     (XAudioGetUnderrunCount) through the same production
//     ExportRegistry/external_call path every other guest caller uses
//     (Part 4.6).
//   - local_helper: r3 = r3 + 1, called only from audio_callback.
//
// Also proves Part 4.8 (clean callback shutdown): session.shutdown() is
// called with the render client still registered (callback activity
// possible), and must join the audio callback KernelThread rather than
// hang or leave it detached.

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

#include "xenon/audio/system.hpp"
#include "xenon/core/session.hpp"
#include "xenon/recomp/driver.hpp"

#if !defined(XENON_HAS_AUDIO)
#error "audio_guest_callback_tests requires xenon_core built with XENON_HAS_AUDIO (real Audio V1)"
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
constexpr std::uint32_t kTextRva = 0x10000u;
constexpr std::uint32_t kDataRva = 0x20000u;
constexpr std::uint32_t kMainKpcrResultRva = kDataRva + 0x00u;
constexpr std::uint32_t kAudioKpcrResultRva = kDataRva + 0x04u;
constexpr std::uint32_t kLocalCallResultRva = kDataRva + 0x08u;
constexpr std::uint32_t kExportCallResultRva = kDataRva + 0x0Cu;
constexpr std::uint32_t kThunkAudioRva = kDataRva + 0x10u;

constexpr std::uint32_t kAudioUnderrunOrdinal = 0x35Au;  // xboxkrnl!XAudioGetUnderrunCount

std::vector<std::byte> make_audio_callback_xex() {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t import_table = 0x1A4;
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
  be32(bytes, 0x14, 1);

  be32(bytes, 0x18, 0x000103FFu);
  be32(bytes, 0x1C, static_cast<std::uint32_t>(import_table));

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  const std::size_t string_table_offset = import_table + 0xC;
  write_str(bytes, string_table_offset, "xboxkrnl");
  constexpr std::uint32_t string_table_size = 12;

  const std::size_t lib0 = string_table_offset + string_table_size;
  be32(bytes, lib0 + 0x00, 0x2C);  // library_size = 0x28 + 4*1
  be16(bytes, lib0 + 0x24, 0);     // name_index
  be16(bytes, lib0 + 0x26, 1);     // import_count
  be32(bytes, lib0 + 0x28, kLoadAddress + kThunkAudioRva);

  const std::uint32_t table_size =
      static_cast<std::uint32_t>(0xC + string_table_size + 0x2C);
  be32(bytes, import_table + 0x0, table_size);
  be32(bytes, import_table + 0x4, string_table_size);
  be32(bytes, import_table + 0x8, 1);

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
  le32(bytes, section + 4, 0x80);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, 0x80);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);

  const std::size_t data_section = section + 0x28;
  write_str(bytes, data_section, ".data");
  le32(bytes, data_section + 4, 0x20);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, 0x20);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040);

  const std::size_t data_file_base = header + data_raw;
  be32(bytes, data_file_base + 0x00, 0xDEADBEEFu);  // main_kpcr_result sentinel
  be32(bytes, data_file_base + 0x04, 0xDEADBEEFu);  // audio_kpcr_result sentinel
  be32(bytes, data_file_base + 0x08, 0xDEADBEEFu);  // local_call_result sentinel
  be32(bytes, data_file_base + 0x0C, 0xDEADBEEFu);  // export_call_result sentinel
  be32(bytes, data_file_base + 0x10, kAudioUnderrunOrdinal);  // import placeholder

  // .text (word indices; text_base = kLoadAddress + kTextRva):
  //   0-3   entry:          lis/ori/stw r13,(main_kpcr_result); blr
  //   4-16  audio_callback: lis/ori/stw r13,(audio_kpcr_result); li r3,41;
  //                         bl local_helper; lis/ori/stw r3,(local_call_result);
  //                         bl xboxkrnl_thunk; lis/ori/stw r3,(export_call_result); blr
  //   17-18 local_helper:   addi r3,r3,1; blr
  const std::uint32_t text_base = kLoadAddress + kTextRva;
  const std::uint32_t audio_callback_addr = text_base + 4u * 4u;
  const std::uint32_t local_helper_addr = text_base + 17u * 4u;
  const std::uint32_t audio_thunk = kLoadAddress + kThunkAudioRva;
  const std::uint32_t main_kpcr_result = kLoadAddress + kMainKpcrResultRva;
  const std::uint32_t audio_kpcr_result = kLoadAddress + kAudioKpcrResultRva;
  const std::uint32_t local_call_result = kLoadAddress + kLocalCallResultRva;
  const std::uint32_t export_call_result = kLoadAddress + kExportCallResultRva;

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

  std::array<std::uint32_t, 19> words{};
  words[0] = d_form(15, 6, 0, hi16(main_kpcr_result));   // lis r6,hi16
  words[1] = d_form(24, 6, 6, lo16(main_kpcr_result));   // ori r6,r6,lo16
  words[2] = d_form(36, 13, 6, 0);                       // stw r13,0(r6)
  words[3] = 0x4E800020u;                                // blr

  words[4] = d_form(15, 6, 0, hi16(audio_kpcr_result));
  words[5] = d_form(24, 6, 6, lo16(audio_kpcr_result));
  words[6] = d_form(36, 13, 6, 0);                       // stw r13,0(r6)
  words[7] = d_form(14, 3, 0, 41);                       // li r3,41
  words[8] = bl_rel(text_base + 8u * 4u, local_helper_addr);
  words[9] = d_form(15, 6, 0, hi16(local_call_result));
  words[10] = d_form(24, 6, 6, lo16(local_call_result));
  words[11] = d_form(36, 3, 6, 0);                       // stw r3,0(r6)
  words[12] = bl_rel(text_base + 12u * 4u, audio_thunk);
  words[13] = d_form(15, 6, 0, hi16(export_call_result));
  words[14] = d_form(24, 6, 6, lo16(export_call_result));
  words[15] = d_form(36, 3, 6, 0);                       // stw r3,0(r6)
  words[16] = 0x4E800020u;                               // blr

  words[17] = d_form(14, 3, 3, 1);                       // addi r3,r3,1
  words[18] = 0x4E800020u;                               // blr

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
  std::cout << "Testing audio guest callback KernelThread/TLS integration...\n";

  const auto root = std::filesystem::temp_directory_path() / "xenon_audio_guest_callback_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto input = root / "fixture.xex";
  const auto output = root / "generated";
  const auto xex_bytes = make_audio_callback_xex();
  write_file(input, xex_bytes);

  // audio_callback is never reached by any direct call/branch from the
  // entry point - it is a real Xbox render-driver callback, only ever
  // invoked dynamically through AudioSystem::register_render_client() at
  // runtime, exactly like a real title's callback address that only ever
  // appears as a runtime argument, never a static `bl` target. A ModuleHint
  // function-boundary seed is the existing, production mechanism for
  // exactly this case (see include/xenon/recomp/driver.hpp's ModuleHint and
  // Part 1/2 of the Gracemeria readiness pass, which extends this further).
  const std::uint32_t audio_callback_seed_addr = kLoadAddress + kTextRva + 4u * 4u;
  xenon::recomp::ModuleHint hint{};
  hint.name = "audio_guest_callback_test";
  hint.function_boundaries.push_back(audio_callback_seed_addr);
  xenon::recomp::DriverOptions options;
  options.input = input;
  options.output = output;
  options.hints.push_back(std::move(hint));
  xenon::recomp::AnalysisReport report;
  std::string error;
  assert(xenon::recomp::load_and_analyze(options, report, error) && error.empty());
  assert(report.image.imports.size() == 1);

  bool entry_compiled = false, callback_compiled = false, helper_compiled = false;
  const std::uint32_t text_base = kLoadAddress + kTextRva;
  for (const auto& function : report.functions) {
    if (function.guest_start == text_base) { entry_compiled = true; assert(function.compiled); }
    if (function.guest_start == text_base + 4u * 4u) { callback_compiled = true; assert(function.compiled); }
    if (function.guest_start == text_base + 17u * 4u) { helper_compiled = true; assert(function.compiled); }
  }
  assert(entry_compiled && "main entry function should compile");
  assert(callback_compiled && "audio_callback function should compile");
  assert(helper_compiled && "local_helper function should compile");

  assert(xenon::recomp::generate_project(options, report, error) && error.empty());

#if defined(XENON_SOURCE_ROOT)
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

  std::filesystem::path module_path;
  for (const auto* candidate :
       {"xenon_game_module.dll", "Debug/xenon_game_module.dll", "libxenon_game_module.so"}) {
    auto path = build_dir / candidate;
    if (std::filesystem::exists(path)) { module_path = path; break; }
  }
  assert(!module_path.empty() && "built xenon_game_module shared library not found");

  xenon::core::XenonSession session;
  xenon::core::SessionConfig config{};
  config.enable_logging = true;
  config.enable_graphics = false;
  config.enable_input = false;
  config.enable_audio = true;
  config.native_extension_path = module_path.string();

  auto init_result = session.initialize(config);
  assert(init_result.success && "session initialization should succeed");

  auto load_result = session.load_game(xex_bytes, "audio_guest_callback_test");
  if (!load_result.success) std::cout << "load_game failed: " << load_result.message << "\n";
  assert(load_result.success);
  assert(session.kernel_process() != nullptr);
  assert(session.native_extension_bound());
  auto* audio = session.audio();
  assert(audio != nullptr && "audio subsystem must be present with enable_audio=true");

  // Run the main thread so it captures its own KPCR into main_kpcr_result.
  auto start_result = session.start();
  assert(start_result.success);
  for (int i = 0; i < 200 && session.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(!session.execution_active());
  assert(session.last_error().empty());

  // Register the audio_callback function as a real Xbox render-driver
  // callback. register_render_client() grants kMaxQueuedRenderFrames credits
  // immediately (see tests/audio/system_tests.cpp), so the callback fires
  // through the pump loop without needing to submit any frames first.
  const std::uint32_t audio_callback_addr = text_base + 4u * 4u;
  const auto client = audio->register_render_client(audio_callback_addr, 0xCAFEBABEu);
  assert(client.has_value() && "registering a render client should succeed");

  auto* memory = session.memory();
  assert(memory != nullptr);
  bool callback_ran = false;
  for (int i = 0; i < 500 && !callback_ran; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    callback_ran = memory->read32_be(kLoadAddress + kExportCallResultRva) != 0xDEADBEEFu;
  }
  assert(callback_ran && "audio_callback should have executed within the timeout");

  // Part 4.5: the audio callback thread must have its own KPCR, distinct
  // from the main thread's - proves it is a real, independent guest thread
  // identity, not the main thread's context reused or a shared/absent one.
  const auto main_kpcr = memory->read32_be(kLoadAddress + kMainKpcrResultRva);
  const auto audio_kpcr = memory->read32_be(kLoadAddress + kAudioKpcrResultRva);
  assert(main_kpcr != 0xDEADBEEFu && "main thread should have captured its KPCR");
  assert(audio_kpcr != 0xDEADBEEFu && "audio callback should have captured its KPCR");
  assert(main_kpcr != 0 && audio_kpcr != 0 && "captured KPCR addresses must be real, non-null guest addresses");
  assert(main_kpcr != audio_kpcr &&
         "audio callback thread must have its own KPCR/TLS, distinct from the main thread's");

  // Part 4.7: a genuine guest-to-guest bl from inside the callback actually
  // ran through normal CPU V2 dispatch (41 + 1 == 42), no special callback
  // dispatcher.
  const auto local_result = memory->read32_be(kLoadAddress + kLocalCallResultRva);
  assert(local_result == 42u &&
         "audio_callback's local guest-to-guest call must actually execute (41 + 1 == 42)");

  // Part 4.6: a real xboxkrnl export called from inside the callback reached
  // the same production ExportRegistry/AudioSystem as any other caller.
  // Unlike guest_export_abi_tests' no-render-client case, this test
  // registers an active render client to get callbacks at all, so the real
  // host audio device's own render() calls may race in and genuinely
  // increment the underrun counter - the proof here is that a real,
  // non-sentinel value came back from real production code, not a specific
  // count.
  const auto export_result = memory->read32_be(kLoadAddress + kExportCallResultRva);
  assert(export_result != 0xDEADBEEFu &&
         "XAudioGetUnderrunCount's result was never written by the guest call from inside the callback");

  // Part 4.8: shut down with the render client still registered (callback
  // activity possible/likely still pending) - must join the audio callback
  // KernelThread cleanly rather than hang or leave it detached.
  session.shutdown();
  assert(!session.is_initialized() && "session should be cleanly shut down");
  std::cout << "  [ok] shutdown with active audio render client completed cleanly (no hang)\n";
#else
  std::cout << "  (skipping full build/execution stage: XENON_SOURCE_ROOT not defined for this "
               "target - analysis/codegen-assertion coverage above still ran)\n";
#endif

  std::filesystem::remove_all(root);
  std::cout << "All tests passed!\n";
  return 0;
}
