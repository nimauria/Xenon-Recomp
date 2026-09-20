#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "xenon/cpu/ir.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

enum class DiscoverySource : std::uint8_t {
  EntryPoint,
  Export,
  DirectCall,
  DirectBranch,
  UnwindMetadata,
  ModuleHint,
};

struct ModuleHint {
  struct Symbol {
    std::uint32_t address{};
    std::string name;
  };
  std::string name;
  std::vector<std::uint32_t> function_boundaries;
  std::vector<std::uint32_t> data_regions;
  std::vector<std::uint32_t> ignored_regions;
  std::vector<Symbol> known_symbols;
  std::vector<std::string> special_hooks;
  std::vector<std::string> patches;
};

class ModuleHintProvider {
 public:
  virtual ~ModuleHintProvider() = default;
  virtual bool provide(const xbox::XexImage& image,
                       std::vector<ModuleHint>& hints,
                       std::string& error) const = 0;
};

class ModuleCatalog {
 public:
  void register_provider(const ModuleHintProvider& provider);
  [[nodiscard]] const std::vector<const ModuleHintProvider*>& providers() const noexcept {
    return providers_;
  }

 private:
  std::vector<const ModuleHintProvider*> providers_;
};

struct UnresolvedReference {
  std::uint32_t address{};
  std::uint32_t target{};
  std::string kind;
  std::string detail;
};

struct DiscoveredFunction {
  std::uint32_t guest_start{};
  std::uint32_t guest_end{};
  std::vector<std::uint32_t> ranges;
  std::string name;
  std::vector<DiscoverySource> sources;
  std::vector<std::uint32_t> calls;
  std::vector<std::uint32_t> callers;
  std::vector<std::uint32_t> branch_references;
  std::uint32_t confidence{};
  std::uint64_t source_hash{};
  bool compiled{};
  std::string error;
  cpu::ir::Function ir;
};

struct AnalysisReport {
  xbox::XexImage image;
  std::vector<DiscoveredFunction> functions;
  std::vector<UnresolvedReference> unresolved;
  std::vector<std::string> warnings;
  std::uint64_t configuration_hash{};
};

struct DriverOptions {
  std::filesystem::path input;
  std::filesystem::path output{"generated"};
  std::filesystem::path hints_file;
  std::vector<ModuleHint> hints;
  std::vector<const ModuleHintProvider*> module_providers;
  std::size_t shard_function_count{128};
  bool allow_partial{false};
};

[[nodiscard]] bool load_and_analyze(const DriverOptions& options,
                                    AnalysisReport& report,
                                    std::string& error);
[[nodiscard]] bool generate_project(const DriverOptions& options,
                                    AnalysisReport& report,
                                    std::string& error);
[[nodiscard]] std::string format_report(const AnalysisReport& report);
[[nodiscard]] std::string format_report_json(const AnalysisReport& report);
[[nodiscard]] std::string format_ir(const DiscoveredFunction& function);
[[nodiscard]] const char* discovery_source_name(DiscoverySource source) noexcept;

}  // namespace xenon::recomp
