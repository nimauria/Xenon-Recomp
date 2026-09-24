// End-to-end tests for the xenon-prepare automatic game preparation worker
// (tools/xenon_prepare.cpp): first-Play build, no-rebuild on a second Play,
// rebuild when a module's analysis hints actually change (while the
// superseded entry's absence is verified without disturbing anything else),
// and deterministic cancellation. Mirrors tests/recomp/recomp_driver_tests.cpp
// and tests/recomp/module_hint_provider_tests.cpp's fixture-building style,
// but drives the real xenon-prepare BINARY as a subprocess - this is what
// actually proves the worker (not just the library calls it wraps) behaves
// correctly end to end.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "xenon/core/json.hpp"
#include "xenon/recomp/analysis_schema_json.hpp"
#include "xenon/recomp/artifact_cache.hpp"
#include "xenon/recomp/compilation_graph.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_loader.hpp"

using namespace xenon::recomp;
using namespace xenon::recomp::analysis;
using xenon::core::JsonValue;
namespace xbox = xenon::xbox;

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

// Same minimal-but-real single-entry-function fixture shape as
// recomp_driver_tests.cpp/module_hint_provider_tests.cpp.
std::vector<std::byte> make_xex() {
  constexpr std::size_t header = 0x200;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x280;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::uint32_t load_address = 0x80000000u;
  std::vector<std::byte> bytes(0x800, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, header); be32(bytes, 0x10, security); be32(bytes, 0x14, 0);

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, load_address);

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C); le16(bytes, coff + 2, 1);
  le16(bytes, coff + 0x10, 0xE0); le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, 0x1000);
  le32(bytes, optional + 0x1C, load_address);
  le32(bytes, optional + 0x38, 0x2000);
  bytes[section] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  le32(bytes, section + 4, 0x20);
  le32(bytes, section + 0x0C, 0x1000);
  le32(bytes, section + 0x10, 0x20);
  le32(bytes, section + 0x14, 0x400);
  le32(bytes, section + 0x24, 0x60000020);
  be32(bytes, header + 0x400, 0x4E800020);  // blr
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

std::string quote(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }

// std::system() on Windows runs the command through `cmd /c`. cmd's own
// argument-stripping rule only preserves inner quoting when the WHOLE
// command line is wrapped in exactly one extra pair of quotes; without it,
// cmd blindly strips the first and last quote characters of the entire
// string (not each individually-quoted token), corrupting a command that -
// like this test's - starts with a quoted executable path followed by
// several more quoted arguments. This is unrelated to (and does not affect)
// xenon-prepare's own child-process invocations, which use CreateProcessW
// directly and never go through cmd.exe.
int run_system(const std::string& command) {
#if defined(_WIN32)
  return std::system(("\"" + command + "\"").c_str());
#else
  return std::system(command.c_str());
#endif
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::uint64_t hash_hint_set(const AnalysisHintSetV2& hint_set) {
  const auto json_text = to_json(hint_set).dump();
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(json_text.data()),
                                         json_text.size());
  const auto digest = xbox::crypto::sha1(bytes);
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8 && i < digest.size(); ++i) {
    value = (value << 8) | std::to_integer<std::uint64_t>(digest[i]);
  }
  return value;
}

std::string default_target_arch() {
#if defined(_M_X64) || defined(__x86_64__)
  return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#else
  return "x86";
#endif
}

ArtifactCacheKey expected_key(const xbox::XexEffectiveIdentity& identity,
                             const AnalysisHintSetV2& hints) {
  ArtifactCacheKey key;
  key.title_id = identity.title_id;
  key.media_id = identity.media_id;
  key.effective_image_hash = xbox::format_effective_image_hash(identity.effective_image_hash);
  key.xex_relative_path = "default.xex";
  key.module_id = "xenon_test_module";
  key.module_compatibility_version = "1";
  key.hint_set_hash = hash_hint_set(hints);
  key.preparation_identity = xenon::recomp::graph::preparation_identity(
      std::filesystem::path(XENON_SOURCE_ROOT), XENON_CMAKE_COMMAND, XENON_NATIVE_COMPILER);
  key.target_arch = default_target_arch();
  key.build_config = "Release";
  return key;
}

}  // namespace

