#pragma once

// Module hint inputs to analysis: the legacy ModuleHint model and its
// providers, and the Analysis Hint Schema V2 provider interface.

#include <cstdint>
#include <string>
#include <vector>

#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::recomp {

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

// Production source of Analysis Hint Schema V2 data (Part 2 of the
// Gracemeria readiness pass) - distinct from the legacy ModuleHintProvider
// above, which stays supported unchanged for existing callers/tests. A real
// implementation (see module_hint_provider.hpp's FileModuleHintProvider)
// reads installed module package data from disk; this interface is what the
// Recomp Driver actually calls.
class ModuleHintProviderV2 {
 public:
  virtual ~ModuleHintProviderV2() = default;

  // Supplies the hint set scoped to `identity` (the executable actually
  // being analyzed - see xbox::compute_effective_identity()). Returns false
  // with `error` set when this provider has no hint set for that exact
  // revision - a hard failure the caller must not paper over with a
  // different revision's data (Part 2.5: "fail clearly", never the
  // nearest-looking revision).
  [[nodiscard]] virtual bool provide(const xbox::XexEffectiveIdentity& identity,
                                     analysis::AnalysisHintSetV2& out_hint_set,
                                     std::string& error) const = 0;
};

}  // namespace xenon::recomp
