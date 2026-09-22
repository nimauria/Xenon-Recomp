#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"
#include "xenon/recomp/module_hint_provider.hpp"
#include "xenon/cpu/decoder.hpp"

#if defined(XENON_TOOL_HAS_CORE)
#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#endif

int main(int argc, char** argv) {
  const std::string tool = XENON_TOOL_NAME;
  if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
    std::cout << tool << " - Xenon recompilation utility\n";
    if (tool == "recomp-driver")
      std::cout << "usage: recomp-driver <game.xex> [output-directory] [--hints file] "
                   "[--module <module-directory>] [--jobs auto|N] [--quiet] [--json]\n"
                   "  --module points at an installed Project Gracemeria module package\n"
                   "  (manifest.json + revisions/<effective-image-hash>/analysis.json - see\n"
                   "  docs/recomp/ANALYSIS_HINT_SCHEMA_V2.md). The Recomp Driver selects and validates\n"
                   "  the hint set for the effective executable revision being analyzed and\n"
                   "  fails clearly if the module has no data for it - never a silent fallback.\n"
                   "  --jobs controls both analysis and codegen worker counts (Part 3 of the\n"
                   "  Recomp Analysis V2 pass): 'auto' (default) is roughly hardware_concurrency\n"
                   "  minus one, '1' forces deterministic single-thread execution, or supply an\n"
                   "  explicit worker count. --quiet suppresses progress milestones on stderr.\n";
    else if (tool == "ppc-disasm") std::cout << "usage: ppc-disasm <game.xex> [--json]\n";
    else if (tool == "ir-dump") std::cout << "usage: ir-dump <game.xex> [--json]\n";
    else if (tool == "import-scanner") std::cout << "usage: import-scanner <game.xex> [--json]\n"
                                                     "  Prints, for every guest import: module!symbol, ordinal, the\n"
                                                     "  resolved Xenon export name (if any), and whether it is registered\n"
                                                     "  in the production core::ExportRegistry (same registry\n"
                                                     "  XenonSession uses) - not a second, hand-maintained ordinal list.\n"
                                                     "  Used by Project Gracemeria to audit a title's XAM imports.\n";
    else std::cout << "usage: module-inspector <game.xex> [--json]\n";
    return argc < 2 ? 1 : 0;
  }
  xenon::recomp::DriverOptions options;
  options.input = argv[1];
  std::unique_ptr<xenon::recomp::FileModuleHintProvider> module_provider;
  bool quiet = false;
  for (int index = 2; index < argc; ++index) {
    if (std::string(argv[index]) == "--json") {
      continue;
    } else if (std::string(argv[index]) == "--quiet") {
      quiet = true;
    } else if (std::string(argv[index]) == "--hints" && index + 1 < argc) {
      options.hints_file = argv[++index];
    } else if (std::string(argv[index]) == "--jobs" && index + 1 < argc) {
      // Part 3/16: shared --jobs flag controls both analysis and codegen
      // worker counts. "auto" leaves both at their DriverOptions default
      // (nullopt -> resolve_worker_count()'s hardware-derived policy);
      // any other value must be a positive integer (1 == deterministic
      // single-thread mode).
      const std::string value = argv[++index];
      if (value != "auto") {
        try {
          const auto jobs = static_cast<std::size_t>(std::stoul(value));
          if (jobs == 0) throw std::invalid_argument("zero");
          options.analysis_jobs = jobs;
          options.codegen_jobs = jobs;
        } catch (const std::exception&) {
          std::cerr << tool << ": --jobs must be 'auto' or a positive integer, got '" << value << "'\n";
          return 1;
        }
      }
    } else if (std::string(argv[index]) == "--module" && index + 1 < argc) {
      // Production ModuleHintProviderV2 construction (Part 2.3/2.4): the CLI
      // is the "developer tooling" call site that actually builds a real
      // provider and hands it to the Recomp Driver, rather than the
      // provider only ever existing as an unused class.
      module_provider = std::make_unique<xenon::recomp::FileModuleHintProvider>(argv[++index]);
      if (!module_provider->manifest_loaded()) {
        std::cerr << tool << ": --module " << argv[index] << ": " << module_provider->manifest_error() << "\n";
        return 1;
      }
      options.hint_provider_v2 = module_provider.get();
    } else if (tool == "recomp-driver" && index == 2) {
      options.output = argv[index];
    } else {
      std::cerr << tool << ": unknown argument: " << argv[index] << "\n";
      return 1;
    }
  }
  // Progress milestones (Part 18): printed to stderr so they never pollute
  // --json/plain report output on stdout. A single reporting point (the
  // driver only ever calls this from whichever thread is driving the
  // current phase, never concurrently - see DriverOptions::progress) means
  // no locking is needed here either.
  if (!quiet) options.progress = [](const std::string& message) { std::cerr << message << "\n"; };
  xenon::recomp::AnalysisReport report;
  std::string error;
  if (!xenon::recomp::load_and_analyze(options, report, error)) {
    std::cerr << tool << ": " << error << "\n";
    return 2;
  }
  bool json = false;
  for (int index = 2; index < argc; ++index)
    if (std::string(argv[index]) == "--json") json = true;
  if (tool == "recomp-driver") {
    if (!xenon::recomp::generate_project(options, report, error)) {
      std::cerr << tool << ": " << error << "\n";
      return 3;
    }
    std::cout << (json ? xenon::recomp::format_report_json(report)
                       : xenon::recomp::format_report(report));
  } else if (tool == "ir-dump") {
    for (const auto& function : report.functions) std::cout << xenon::recomp::format_ir(function);
  } else if (tool == "ppc-disasm") {
    xenon::cpu::Decoder decoder;
    for (const auto& section : report.image.sections) {
      if (!section.executable) continue;
      const auto count = section.bytes.size() / 4u;
      for (std::size_t index = 0; index < count; ++index) {
        const auto word = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4])) << 24u) |
                          (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4 + 1])) << 16u) |
                          (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4 + 2])) << 8u) |
                          static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4 + 3]));
        const auto instruction = decoder.decode(section.virtual_address + static_cast<std::uint32_t>(index * 4u), word);
        std::cout << "0x" << std::hex << instruction.address << ": 0x" << word << " "
                  << (instruction.valid() ? instruction.mnemonic() : "invalid") << "\n";
      }
    }
  } else if (tool == "import-scanner") {
#if defined(XENON_TOOL_HAS_CORE)
    // Cross-reference every import against a real, minimally-initialized
    // XenonSession's export_registry_ - the exact same registry
    // XamSession::register_exports() (and every other subsystem's exports)
    // populate in production - rather than a second, hand-maintained
    // ordinal table that could silently drift from what actually resolves
    // at runtime. Game-agnostic: works for any XEX passed on the command
    // line.
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = false;
    config.enable_audio = false;
    const bool session_ready = session.initialize(config).success;
    if (!session_ready) {
      std::cerr << tool
                << ": warning: could not initialize a XenonSession to resolve imports "
                   "against the production export registry; falling back to raw import "
                   "listing\n";
    }

    // Part 6.1 of the Gracemeria readiness pass: group output by library so
    // a commercial-title import list (potentially hundreds of entries) is
    // actually reviewable. Audio exports are registered under the literal
    // "xboxkrnl" library string (see src/audio/exports.cpp) rather than a
    // distinct import module, so they are broken out as an "xboxkrnl
    // (audio)" sub-group by resolved name prefix instead of by (nonexistent)
    // separate module string.
    std::map<std::string, std::vector<const xenon::xbox::XexImport*>> groups;
    for (const auto& item : report.image.imports) {
      std::string group = item.module;
      if (session_ready && item.module == "xboxkrnl") {
        if (const auto* descriptor = session.exports()->resolve(item.module, item.ordinal);
            descriptor != nullptr && descriptor->name.rfind("XAudio", 0) == 0) {
          group = "xboxkrnl (audio)";
        }
      }
      groups[group].push_back(&item);
    }
    for (auto& [group, items] : groups) {
      std::cout << "== " << group << " (" << items.size() << ") ==\n";
      for (const auto* item_ptr : items) {
        const auto& item = *item_ptr;
        std::cout << "  " << item.module << "!" << item.symbol << " ordinal=" << item.ordinal
                  << " thunk=0x" << std::hex << item.guest_thunk << std::dec;
        if (session_ready) {
          const auto* descriptor = session.exports()->resolve(item.module, item.ordinal);
          if (descriptor != nullptr) {
            const char* status = "Optional";
            switch (descriptor->requirement) {
              case xenon::core::ExportRequirement::Required: status = "Required"; break;
              case xenon::core::ExportRequirement::Stubbed: status = "Stubbed"; break;
              case xenon::core::ExportRequirement::Optional: status = "Optional"; break;
              case xenon::core::ExportRequirement::DiagnosticOnly: status = "DiagnosticOnly"; break;
            }
            std::cout << " resolved=" << descriptor->name << " status=" << status;
          } else {
            std::cout << " resolved=<none> status=NOT_REGISTERED";
          }
        }
        std::cout << "\n";
      }
    }
    if (session_ready) session.shutdown();
#else
    for (const auto& item : report.image.imports)
      std::cout << item.module << "!" << item.symbol << " ordinal=" << item.ordinal << " thunk=0x" << std::hex << item.guest_thunk << "\n";
#endif
  } else if (tool == "module-inspector") {
    std::cout << "format=" << static_cast<unsigned>(report.image.format) << " entry=0x" << std::hex << report.image.entry_point
              << " sections=" << std::dec << report.image.sections.size() << " imports=" << report.image.imports.size()
              << " exports=" << report.image.exports.size()
              << " function_metadata=" << report.image.function_metadata.size() << "\n";
  } else {
    std::cout << xenon::recomp::format_report(report);
  }
  return report.unresolved.empty() ? 0 : 4;
}