int main() {
#if !defined(XENON_PREPARE_EXECUTABLE) || !defined(XENON_SOURCE_ROOT) || \
    !defined(XENON_NATIVE_COMPILER) || !defined(XENON_CMAKE_COMMAND)
#error "XENON_PREPARE_EXECUTABLE, XENON_SOURCE_ROOT, XENON_NATIVE_COMPILER and XENON_CMAKE_COMMAND must be defined for this test"
#endif
  std::cout << "Testing xenon-prepare worker end to end...\n";

  const auto root = std::filesystem::temp_directory_path() / "xenon_prepare_worker_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto content_dir = root / "content";
  std::filesystem::create_directories(content_dir);
  const auto xex_bytes = make_xex();
  write_file(content_dir / "default.xex", xex_bytes);

  xbox::XexImage image{};
  std::string parse_error;
  assert(xbox::parse_xex_image(xex_bytes, image, &parse_error));
  const auto identity = xbox::compute_effective_identity(image);
  const auto revision_hex = xbox::format_effective_image_hash(identity.effective_image_hash);

  const auto module_dir = root / "module";
  std::filesystem::create_directories(module_dir / "revisions" / revision_hex);
  write_text_file(module_dir / "manifest.json",
                  R"json({"moduleName": "xenon_test_module", "compatibilityVersion": "1", )json"
                  R"json("revisions": [")json" + revision_hex + R"json("]})json");

  AnalysisHintSetV2 hints_v1{};
  hints_v1.module_name = "xenon_test_module";
  hints_v1.identity = identity;
  FunctionHint fn_v1{};
  fn_v1.address = image.entry_point;
  fn_v1.name = "prepared_entry_v1";
  hints_v1.functions.push_back(fn_v1);
  write_text_file(module_dir / "revisions" / revision_hex / "analysis.json", to_json(hints_v1).dump(2));

  const auto cache_root = root / "cache";
  const auto status_file = root / "status.json";
  const std::string prepare_exe = XENON_PREPARE_EXECUTABLE;

  const auto run_prepare = [&](bool query, bool force) {
    std::ostringstream command;
    command << quote(prepare_exe) << " --content " << quote(content_dir) << " --module "
            << quote(module_dir) << " --module-id xenon_test_module --cache-root "
            << quote(cache_root) << " --status-file " << quote(status_file) << " --recomp-root "
            << quote(std::filesystem::path(XENON_SOURCE_ROOT));
    if (query) command << " --query";
    if (force) command << " --force";
    return run_system(command.str());
  };

  // 1. First Play: no prepared artifact exists yet - must build successfully.
  const auto first_result = run_prepare(/*query=*/false, /*force=*/false);
  assert(first_result == 0);
  std::cout << "  [PASS] First preparation with no cached artifact builds successfully\n";

  ArtifactCacheStore store(cache_root);
  const auto key_v1 = expected_key(identity, hints_v1);
  const auto entry_v1 = store.lookup(key_v1);
  assert(entry_v1.status == ArtifactCacheStatus::Fresh);
  assert(std::filesystem::exists(entry_v1.native_extension_path));
  std::error_code ec;
  const auto first_mtime = std::filesystem::last_write_time(entry_v1.native_extension_path, ec);
  assert(!ec);

  // 2. --query reports Fresh with no rebuild needed.
  {
    const auto query_result = run_prepare(/*query=*/true, /*force=*/false);
    assert(query_result == 0);
  }
  std::cout << "  [PASS] Query mode succeeds after a build\n";

  // 3. Second Play: identical content/module - must NOT rebuild (the
  //    committed module's mtime must be unchanged).
  const auto second_result = run_prepare(/*query=*/false, /*force=*/false);
  assert(second_result == 0);
  const auto second_mtime = std::filesystem::last_write_time(entry_v1.native_extension_path, ec);
  assert(!ec);
  assert(second_mtime == first_mtime && "a second Play with nothing changed must not rebuild");
  std::cout << "  [PASS] A second Play with nothing changed does not rebuild\n";

  // 4. The module's analysis hints actually change (simulating a module
  //    update that touches analysis data) - this MUST invalidate the cache
  //    and rebuild, producing a second, independent cache entry, while the
  //    first entry's artifact is left completely alone (Part 16/18).
  AnalysisHintSetV2 hints_v2 = hints_v1;
  hints_v2.functions[0].name = "prepared_entry_v2";
  write_text_file(module_dir / "revisions" / revision_hex / "analysis.json", to_json(hints_v2).dump(2));

  const auto third_result = run_prepare(/*query=*/false, /*force=*/false);
  assert(third_result == 0);

  const auto key_v2 = expected_key(identity, hints_v2);
  assert(key_v2.digest() != key_v1.digest() &&
        "changing analysis hint content must change the artifact cache key");
  const auto entry_v2 = store.lookup(key_v2);
  assert(entry_v2.status == ArtifactCacheStatus::Fresh);
  assert(entry_v2.native_extension_path != entry_v1.native_extension_path);

  // The original entry must still be intact and unaffected.
  const auto still_v1 = store.lookup(key_v1);
  assert(still_v1.status == ArtifactCacheStatus::Fresh);
  const auto v1_mtime_after = std::filesystem::last_write_time(entry_v1.native_extension_path, ec);
  assert(!ec && v1_mtime_after == first_mtime);
  std::cout << "  [PASS] Changed analysis hints trigger a rebuild into a new, independent cache "
              "entry without disturbing the previous one\n";

  // 5. Deterministic cancellation: pre-creating the stop-signal file before
  //    the worker starts guarantees it observes the request at its very
  //    first checkpoint. Use a key that is not yet cached (--force) so the
  //    worker actually attempts to do work rather than short-circuiting on
  //    an existing Fresh entry.
  const auto stop_signal = root / "stop.signal";
  write_text_file(stop_signal, "");
  {
    std::ostringstream command;
    command << quote(prepare_exe) << " --content " << quote(content_dir) << " --module "
            << quote(module_dir) << " --module-id xenon_test_module --cache-root "
            << quote(cache_root) << " --status-file " << quote(status_file) << " --recomp-root "
            << quote(std::filesystem::path(XENON_SOURCE_ROOT)) << " --stop-signal "
            << quote(stop_signal) << " --force";
    const auto result = run_system(command.str());
    assert(result == 7 && "a pre-signalled cancellation must exit with the documented code 7");
  }
  const auto status_text = read_text_file(status_file);
  assert(status_text.find("\"Cancelled\"") != std::string::npos);
  // Cancellation must not disturb either existing valid entry.
  assert(store.lookup(key_v1).status == ArtifactCacheStatus::Fresh);
  assert(store.lookup(key_v2).status == ArtifactCacheStatus::Fresh);
  // And it must not leave a stray staging directory behind.
  std::size_t staging_entries = 0;
  const auto staging_dir = cache_root / "staging";
  if (std::filesystem::exists(staging_dir)) {
    for (const auto& item : std::filesystem::directory_iterator(staging_dir)) {
      (void)item;
      ++staging_entries;
    }
  }
  assert(staging_entries == 0);
  std::cout << "  [PASS] A pre-signalled cancellation exits cleanly and disturbs nothing\n";

  // ---------------------------------------------------------------------
  // Gen 11 (Autonomous Game Intake): a content directory shipping a second
  // executable XEX module alongside the mandatory default.xex must have both
  // independently analyzed, compiled and cached - never colliding on one
  // cache entry even though both fixtures share byte-identical content here -
  // and the outcome of both must be recorded in a deterministic Game
  // Compilation Graph manifest. Uses its own fixture root, entirely separate
  // from every test above.
  // ---------------------------------------------------------------------
  {
    const auto multi_root = std::filesystem::temp_directory_path() / "xenon_prepare_worker_test_multi";
    std::filesystem::remove_all(multi_root);
    std::filesystem::create_directories(multi_root);

    const auto multi_content = multi_root / "content";
    std::filesystem::create_directories(multi_content / "media");
    write_file(multi_content / "default.xex", xex_bytes);
    write_file(multi_content / "media" / "secondary.xex", xex_bytes);

    const auto multi_cache = multi_root / "cache";
    const auto multi_status = multi_root / "status.json";
    const auto run_multi_prepare = [&](bool query) {
      std::ostringstream command;
      command << quote(prepare_exe) << " --content " << quote(multi_content) << " --cache-root "
              << quote(multi_cache) << " --status-file " << quote(multi_status) << " --recomp-root "
              << quote(std::filesystem::path(XENON_SOURCE_ROOT));
      if (query) command << " --query";
      return run_system(command.str());
    };

    const auto read_graph = [&] {
      JsonValue graph;
      std::string parse_error;
      const auto text = read_text_file(multi_cache / "game-compilation-graph.json");
      assert(JsonValue::parse(text, graph, &parse_error) && parse_error.empty());
      return graph;
    };

    const auto find_module = [](const JsonValue& graph, const std::string& relative_path) -> JsonValue {
      const auto* modules_value = graph.find("modules");
      assert(modules_value != nullptr);
      const auto* modules = modules_value->as_array();
      assert(modules != nullptr);
      for (const auto& module : *modules) {
        if (module.get_string("relativePath") == relative_path) return module;
      }
      assert(false && "expected module missing from game-compilation-graph.json");
      return {};
    };

    assert(run_multi_prepare(/*query=*/false) == 0);
    {
      const auto graph = read_graph();
      assert(graph.get_number("schemaVersion") == 1.0);
      const auto* modules_value = graph.find("modules");
      assert(modules_value != nullptr);
      const auto* modules = modules_value->as_array();
      assert(modules != nullptr && modules->size() == 2);

      const auto primary = find_module(graph, "default.xex");
      assert(primary.get_bool("isPrimary"));
      assert(primary.get_string("status") == "Prepared");
      const auto primary_path = primary.get_string("nativeExtensionPath");
      assert(!primary_path.empty());
      assert(std::filesystem::exists(primary_path));

      const auto secondary = find_module(graph, "media/secondary.xex");
      assert(!secondary.get_bool("isPrimary"));
      assert(secondary.get_string("status") == "Prepared");
      const auto secondary_path = secondary.get_string("nativeExtensionPath");
      assert(!secondary_path.empty());
      assert(std::filesystem::exists(secondary_path));

      // Two distinct entries, never one module silently reusing the other's
      // compiled artifact.
      assert(primary_path != secondary_path);
    }
    std::cout << "  [PASS] A secondary discovered XEX module is independently analyzed, compiled and cached\n";

    const auto first_primary_mtime =
        std::filesystem::last_write_time(find_module(read_graph(), "default.xex").get_string("nativeExtensionPath"));
    const auto first_secondary_mtime = std::filesystem::last_write_time(
        find_module(read_graph(), "media/secondary.xex").get_string("nativeExtensionPath"));

    assert(run_multi_prepare(/*query=*/false) == 0);
    {
      const auto graph = read_graph();
      assert(find_module(graph, "default.xex").get_string("status") == "Fresh");
      assert(find_module(graph, "media/secondary.xex").get_string("status") == "Fresh");
      assert(std::filesystem::last_write_time(
                 find_module(graph, "default.xex").get_string("nativeExtensionPath")) == first_primary_mtime);
      assert(std::filesystem::last_write_time(find_module(graph, "media/secondary.xex")
                                              .get_string("nativeExtensionPath")) == first_secondary_mtime);
    }
    std::cout << "  [PASS] A second Play with both modules already fresh rebuilds neither\n";

    assert(run_multi_prepare(/*query=*/true) == 0);
    std::cout << "  [PASS] Query mode succeeds for a multi-module content source\n";

    std::filesystem::remove_all(multi_root);
  }

  std::filesystem::remove_all(root);
  std::cout << "All xenon-prepare worker tests passed!\n";
  return 0;
}
