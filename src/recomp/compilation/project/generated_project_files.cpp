#include <fstream>
#include <optional>
#include <string>

#include "recomp/compilation/project_generation_internal.hpp"

namespace xenon::recomp::detail {

bool write_import_manifest(const std::filesystem::path& generation_stage, const AnalysisReport& report) {
  std::ofstream imports(generation_stage / "imports.cpp");
  imports << "// Generated import manifest.\n#include <cstddef>\n#include <cstdint>\nnamespace xenon::recomp {\n"
             "struct GeneratedImport { const char* module; const char* symbol; std::uint16_t ordinal; std::uint32_t thunk; };\n"
             "const GeneratedImport kGeneratedImports["
             + std::to_string(std::max<std::size_t>(1u, report.image.imports.size()))
             + "] = {\n";
  // A real, previously-undetected bug lived here: std::hex applied to
  // `imports` for one entry's guest_thunk is STICKY on the underlying
  // ostream - it silently also applied to the NEXT entry's `item.ordinal`
  // (printed before any hex/dec marker of its own), corrupting that
  // decimal field. Most ordinal values happened to still be all-digit
  // hex representations (e.g. 0x190 in hex prints as "190", a numerically
  // different but still syntactically valid decimal-looking literal), so
  // this silently produced a WRONG (never validated by any test - nothing
  // reads this generated table back) ordinal for every import after the
  // first. An ordinal whose hex digits include a letter (e.g. NtCreateFile,
  // 0x00D2 = 210, prints as "d2" in leftover hex mode) turns this from a
  // silently wrong value into an outright compile error - which is how this
  // pass found it. Fixed by explicitly forcing std::dec immediately before
  // the decimal field and std::dec again after the hex one, so no iteration
  // can inherit stream state from a previous one.
  for (const auto& item : report.image.imports)
    imports << "  {\"" << item.module << "\", \"" << item.symbol << "\", " << std::dec << item.ordinal
            << ", 0x" << std::hex << item.guest_thunk << std::dec << "},\n";
  imports << "};\nconst std::size_t kGeneratedImportCount = "
             "sizeof(kGeneratedImports) / sizeof(kGeneratedImports[0]);\n}\n";
  imports.close();
  return static_cast<bool>(imports);
}

bool write_generated_metadata(const std::filesystem::path& generation_stage, const AnalysisReport& report) {
  std::optional<std::uint32_t> setjmp_address, longjmp_address;
  if (report.hint_set_v2) {
    for (const auto& helper : report.hint_set_v2->runtime_helpers) {
      if (helper.kind == analysis::RuntimeHelperKind::SetJmp) setjmp_address = helper.address;
      else if (helper.kind == analysis::RuntimeHelperKind::LongJmp) longjmp_address = helper.address;
    }
  }
  std::ofstream metadata(generation_stage / "metadata.cpp");
  metadata << "// Generated analysis metadata.\n#include \"registry.hpp\"\nnamespace xenon::recomp {\n";
  metadata << "const bool kHasSetJmpAddress = " << (setjmp_address ? "true" : "false") << ";\n";
  metadata << "const std::uint32_t kSetJmpAddress = 0x" << std::hex << setjmp_address.value_or(0) << std::dec << ";\n";
  metadata << "const bool kHasLongJmpAddress = " << (longjmp_address ? "true" : "false") << ";\n";
  metadata << "const std::uint32_t kLongJmpAddress = 0x" << std::hex << longjmp_address.value_or(0) << std::dec << ";\n";
  metadata << "}\n";
  metadata << "// entry=0x" << std::hex << report.image.entry_point << " functions=" << std::dec << report.functions.size()
           << " unresolved=" << report.unresolved.size() << " config_hash=0x" << std::hex << report.configuration_hash << "\n";
  metadata.close();
  return static_cast<bool>(metadata);
}

bool write_generated_hooks(const std::filesystem::path& generation_stage, const DriverOptions& options,
                           const AnalysisReport& report) {
  std::ofstream hooks(generation_stage / "hooks.cpp");
  hooks << "// Generated module hooks and patch manifest. Runtime integration owns semantics.\n";
  for (const auto& hint : options.hints) {
    for (const auto& hook : hint.special_hooks)
      hooks << "// hook[" << hint.name << "] " << hook << "\n";
    for (const auto& patch : hint.patches)
      hooks << "// patch[" << hint.name << "] " << patch << "\n";
  }
  if (report.hint_set_v2) {
    for (const auto& hook : report.hint_set_v2->hooks)
      hooks << "// hook-v2[0x" << std::hex << hook.address << std::dec << "] "
            << hook.native_replacement_identity << " " << hook.description << "\n";
    for (const auto& patch : report.hint_set_v2->patches)
      hooks << "// patch-v2[0x" << std::hex << patch.address << std::dec << "] bytes="
            << patch.patch_bytes_hex << " " << patch.description << "\n";
  }
  hooks.close();
  return static_cast<bool>(hooks);
}

bool write_analysis_json(const std::filesystem::path& generation_stage, const AnalysisReport& report) {
  std::ofstream json(generation_stage / "analysis.json");
  json << format_report_json(report);
  json.close();
  return static_cast<bool>(json);
}

bool write_shard_manifest(const std::filesystem::path& generation_stage, const AnalysisReport& report,
                          const std::vector<std::filesystem::path>& shards) {
  std::ofstream manifest(generation_stage / "manifest.txt");
  manifest << "configuration_hash=0x" << std::hex << report.configuration_hash << "\n";
  for (const auto& path : shards) manifest << path.filename().string() << "\n";
  manifest.close();
  return static_cast<bool>(manifest);
}

bool write_module_export(const std::filesystem::path& generation_stage, const AnalysisReport& report) {
  // The canonical Xenon module ABI (docs/runtime/RUNTIME_HOST.md): a single dynamic
  // library exporting Xenon_BindCompiledRegistry(ExecutionContext&), which
  // XenonSession::load_native_extension() resolves via LoadLibrary/dlopen.
  // xenon_game (STATIC, above) stays the reusable embedding/development
  // artifact; this wraps it in a SHARED module without recompiling any
  // generated game code, so a real title's build can produce both from the
  // same generated project.
  // The effective-image identity of exactly the executable this module was
  // analyzed/compiled from (report.image - whatever bytes options.input
  // pointed at, base or already-title-update-patched by upstream tooling).
  // Emitted below as Xenon_SupportedExecutableRevisions() so
  // XenonSession::load_native_extension() can refuse to run this module's
  // compiled code against a different effective executable revision (see
  // docs/runtime/RUNTIME_HOST.md "Effective executable identity" / section 8 of
  // docs/runtime/RUNTIME_SESSION.md's title-update integration).
  const auto module_revision_hex =
      xbox::format_effective_image_hash(xbox::compute_effective_image_hash(report.image));

  std::ofstream module_export(generation_stage / "module_export.cpp");
  module_export << "// Generated Xenon game module export shim - wraps the\n"
                   "// generated compiled-code registry (registry.cpp) in the\n"
                   "// canonical Xenon_BindCompiledRegistry ABI a loadable game\n"
                   "// module must export. See docs/runtime/RUNTIME_HOST.md.\n"
                   "#include \"registry.hpp\"\n"
                   "#if defined(_WIN32)\n"
                   "#define XENON_GAME_MODULE_EXPORT extern \"C\" __declspec(dllexport)\n"
                   "#else\n"
                   "#define XENON_GAME_MODULE_EXPORT extern \"C\" __attribute__((visibility(\"default\")))\n"
                   "#endif\n"
                   "XENON_GAME_MODULE_EXPORT void Xenon_BindCompiledRegistry(xenon::cpu::ExecutionContext& context) {\n"
                   "  xenon::recomp::bind_compiled_registry(context);\n"
                   "}\n"
                   "// Declares the exact effective-XEX revision this module's compiled\n"
                   "// registry was generated from - see XenonSession::load_native_extension().\n"
                   "XENON_GAME_MODULE_EXPORT const char* Xenon_SupportedExecutableRevisions() {\n"
                   "  return \"" + module_revision_hex + "\";\n"
                   "}\n";
  module_export.close();
  return static_cast<bool>(module_export);
}

bool write_generated_cmake(const std::filesystem::path& generation_stage, const DriverOptions& options,
                           const std::vector<std::filesystem::path>& shards) {
  std::ofstream build(generation_stage / "CMakeLists.txt");
  build << "cmake_minimum_required(VERSION 3.25)\n"
           "project(xenon_game_generated LANGUAGES CXX)\n"
           "set(CMAKE_CXX_STANDARD 20)\n"
           "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
           "set(XENON_BUILD_TESTS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_BUILD_BENCHMARKS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_BUILD_LAUNCHER OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_GRAPHICS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_AUDIO OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_INPUT OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_DEPENDENCY_MODE SYSTEM CACHE STRING \"\" FORCE)\n"
           "set(XENON_AUTO_BOOTSTRAP_DEPS OFF CACHE BOOL \"\" FORCE)\n"
           "if(MSVC)\n"
           "  set(CMAKE_CXX_FLAGS \"${CMAKE_CXX_FLAGS} /constexpr:steps2147483647\")\n"
           "endif()\n"
           "set(XENON_RECOMP_ROOT \"\" CACHE PATH \"Path to the Xenon-Recomp source tree\")\n"
           "if(NOT XENON_RECOMP_ROOT)\n"
           "  message(FATAL_ERROR \"Set XENON_RECOMP_ROOT to the Xenon-Recomp source tree\")\n"
           "endif()\n"
           "add_subdirectory(${XENON_RECOMP_ROOT} xenon-recomp EXCLUDE_FROM_ALL)\n"
           "add_library(xenon_game STATIC\n";
  for (const auto& path : shards)
    build << "  " << std::filesystem::relative(path, options.output).generic_string() << "\n";
  build << "  registry.cpp\n  imports.cpp\n  metadata.cpp\n  hooks.cpp\n"
           ")\n"
           // Xenon::Recomp: native_replacements.hpp/.cpp - referenced by
           // registry.cpp's lookup_compiled() switch whenever this module
           // used at least one NativeReplacement hint (Part 1.10). Always
           // linked (small, already a dependency of the driver itself) so a
           // module gaining its first native replacement never needs a
           // build-system change.
           "target_link_libraries(xenon_game PRIVATE Xenon::CPU Xenon::Memory Xenon::XboxKernelIo Xenon::Recomp)\n"
           "target_include_directories(xenon_game PRIVATE ${XENON_RECOMP_ROOT}/include)\n"
           "set_property(TARGET xenon_game PROPERTY POSITION_INDEPENDENT_CODE ON)\n"
           "add_library(xenon_game_module SHARED module_export.cpp)\n"
           "target_link_libraries(xenon_game_module PRIVATE xenon_game)\n"
           "target_include_directories(xenon_game_module PRIVATE ${XENON_RECOMP_ROOT}/include)\n";
  build << "find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)\n"
           "set(XENON_GRAPH_CACHE \"${CMAKE_CURRENT_SOURCE_DIR}/.graph/native\" CACHE PATH \"Gen 10 native object cache\")\n"
           "if(MSVC)\n  target_compile_options(xenon_game PRIVATE /Brepro)\n  target_link_options(xenon_game_module PRIVATE /Brepro /INCREMENTAL:NO)\nendif()\n"
           "set_property(TARGET xenon_game PROPERTY CXX_COMPILER_LAUNCHER \"${Python3_EXECUTABLE};${XENON_RECOMP_ROOT}/tools/compilation_cache.py;compile;${XENON_GRAPH_CACHE}\")\n";
  build.close();
  return static_cast<bool>(build);
}

}  // namespace xenon::recomp::detail
