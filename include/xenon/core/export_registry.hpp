#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"

namespace xenon::core {

// Classification for export behavior when not implemented
enum class ExportRequirement : std::uint8_t {
  Required,       // Must be implemented; fatal if missing
  Stubbed,        // Stubbed implementation exists (e.g., returns success)
  Optional,       // Game may not use it; diagnostic only
  DiagnosticOnly  // Logged but does not affect execution
};

// Result of an export call
struct ExportCallResult {
  bool handled{false};
  bool success{false};
  std::string diagnostic_message{};
};

// Context passed to export handlers
struct ExportCallContext {
  cpu::CpuState& cpu;
  cpu::MemoryPort& memory;
  cpu::GuestAddress call_address{};
  std::uint32_t thread_id{};
};

// Export handler function type
using ExportHandler = std::function<bool(ExportCallContext& context)>;

// Descriptor for a single export
struct ExportDescriptor {
  std::string library{};
  std::string name{};
  std::uint32_t ordinal{};
  ExportHandler handler{};
  ExportRequirement requirement{ExportRequirement::Required};
};

// Guest-backed variable exported by an Xbox system module. XEX native-import
// type-0 records for true variables are rewritten to this guest address before
// execution, matching the console loader's variable-import contract.
struct VariableExportDescriptor {
  std::string library{};
  std::string name{};
  std::uint32_t ordinal{};
  cpu::GuestAddress guest_address{};
};

// Unified Xbox export registry
// Replaces subsystem-specific import dispatchers with a common model
// Supports xboxkrnl.exe, xam.xex, xbdm.xex, and future system modules
class ExportRegistry {
 public:
  ExportRegistry() = default;
  ~ExportRegistry() = default;

  ExportRegistry(const ExportRegistry&) = delete;
  ExportRegistry& operator=(const ExportRegistry&) = delete;

  // Register an export by library and ordinal
  [[nodiscard]] bool register_export(ExportDescriptor descriptor);

  // Register multiple exports at once
  [[nodiscard]] bool register_exports(std::span<const ExportDescriptor> descriptors);

  // Unregister an export
  [[nodiscard]] bool unregister_export(std::string_view library, std::uint32_t ordinal);

  // Resolve an export by library and ordinal
  [[nodiscard]] const ExportDescriptor* resolve(std::string_view library,
                                                std::uint32_t ordinal) const;

  // Resolve an export by library and name
  [[nodiscard]] const ExportDescriptor* resolve(std::string_view library,
                                                std::string_view name) const;

  // Invoke an export by library and ordinal
  [[nodiscard]] ExportCallResult invoke(std::string_view library,
                                       std::uint32_t ordinal,
                                       ExportCallContext& context) const;

  // Invoke an export by library and name
  [[nodiscard]] ExportCallResult invoke(std::string_view library,
                                       std::string_view name,
                                       ExportCallContext& context) const;

  // Register/resolve a guest-backed variable export. Function and variable
  // namespaces are intentionally separate because Xbox modules may expose
  // both through the same ordinal-oriented import format.
  [[nodiscard]] bool register_variable(VariableExportDescriptor descriptor);
  [[nodiscard]] bool unregister_variable(std::string_view library,
                                         std::uint32_t ordinal);
  [[nodiscard]] std::optional<cpu::GuestAddress> resolve_variable(
      std::string_view library, std::uint32_t ordinal) const;
  [[nodiscard]] std::optional<cpu::GuestAddress> resolve_variable(
      std::string_view library, std::string_view name) const;
  [[nodiscard]] bool contains_variable(std::string_view library,
                                       std::uint32_t ordinal) const;

  // Check if an export exists
  [[nodiscard]] bool contains(std::string_view library, std::uint32_t ordinal) const;
  [[nodiscard]] bool contains(std::string_view library, std::string_view name) const;

  // Enumerate all registered exports
  [[nodiscard]] std::vector<ExportDescriptor> enumerate(
      std::string_view library = {}) const;

  // Clear all registered exports
  void clear();

 private:
  [[nodiscard]] static std::string normalize_library(std::string_view library);
  [[nodiscard]] static std::string make_ordinal_key(std::string_view library,
                                                    std::uint32_t ordinal);
  [[nodiscard]] static std::string make_name_key(std::string_view library,
                                                 std::string_view name);

  mutable std::shared_mutex mutex_{};
  std::unordered_map<std::string, ExportDescriptor> ordinal_map_{};
  std::unordered_map<std::string, ExportDescriptor> name_map_{};
  std::unordered_map<std::string, VariableExportDescriptor> variable_ordinal_map_{};
  std::unordered_map<std::string, VariableExportDescriptor> variable_name_map_{};
};

}  // namespace xenon::core
