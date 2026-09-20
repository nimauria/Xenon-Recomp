#include "xenon/recomp/driver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/function_compiler.hpp"

namespace xenon::recomp {
namespace {
using cpu::GuestAddress;

std::uint32_t be32(const std::vector<std::byte>& data, std::size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return (std::to_integer<std::uint32_t>(data[offset]) << 24) |
         (std::to_integer<std::uint32_t>(data[offset + 1]) << 16) |
         (std::to_integer<std::uint32_t>(data[offset + 2]) << 8) |
         std::to_integer<std::uint32_t>(data[offset + 3]);
}

std::uint64_t hash_bytes(std::span<const std::byte> bytes, std::uint64_t seed = 1469598103934665603ull) {
  auto hash = seed;
  for (const auto byte : bytes) {
    hash ^= std::to_integer<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t hash_config(const DriverOptions& options) {
  std::uint64_t hash = hash_bytes({});
  for (const auto& hint : options.hints) {
    hash = hash_bytes(std::as_bytes(std::span(hint.name.data(), hint.name.size())), hash);
    for (const auto address : hint.function_boundaries)
      hash = hash_bytes(std::as_bytes(std::span(&address, 1)), hash);
    for (const auto address : hint.data_regions)
      hash = hash_bytes(std::as_bytes(std::span(&address, 1)), hash);
    for (const auto& symbol : hint.known_symbols) {
      hash = hash_bytes(std::as_bytes(std::span(&symbol.address, 1)), hash);
      hash = hash_bytes(std::as_bytes(std::span(symbol.name.data(), symbol.name.size())), hash);
    }
    for (const auto& hook : hint.special_hooks)
      hash = hash_bytes(std::as_bytes(std::span(hook.data(), hook.size())), hash);
    for (const auto& patch : hint.patches)
      hash = hash_bytes(std::as_bytes(std::span(patch.data(), patch.size())), hash);
  }
  return hash;
}

const xbox::XexSection* executable_section(const xbox::XexImage& image, GuestAddress address) {
  for (const auto& section : image.sections) {
    const auto end = static_cast<std::uint64_t>(section.virtual_address) +
                     std::max(section.virtual_size, section.raw_size);
    if (section.executable && address >= section.virtual_address && address < end)
      return &section;
  }
  return nullptr;
}

bool source_contains(const DiscoveredFunction& function, DiscoverySource source) {
  return std::find(function.sources.begin(), function.sources.end(), source) != function.sources.end();
}

void add_source(DiscoveredFunction& function, DiscoverySource source) {
  if (!source_contains(function, source)) function.sources.push_back(source);
}

std::string cpp_name(std::uint32_t address) {
  std::ostringstream out;
  out << "xenon_fn_" << std::hex << std::uppercase << address;
  return out.str();
}

std::string sanitize_cpp_name(std::string name, std::uint32_t address) {
  for (auto& character : name)
    if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
      character = '_';
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
    name = "xenon_" + name;
  return name.empty() ? cpp_name(address) : name;
}

bool parse_address_list(const std::string& value, std::vector<std::uint32_t>& output) {
  std::istringstream input(value);
  std::string token;
  while (std::getline(input, token, ',')) {
    try {
      output.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 0)));
    } catch (const std::exception&) {
      return false;
    }
  }
  return true;
}

bool load_hints(const DriverOptions& options, std::vector<ModuleHint>& hints, std::string& error) {
  hints = options.hints;
  if (options.hints_file.empty()) return true;
  std::ifstream input(options.hints_file);
  if (!input) {
    error = "unable to open module hints: " + options.hints_file.string();
    return false;
  }
  ModuleHint hint{};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      error = "invalid module hint line: " + line;
      return false;
    }
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "name") hint.name = value;
    else if (key == "function_boundaries" && !parse_address_list(value, hint.function_boundaries))
      error = "invalid function_boundaries hint";
    else if (key == "data_regions" && !parse_address_list(value, hint.data_regions))
      error = "invalid data_regions hint";
    else if (key == "ignored_regions" && !parse_address_list(value, hint.ignored_regions))
      error = "invalid ignored_regions hint";
    else if (key == "known_symbols") {
      const auto at = value.find('@');
      if (at == std::string::npos) {
        error = "known_symbols must use name@address";
      } else {
        try {
          hint.known_symbols.push_back({static_cast<std::uint32_t>(
              std::stoul(value.substr(at + 1), nullptr, 0)), value.substr(0, at)});
        } catch (const std::exception&) {
          error = "invalid known_symbols hint";
        }
      }
    }
    else if (key == "special_hooks") hint.special_hooks.push_back(value);
    else if (key == "patches") hint.patches.push_back(value);
    else {
      error = "unknown module hint key: " + key;
    }
    if (!error.empty()) return false;
  }
  if (!hint.name.empty() || !hint.function_boundaries.empty() || !hint.known_symbols.empty())
    hints.push_back(std::move(hint));
  return true;
}

