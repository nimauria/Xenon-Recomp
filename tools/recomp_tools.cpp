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
#include "xenon/core/import_classification.hpp"
#include "xenon/core/json.hpp"
#include "xenon/core/session.hpp"
#endif

int main(int argc, char** argv) {
  const std::string tool = XENON_TOOL_NAME;
  if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
    std::cout << tool << " - Xenon recompilation utility\n";
    if (tool == "recomp-driver")
      std::cout << "usage: recomp-driver <game.xex> [output-directory] [--hints file] "
                   "[--module <module-directory>] [--observations <adaptive-observations.jsonl>] "
                   "[--graph-cache <directory>] [--knowledge <knowledge.jsonl>] [--knowledge-export <knowledge.jsonl>] "
                   "[--knowledge-min-score N] [--no-knowledge-seed] "
                   "[--jobs auto|N] [--quiet] [--json]\n"
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
                                                     "  Whole-XEX import capability audit (Phase 6 of the AC6 Runtime\n"
                                                     "  Readiness pass). Classifies every guest import against the real\n"
                                                     "  production core::ExportRegistry (the same registry XenonSession\n"
                                                     "  uses, not a second hand-maintained ordinal list) as one of:\n"
                                                     "    IMPLEMENTED - registered, real behavior.\n"
                                                     "    SAFE_STUB   - registered, a deliberate safe no-op.\n"
                                                     "    PARTIAL     - registered but has a real, documented behavior gap.\n"
                                                     "    MISSING     - not registered at all.\n"
                                                     "  Prints a per-library summary and an overall PASS / PASS_WITH_FALLBACK\n"
                                                     "  / FAIL verdict: FAIL if any import is MISSING, PASS_WITH_FALLBACK if\n"
                                                     "  every import resolves but at least one is SAFE_STUB/PARTIAL, PASS\n"
                                                     "  only when every import is a complete IMPLEMENTED. Exits nonzero only\n"
                                                     "  on FAIL. --json emits the same data as JSON. Used by Project\n"
                                                     "  Gracemeria to audit a title's imports before the expensive native\n"
                                                     "  module build.\n";
    else std::cout << "usage: module-inspector <game.xex> [--json]\n";
    return argc < 2 ? 1 : 0;
  }
  xenon::recomp::DriverOptions options;
  options.input = argv[1];
  std::unique_ptr<xenon::recomp::FileModuleHintProvider> module_provider;
  std::filesystem::path knowledge_export_path;
  bool quiet = false;
  for (int index = 2; index < argc; ++index) {
    if (std::string(argv[index]) == "--json") {
      continue;
    } else if (std::string(argv[index]) == "--quiet") {
      quiet = true;
    } else if (std::string(argv[index]) == "--hints" && index + 1 < argc) {
      options.hints_file = argv[++index];
    } else if (std::string(argv[index]) == "--observations" && index + 1 < argc) {
      std::string observation_error;
      if (!xenon::recomp::load_adaptive_observations(
              argv[++index], options.adaptive_observations, observation_error)) {
        std::cerr << tool << ": --observations: " << observation_error << "\n";
        return 1;
      }
    } else if (std::string(argv[index]) == "--graph-cache" && index + 1 < argc) {
      options.graph_cache = argv[++index];
    } else if (std::string(argv[index]) == "--knowledge" && index + 1 < argc) {
      std::string knowledge_error;
      if (!xenon::recomp::load_knowledge_base(
              argv[++index], options.knowledge_records, knowledge_error)) {
        std::cerr << tool << ": --knowledge: " << knowledge_error << "\n";
        return 1;
      }
    } else if (std::string(argv[index]) == "--knowledge-export" && index + 1 < argc) {
      knowledge_export_path = argv[++index];
    } else if (std::string(argv[index]) == "--knowledge-min-score" && index + 1 < argc) {
      try {
        const auto score = static_cast<std::uint32_t>(std::stoul(argv[++index]));
        if (score > 100u) throw std::out_of_range("score");
        options.knowledge_match_min_score = score;
      } catch (const std::exception&) {
        std::cerr << tool << ": --knowledge-min-score must be an integer from 0 to 100\n";
        return 1;
      }
    } else if (std::string(argv[index]) == "--no-knowledge-seed") {
      options.enable_knowledge_seeding = false;
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
  if (!knowledge_export_path.empty()) {
    const auto image_hash = xenon::xbox::format_effective_image_hash(
        xenon::xbox::compute_effective_image_hash(report.image));
    const auto learned = xenon::recomp::export_analysis_knowledge(report, image_hash);
    auto accumulated = options.knowledge_records;
    accumulated.insert(accumulated.end(), learned.begin(), learned.end());
    std::string knowledge_error;
    if (!xenon::recomp::save_knowledge_base(knowledge_export_path, accumulated, knowledge_error)) {
      std::cerr << tool << ": --knowledge-export: " << knowledge_error << "\n";
      return 2;
    }
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

    // Phase 6 of the AC6 Runtime Readiness pass: four-state classification
    // (not the previous binary resolved/unresolved) via
    // xenon::core::classify_import() - a small, independently-tested
    // function (tests/core/import_capability_report_tests.cpp), not logic
    // embedded only here:
    //   IMPLEMENTED - registered, not a stub, not flagged partial.
    //   SAFE_STUB   - registered as ExportRequirement::Stubbed and not
    //                 flagged partial (a deliberate, safe no-op).
    //   PARTIAL     - registered but ExportDescriptor::partial is set (a
    //                 real, non-obvious behavior gap - see partial_note).
    //   MISSING     - not registered at all (including when the registry
    //                 itself could not be initialized - never silently
    //                 accepted as anything better than MISSING).
    using xenon::core::ImportClassification;

    struct ClassifiedImport {
      const xenon::xbox::XexImport* item;
      ImportClassification status;
      std::string resolved_name;
      std::string partial_note;
    };

    // Part 6.1 of the Gracemeria readiness pass: group output by library so
    // a commercial-title import list (potentially hundreds of entries) is
    // actually reviewable. Audio exports are registered under the literal
    // "xboxkrnl" library string (see src/audio/exports.cpp) rather than a
    // distinct import module, so they are broken out as an "xboxkrnl
    // (audio)" sub-group by resolved name prefix instead of by (nonexistent)
    // separate module string.
    std::map<std::string, std::vector<ClassifiedImport>> groups;
    for (const auto& item : report.image.imports) {
      const auto* descriptor =
          session_ready ? session.exports()->resolve(item.module, item.ordinal) : nullptr;

      std::string group = item.module;
      if (descriptor != nullptr && item.module == "xboxkrnl" &&
          descriptor->name.rfind("XAudio", 0) == 0) {
        group = "xboxkrnl (audio)";
      }
      const auto status = xenon::core::classify_import(descriptor);
      const std::string resolved_name = descriptor != nullptr ? descriptor->name : "<none>";
      const std::string partial_note = descriptor != nullptr ? descriptor->partial_note : "";
      groups[group].push_back(ClassifiedImport{&item, status, resolved_name, partial_note});
    }

    std::size_t total_implemented = 0, total_safe_stub = 0, total_partial = 0, total_missing = 0;
    for (const auto& [group, items] : groups) {
      for (const auto& classified : items) {
        switch (classified.status) {
          case ImportClassification::Implemented: ++total_implemented; break;
          case ImportClassification::SafeStub: ++total_safe_stub; break;
          case ImportClassification::Partial: ++total_partial; break;
          case ImportClassification::Missing: ++total_missing; break;
        }
      }
    }
    // Reviewer feedback on the AC6 Runtime Readiness pass's capability
    // report: a binary PASS/FAIL hides the difference between "every
    // required import resolves with zero known gaps" and "every required
    // import resolves, but some rely on a safe stub or a documented partial
    // behavior gap" - the latter may still boot, just not gap-free. See
    // xenon::core::compute_import_capability_verdict().
    const auto overall_verdict = xenon::core::compute_import_capability_verdict(
        total_implemented, total_safe_stub, total_partial, total_missing);
    const bool overall_pass = overall_verdict != xenon::core::ImportCapabilityVerdict::Fail;

    if (json) {
      xenon::core::JsonValue root = xenon::core::JsonValue::make_object();
      xenon::core::JsonValue libraries = xenon::core::JsonValue::make_object();
      for (const auto& [group, items] : groups) {
        std::size_t implemented = 0, safe_stub = 0, partial = 0, missing = 0;
        xenon::core::JsonValue import_list = xenon::core::JsonValue::make_array();
        for (const auto& classified : items) {
          switch (classified.status) {
            case ImportClassification::Implemented: ++implemented; break;
            case ImportClassification::SafeStub: ++safe_stub; break;
            case ImportClassification::Partial: ++partial; break;
            case ImportClassification::Missing: ++missing; break;
          }
          xenon::core::JsonValue entry = xenon::core::JsonValue::make_object();
          entry.set("module", classified.item->module);
          entry.set("symbol", classified.item->symbol);
          entry.set("ordinal", classified.item->ordinal);
          entry.set("resolvedName", classified.resolved_name);
          entry.set("status", std::string(xenon::core::to_string(classified.status)));
          if (!classified.partial_note.empty()) {
            entry.set("partialNote", classified.partial_note);
          }
          import_list.append(std::move(entry));
        }
        xenon::core::JsonValue library_entry = xenon::core::JsonValue::make_object();
        library_entry.set("implemented", static_cast<std::int64_t>(implemented));
        library_entry.set("safeStub", static_cast<std::int64_t>(safe_stub));
        library_entry.set("partial", static_cast<std::int64_t>(partial));
        library_entry.set("missing", static_cast<std::int64_t>(missing));
        library_entry.set("imports", std::move(import_list));
        libraries.set(group, std::move(library_entry));
      }
      root.set("libraries", std::move(libraries));
      xenon::core::JsonValue overall = xenon::core::JsonValue::make_object();
      overall.set("implemented", static_cast<std::int64_t>(total_implemented));
      overall.set("safeStub", static_cast<std::int64_t>(total_safe_stub));
      overall.set("partial", static_cast<std::int64_t>(total_partial));
      overall.set("missing", static_cast<std::int64_t>(total_missing));
      overall.set("result", std::string(xenon::core::to_string(overall_verdict)));
      root.set("overall", std::move(overall));
      std::cout << root.dump(2) << "\n";
    } else {
      std::cout << "XEX import capability report\n\n";
      for (const auto& [group, items] : groups) {
        std::size_t implemented = 0, safe_stub = 0, partial = 0, missing = 0;
        for (const auto& classified : items) {
          switch (classified.status) {
            case ImportClassification::Implemented: ++implemented; break;
            case ImportClassification::SafeStub: ++safe_stub; break;
            case ImportClassification::Partial: ++partial; break;
            case ImportClassification::Missing: ++missing; break;
          }
        }
        std::cout << group << ":\n"
                  << "  implemented: " << implemented << "\n"
                  << "  safe_stub: " << safe_stub << "\n"
                  << "  partial: " << partial << "\n"
                  << "  missing: " << missing << "\n";
        for (const auto& classified : items) {
          const auto& item = *classified.item;
          std::cout << "  " << item.module << "!" << item.symbol << " ordinal=" << item.ordinal
                    << " thunk=0x" << std::hex << item.guest_thunk << std::dec
                    << " resolved=" << classified.resolved_name
                    << " status=" << xenon::core::to_string(classified.status);
          if (!classified.partial_note.empty()) {
            std::cout << " note=\"" << classified.partial_note << "\"";
          }
          std::cout << "\n";
        }
        std::cout << "\n";
      }
      std::cout << "overall:\n"
                << "  implemented: " << total_implemented << "\n"
                << "  safe_stub: " << total_safe_stub << "\n"
                << "  partial: " << total_partial << "\n"
                << "  missing: " << total_missing << "\n"
                << "  result: " << xenon::core::to_string(overall_verdict) << "\n";
    }

    if (session_ready) session.shutdown();
    if (!overall_pass) return 4;
#else
    for (const auto& item : report.image.imports)
      std::cout << item.module << "!" << item.symbol << " ordinal=" << item.ordinal << " thunk=0x" << std::hex << item.guest_thunk << "\n";
#endif
  } else if (tool == "module-inspector") {
    // ENTRY BYTE LAYOUT DIAGNOSTIC
    const auto read_be32_at =
        [](const std::vector<std::byte>& bytes,
           std::size_t offset) -> std::uint32_t {
      if (offset + 4u > bytes.size()) return 0u;

      return
          (static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(bytes[offset])) << 24u) |
          (static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(bytes[offset + 1u])) << 16u) |
          (static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(bytes[offset + 2u])) << 8u) |
          static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(bytes[offset + 3u]));
    };

    {
      const auto entry = report.image.entry_point;
      const auto base = report.image.image_base;

      std::cout << "\n=== ENTRY BYTE LAYOUT DIAGNOSTIC ===\n";
      std::cout << std::hex;
      std::cout << "image_base=0x" << base << "\n";
      std::cout << "entry=0x" << entry << "\n";

      if (entry >= base) {
        const auto entry_rva =
            static_cast<std::size_t>(entry - base);

        std::cout << "entry_rva=0x" << entry_rva << "\n";

        if (entry_rva + 4u <=
            report.image.effective_image.size()) {
          std::cout
              << "loaded_image_word=0x"
              << read_be32_at(
                     report.image.effective_image,
                     entry_rva)
              << "\n";
        } else {
          std::cout
              << "loaded_image_word=<out-of-range>\n";
        }
      }

      for (const auto& section : report.image.sections) {
        const auto section_begin =
            static_cast<std::uint64_t>(
                section.virtual_address);

        const auto section_size =
            std::max<std::uint64_t>(
                section.virtual_size,
                section.raw_size);

        const auto section_end =
            section_begin + section_size;

        if (static_cast<std::uint64_t>(entry) <
                section_begin ||
            static_cast<std::uint64_t>(entry) >=
                section_end) {
          continue;
        }

        const auto delta =
            static_cast<std::size_t>(
                entry -
                static_cast<std::uint32_t>(
                    section.virtual_address));

        std::cout << "\nentry_section="
                  << section.name << "\n";

        std::cout
            << "section_va=0x"
            << static_cast<std::uint32_t>(
                   section.virtual_address)
            << "\n";

        std::cout
            << "virtual_size=0x"
            << section.virtual_size << "\n";

        std::cout
            << "raw_pointer=0x"
            << section.raw_pointer << "\n";

        std::cout
            << "raw_size=0x"
            << section.raw_size << "\n";

        std::cout
            << "entry_section_delta=0x"
            << delta << "\n";

        if (delta + 4u <= section.bytes.size()) {
          std::cout
              << "section_bytes_word=0x"
              << read_be32_at(
                     section.bytes,
                     delta)
              << "\n";
        } else {
          std::cout
              << "section_bytes_word=<out-of-range>\n";
        }

        const auto raw_offset =
            static_cast<std::size_t>(
                section.raw_pointer) +
            delta;

        std::cout
            << "raw_pointer_offset=0x"
            << raw_offset << "\n";

        if (raw_offset + 4u <=
            report.image.effective_image.size()) {
          std::cout
              << "raw_pointer_word=0x"
              << read_be32_at(
                     report.image.effective_image,
                     raw_offset)
              << "\n";
        } else {
          std::cout
              << "raw_pointer_word=<out-of-range>\n";
        }

        break;
      }

      std::cout
          << "=== END ENTRY BYTE LAYOUT DIAGNOSTIC ===\n\n";
      std::cout << std::dec;
    }

    std::cout << "format=" << static_cast<unsigned>(report.image.format) << " entry=0x" << std::hex << report.image.entry_point
              << " sections=" << std::dec << report.image.sections.size() << " imports=" << report.image.imports.size()
              << " exports=" << report.image.exports.size()
              << " function_metadata=" << report.image.function_metadata.size() << "\n";
  } else {
    std::cout << xenon::recomp::format_report(report);
  }
  return report.unresolved.empty() ? 0 : 4;
}