bool is_terminal(const cpu::DecodedInstruction& instruction) {
  return instruction.info && instruction.info->group == cpu::InstructionGroup::Branch &&
         !instruction.lk();
}

std::string hash_name(std::uint64_t hash) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  for (const char character : value) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    escaped.push_back(character);
  }
  return escaped;
}

}  // namespace

void ModuleCatalog::register_provider(const ModuleHintProvider& provider) {
  providers_.push_back(&provider);
}

const char* discovery_source_name(DiscoverySource source) noexcept {
  switch (source) {
    case DiscoverySource::EntryPoint: return "entry";
    case DiscoverySource::Export: return "export";
    case DiscoverySource::DirectCall: return "direct-call";
    case DiscoverySource::DirectBranch: return "direct-branch";
    case DiscoverySource::UnwindMetadata: return "unwind-metadata";
    case DiscoverySource::ModuleHint: return "module-hint";
  }
  return "unknown";
}

bool load_and_analyze(const DriverOptions& options, AnalysisReport& report, std::string& error) {
  std::vector<ModuleHint> hints;
  if (!load_hints(options, hints, error)) return false;
  DriverOptions effective = options;
  effective.hints = std::move(hints);
  std::ifstream file(options.input, std::ios::binary);
  if (!file) { error = "unable to open input XEX: " + options.input.string(); return false; }
  const std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
  std::vector<std::byte> bytes(raw.size());
  std::transform(raw.begin(), raw.end(), bytes.begin(),
                 [](char value) { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
  if (!xbox::parse_xex_image(bytes, report.image, &error)) return false;
  for (const auto* provider : effective.module_providers) {
    if (provider == nullptr) continue;
    if (!provider->provide(report.image, effective.hints, error)) return false;
  }
  report.configuration_hash = hash_config(effective);

  std::map<GuestAddress, DiscoverySource> seeds;
  seeds[report.image.entry_point] = DiscoverySource::EntryPoint;
  for (const auto& export_entry : report.image.exports)
    seeds[export_entry.address] = DiscoverySource::Export;
  for (const auto& hint : effective.hints)
    for (const auto address : hint.function_boundaries)
      seeds[address] = DiscoverySource::ModuleHint;
  std::map<GuestAddress, std::string> known_names;
  for (const auto& hint : effective.hints)
    for (const auto& symbol : hint.known_symbols) {
      seeds[symbol.address] = DiscoverySource::ModuleHint;
      known_names[symbol.address] = symbol.name;
    }
  for (const auto& hint : effective.hints)
    for (const auto address : hint.data_regions)
      seeds.erase(address);
  for (const auto& hint : effective.hints)
    for (const auto address : hint.ignored_regions)
      seeds.erase(address);
  for (const auto& metadata : report.image.function_metadata)
    if (metadata.valid) seeds[metadata.begin] = DiscoverySource::UnwindMetadata;

  std::set<GuestAddress> pending;
  for (const auto& [address, source] : seeds)
    if (executable_section(report.image, address)) pending.insert(address);
    else report.unresolved.push_back({address, address, "seed", "target is not in an executable section"});

  cpu::Decoder decoder;
  while (!pending.empty()) {
    const auto start = *pending.begin();
    pending.erase(pending.begin());
    if (std::any_of(report.functions.begin(), report.functions.end(),
                    [start](const auto& f) { return f.guest_start == start; }))
      continue;
    const auto* section = executable_section(report.image, start);
    if (!section || start < section->virtual_address ||
        static_cast<std::uint64_t>(start - section->virtual_address) + 4 > section->bytes.size()) {
      report.unresolved.push_back({start, start, "function", "function start is outside section bytes"});
      continue;
    }

    DiscoveredFunction function{};
    function.guest_start = start;
    function.name = known_names.contains(start)
                        ? sanitize_cpp_name(known_names[start], start)
                        : cpp_name(start);
    add_source(function, seeds.contains(start) ? seeds[start] : DiscoverySource::DirectBranch);
    std::vector<std::uint32_t> words;
    const auto offset = static_cast<std::size_t>(start - section->virtual_address);
    const auto max_words = std::min<std::size_t>((section->bytes.size() - offset) / 4, 4096);
    std::optional<GuestAddress> metadata_end;
    for (const auto& metadata : report.image.function_metadata) {
      if (metadata.valid && metadata.begin == start) {
        metadata_end = metadata.end;
        add_source(function, DiscoverySource::UnwindMetadata);
        break;
      }
    }
    for (const auto& hint : effective.hints)
      for (const auto boundary : hint.function_boundaries)
        if (boundary > start && (!metadata_end || boundary < *metadata_end))
          metadata_end = boundary;
    const auto hinted_non_code = [&effective](GuestAddress address) {
      for (const auto& hint : effective.hints)
        if (std::find(hint.data_regions.begin(), hint.data_regions.end(), address) !=
                hint.data_regions.end() ||
            std::find(hint.ignored_regions.begin(), hint.ignored_regions.end(), address) !=
                hint.ignored_regions.end())
          return true;
      return false;
    };
    std::set<GuestAddress> local_boundaries;
    for (std::size_t index = 0; index < max_words; ++index) {
      const auto address = static_cast<GuestAddress>(start + index * 4u);
      if (index != 0 && hinted_non_code(address)) {
        report.warnings.push_back("function scan stopped at hinted non-code region 0x" +
                                  [&] { std::ostringstream s; s << std::hex << address; return s.str(); }());
        function.guest_end = address;
        break;
      }
      const auto instruction = decoder.decode(address, be32(section->bytes, offset + index * 4));
      if (!instruction.valid()) {
        function.error = "invalid PPC at 0x" + [&] { std::ostringstream s; s << std::hex << address; return s.str(); }();
        report.unresolved.push_back({address, be32(section->bytes, offset + index * 4), "invalid-ppc", function.error});
        break;
      }
      words.push_back(instruction.word);
      if (metadata_end && address + 4u >= *metadata_end) {
        function.guest_end = address + 4u;
        break;
      }
      if (instruction.info->group == cpu::InstructionGroup::Branch) {
        if (instruction.lk() && (instruction.info->format == cpu::InstructionFormat::I ||
                                 instruction.info->format == cpu::InstructionFormat::B)) {
          const auto target = instruction.direct_branch_target();
          function.calls.push_back(target);
          pending.insert(target);
        } else if (!instruction.lk() && (instruction.info->format == cpu::InstructionFormat::I ||
                                          instruction.info->format == cpu::InstructionFormat::B)) {
          const auto target = instruction.direct_branch_target();
          function.branch_references.push_back(target);
          local_boundaries.insert(target);
          pending.insert(target);
        } else if (instruction.info->format == cpu::InstructionFormat::XL) {
          report.unresolved.push_back({address, 0, instruction.lk() ? "indirect-call" : "indirect-branch",
                                       "target depends on runtime register state"});
          if (!instruction.lk()) {
            const auto table_start = offset + index * 4u + 4u;
            const auto table_end = std::min(section->bytes.size(), table_start + 256u);
            for (auto table = table_start; table + 4u <= table_end; table += 4u) {
              const auto target = be32(section->bytes, table);
              if (executable_section(report.image, target)) {
                function.branch_references.push_back(target);
                pending.insert(target);
                report.warnings.push_back("recovered possible jump-table target 0x" +
                                          [&] { std::ostringstream s; s << std::hex << target; return s.str(); }());
              }
            }
          }
        }
        if (is_terminal(instruction)) { function.guest_end = address + 4; break; }
      }
      function.guest_end = address + 4;
    }
    if (words.empty() || !function.error.empty()) {
      if (!effective.allow_partial) continue;
    } else {
      cpu::StaticFunctionCompiler compiler;
      const auto result = compiler.compile(start, words);
      if (!result.ok) {
        function.error = result.error;
        report.unresolved.push_back({result.error_address, result.error_word, "compile", result.error});
      } else {
        function.ir = result.function;
        function.compiled = true;
        function.confidence = local_boundaries.empty() ? 85 : 70;
        function.source_hash = hash_bytes(std::as_bytes(std::span(words)), report.configuration_hash);
      }
    }
    function.ranges = {function.guest_start, function.guest_end};
    report.functions.push_back(std::move(function));
  }

  std::sort(report.functions.begin(), report.functions.end(),
            [](const auto& a, const auto& b) { return a.guest_start < b.guest_start; });
  for (auto& function : report.functions) {
    for (auto& other : report.functions) {
      if (&function == &other) continue;
      if (function.guest_start < other.guest_end && other.guest_start < function.guest_end) {
        report.warnings.push_back("overlapping functions at 0x" +
                                  [&] { std::ostringstream s; s << std::hex << function.guest_start; return s.str(); }() +
                                  " and 0x" +
                                  [&] { std::ostringstream s; s << std::hex << other.guest_start; return s.str(); }());
        break;
      }
    }
    for (const auto target : function.calls)
      for (auto& callee : report.functions)
        if (callee.guest_start == target) {
          callee.callers.push_back(function.guest_start);
          add_source(callee, DiscoverySource::DirectCall);
        }
    for (const auto target : function.branch_references)
      if (!executable_section(report.image, target))
        report.unresolved.push_back({function.guest_start, target, "branch-target", "target is not executable"});
      else if (!std::any_of(report.functions.begin(), report.functions.end(),
                            [target](const auto& candidate) {
                              return candidate.guest_start == target;
                            }))
        report.unresolved.push_back({function.guest_start, target, "branch-into-unknown-code",
                                     "executable target has no discovered function boundary"});
  }
  return true;
}

bool generate_project(const DriverOptions& options, AnalysisReport& report, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(options.output / "functions", ec);
  if (ec) { error = "unable to create output directory: " + ec.message(); return false; }
  cpu::backend::CppAotBackend backend;
  std::vector<std::filesystem::path> shards;
  const auto cache_directory = options.output / ".cache";
  std::filesystem::create_directories(cache_directory, ec);
  std::string shard_text;
  std::filesystem::path shard_path;
  std::size_t index = 0;
  for (const auto& function : report.functions) {
    if (!function.compiled) continue;
    if (index % options.shard_function_count == 0) {
      if (!shard_path.empty()) {
        std::ifstream old(shard_path);
        const std::string previous((std::istreambuf_iterator<char>(old)), {});
        if (previous != shard_text) {
          std::ofstream output(shard_path);
          output << shard_text;
        }
      }
      const auto path = options.output / "functions" / ("shard_" + [&] {
        std::ostringstream s; s << std::setfill('0') << std::setw(3) << (index / options.shard_function_count); return s.str();
      }() + ".cpp");
      shards.push_back(path);
      shard_path = path;
      shard_text = "// Generated by xenon codegen. Do not edit.\n";
    }
    const auto cache_path = cache_directory / (hash_name(function.source_hash) + ".cpp");
    std::ifstream cached(cache_path);
    std::string function_source((std::istreambuf_iterator<char>(cached)), {});
    if (function_source.empty()) {
      function_source = backend.emit_translation_unit(function.ir, function.name);
      std::ofstream cache(cache_path);
      cache << function_source;
    }
    shard_text += function_source;
    ++index;
  }
  if (!shard_path.empty()) {
    std::ifstream old(shard_path);
    const std::string previous((std::istreambuf_iterator<char>(old)), {});
    if (previous != shard_text) {
      std::ofstream output(shard_path);
      output << shard_text;
    }
  }
  std::ostringstream registry_text;
  std::ofstream registry_header(options.output / "registry.hpp");
  registry_header << "#pragma once\n#include \"xenon/cpu/runtime.hpp\"\nnamespace xenon::recomp {\n"
                     "void bind_compiled_registry(xenon::cpu::ExecutionContext&) noexcept;\n"
                     "xenon::cpu::NativeCompiledEntry lookup_compiled(void*, xenon::cpu::ExecutionContext&, xenon::cpu::GuestAddress, xenon::cpu::CompiledLookupKind);\n"
                     "}\n";
  registry_text << "// Generated compiled-function registry.\n#include \"registry.hpp\"\n"
                   "#include <cstddef>\n#include <cstdint>\nnamespace xenon::recomp {\n"
                   "using CompiledFunction = xenon::cpu::ExecutionResult (*)(xenon::cpu::CpuState&, xenon::cpu::MemoryPort&, xenon::cpu::RuntimeServices&);\n"
                   "using CompiledFunctionV2 = xenon::cpu::NativeCompiledEntry;\n";
  for (const auto& function : report.functions)
    if (function.compiled)
      registry_text << "extern xenon::cpu::ExecutionResult " << function.name
                    << "(xenon::cpu::CpuState&, xenon::cpu::MemoryPort&, xenon::cpu::RuntimeServices&);\n";
  for (const auto& function : report.functions)
    if (function.compiled)
      registry_text << "extern xenon::cpu::ExecutionResult " << function.name
                    << "_v2(xenon::cpu::ExecutionContext&);\n";
  registry_text << "struct CompiledEntry { std::uint32_t guest_start; CompiledFunction function; };\n"
                << "const CompiledEntry kCompiledFunctions["
                << std::max<std::size_t>(1u, std::count_if(report.functions.begin(), report.functions.end(),
                                                           [](const auto& function) { return function.compiled; }))
                << "] = {\n";
  for (const auto& function : report.functions)
    if (function.compiled) registry_text << "  {0x" << std::hex << function.guest_start << ", &" << function.name << "},\n";
  const auto compiled_count = std::count_if(
      report.functions.begin(), report.functions.end(),
      [](const auto& function) { return function.compiled; });
  registry_text << "};\nconst std::size_t kCompiledFunctionCount = " << compiled_count << ";\n}\n";
  registry_text << "namespace xenon::recomp {\n"
                   "CompiledFunctionV2 lookup_compiled(void*, xenon::cpu::ExecutionContext&, xenon::cpu::GuestAddress target, xenon::cpu::CompiledLookupKind) {\n"
                   "  switch (target) {\n";
  for (const auto& function : report.functions)
    if (function.compiled)
      registry_text << "    case 0x" << std::hex << function.guest_start << ": return &" << function.name << "_v2;\n";
  registry_text << "    default: return nullptr;\n  }\n}\n"
                   "void bind_compiled_registry(xenon::cpu::ExecutionContext& context) noexcept {\n"
                   "  context.compiled_registry = nullptr;\n"
                   "  context.compiled_lookup = &lookup_compiled;\n"
                   "}\n}\n";
  std::ofstream registry(options.output / "registry.cpp");
  registry << registry_text.str();
  std::ofstream imports(options.output / "imports.cpp");
  imports << "// Generated import manifest.\n#include <cstddef>\n#include <cstdint>\nnamespace xenon::recomp {\n"
             "struct GeneratedImport { const char* module; const char* symbol; std::uint16_t ordinal; std::uint32_t thunk; };\n"
             "const GeneratedImport kGeneratedImports["
             + std::to_string(std::max<std::size_t>(1u, report.image.imports.size()))
             + "] = {\n";
  for (const auto& item : report.image.imports)
    imports << "  {\"" << item.module << "\", \"" << item.symbol << "\", " << item.ordinal << ", 0x"
            << std::hex << item.guest_thunk << "},\n";
  imports << "};\nconst std::size_t kGeneratedImportCount = "
             "sizeof(kGeneratedImports) / sizeof(kGeneratedImports[0]);\n}\n";
  std::ofstream metadata(options.output / "metadata.cpp");
  metadata << "// Generated analysis metadata.\n";
  metadata << "// entry=0x" << std::hex << report.image.entry_point << " functions=" << std::dec << report.functions.size()
           << " unresolved=" << report.unresolved.size() << " config_hash=0x" << std::hex << report.configuration_hash << "\n";
  std::ofstream hooks(options.output / "hooks.cpp");
  hooks << "// Generated module hooks and patch manifest. Runtime integration owns semantics.\n";
  for (const auto& hint : options.hints) {
    for (const auto& hook : hint.special_hooks)
      hooks << "// hook[" << hint.name << "] " << hook << "\n";
    for (const auto& patch : hint.patches)
      hooks << "// patch[" << hint.name << "] " << patch << "\n";
  }
  std::ofstream json(options.output / "analysis.json");
  json << format_report_json(report);
  std::ofstream manifest(options.output / "manifest.txt");
  manifest << "configuration_hash=0x" << std::hex << report.configuration_hash << "\n";
  for (const auto& path : shards) manifest << path.filename().string() << "\n";
  const auto function_directory = options.output / "functions";
  for (const auto& entry : std::filesystem::directory_iterator(function_directory, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp" ||
        entry.path().filename().string().find("shard_") != 0)
      continue;
    if (std::find(shards.begin(), shards.end(), entry.path()) == shards.end())
      std::filesystem::remove(entry.path(), ec);
  }
  std::ofstream build(options.output / "CMakeLists.txt");
  build << "cmake_minimum_required(VERSION 3.25)\n"
           "project(xenon_game_generated LANGUAGES CXX)\n"
           "set(CMAKE_CXX_STANDARD 20)\n"
           "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
           "set(XENON_BUILD_TESTS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_BUILD_BENCHMARKS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_BUILD_LAUNCHER OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_GRAPHICS OFF CACHE BOOL \"\" FORCE)\n"
           "set(XENON_ENABLE_INPUT OFF CACHE BOOL \"\" FORCE)\n"
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
           "target_link_libraries(xenon_game PRIVATE Xenon::CPU Xenon::Memory Xenon::XboxKernelIo)\n"
           "target_include_directories(xenon_game PRIVATE ${XENON_RECOMP_ROOT}/include)\n";
  return true;
}

std::string format_report(const AnalysisReport& report) {
  std::ostringstream out;
  out << "entry: 0x" << std::hex << report.image.entry_point << "\nfunctions: " << std::dec << report.functions.size()
      << "\nunresolved: " << report.unresolved.size() << "\n";
  for (const auto& function : report.functions)
    out << "0x" << std::hex << function.guest_start << "-0x" << function.guest_end << " "
        << function.name << " confidence=" << std::dec << function.confidence
        << " status=" << (function.compiled ? "compiled" : "unresolved") << "\n";
  for (const auto& warning : report.warnings)
    out << "warning: " << warning << "\n";
  for (const auto& item : report.unresolved)
    out << "warning 0x" << std::hex << item.address << " " << item.kind << ": " << item.detail << "\n";
  return out.str();
}

std::string format_report_json(const AnalysisReport& report) {
  std::ostringstream out;
  out << "{\n  \"entry_point\": " << report.image.entry_point
      << ",\n  \"configuration_hash\": " << report.configuration_hash
      << ",\n  \"functions\": [\n";
  for (std::size_t i = 0; i < report.functions.size(); ++i) {
    const auto& function = report.functions[i];
    out << "    {\"start\": " << function.guest_start
        << ", \"end\": " << function.guest_end
        << ", \"name\": \"" << json_escape(function.name)
        << "\", \"confidence\": " << function.confidence
        << ", \"compiled\": " << (function.compiled ? "true" : "false") << "}";
    if (i + 1 != report.functions.size()) out << ',';
    out << '\n';
  }
  out << "  ],\n  \"unresolved\": [\n";
  for (std::size_t i = 0; i < report.unresolved.size(); ++i) {
    const auto& item = report.unresolved[i];
    out << "    {\"address\": " << item.address << ", \"target\": " << item.target
        << ", \"kind\": \"" << json_escape(item.kind) << "\", \"detail\": \""
        << json_escape(item.detail) << "\"}";
    if (i + 1 != report.unresolved.size()) out << ',';
    out << '\n';
  }
  out << "  ]\n}\n";
  return out.str();
}

std::string format_ir(const DiscoveredFunction& function) {
  std::ostringstream out;
  out << "function " << function.name << " @ 0x" << std::hex << function.guest_start << "\n";
  for (const auto& block : function.ir.blocks)
    out << "  block 0x" << std::hex << block.guest_address << "-0x" << block.end_address
        << " instructions=" << std::dec << block.instructions.size() << "\n";
  for (const auto& block : function.ir.blocks)
    for (const auto& instruction : block.instructions)
      out << "    0x" << std::hex << instruction.guest_address
          << " word=0x" << instruction.guest_word
          << " op=" << std::dec << static_cast<unsigned>(instruction.op)
          << " result=" << instruction.result << "\n";
  return out.str();
}

}  // namespace xenon::recomp
