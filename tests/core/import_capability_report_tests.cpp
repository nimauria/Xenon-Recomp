// Phase 6 (AC6 Runtime Readiness pass): whole-XEX import capability audit
// classification. Tests xenon::core::classify_import()/to_string() - the
// small, pure function tools/recomp_tools.cpp's import-scanner uses -
// against synthetic ExportDescriptor values, independent of any real XEX
// file (see tools/recomp_tools.cpp for the end-to-end CLI tool that drives
// this against a real title's import table and a real ExportRegistry).

#include "xenon/core/import_classification.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <utility>

using namespace xenon::core;

namespace {

void test_missing_when_not_resolved() {
  assert(classify_import(nullptr) == ImportClassification::Missing);
  assert(to_string(ImportClassification::Missing) == "MISSING");
}

void test_implemented_for_a_required_non_partial_export() {
  ExportDescriptor descriptor{};
  descriptor.requirement = ExportRequirement::Required;
  descriptor.partial = false;
  assert(classify_import(&descriptor) == ImportClassification::Implemented);
  assert(to_string(ImportClassification::Implemented) == "IMPLEMENTED");
}

void test_implemented_for_optional_and_diagnostic_only_too() {
  // Optional/DiagnosticOnly are real, working exports (just not required by
  // every title) - they must not be reported as anything other than
  // IMPLEMENTED unless also flagged partial.
  ExportDescriptor optional{};
  optional.requirement = ExportRequirement::Optional;
  assert(classify_import(&optional) == ImportClassification::Implemented);

  ExportDescriptor diagnostic_only{};
  diagnostic_only.requirement = ExportRequirement::DiagnosticOnly;
  assert(classify_import(&diagnostic_only) == ImportClassification::Implemented);
}

void test_safe_stub_for_a_non_partial_stub() {
  ExportDescriptor descriptor{};
  descriptor.requirement = ExportRequirement::Stubbed;
  descriptor.partial = false;
  assert(classify_import(&descriptor) == ImportClassification::SafeStub);
  assert(to_string(ImportClassification::SafeStub) == "SAFE_STUB");
}

void test_partial_takes_priority_over_requirement() {
  // A partial export is reported as PARTIAL regardless of whether its
  // ExportRequirement is Required or Stubbed - `partial` is a distinct,
  // more specific signal than the requirement level.
  ExportDescriptor required_but_partial{};
  required_but_partial.requirement = ExportRequirement::Required;
  required_but_partial.partial = true;
  assert(classify_import(&required_but_partial) == ImportClassification::Partial);

  ExportDescriptor stubbed_and_partial{};
  stubbed_and_partial.requirement = ExportRequirement::Stubbed;
  stubbed_and_partial.partial = true;
  assert(classify_import(&stubbed_and_partial) == ImportClassification::Partial);

  assert(to_string(ImportClassification::Partial) == "PARTIAL");
}

void test_verdict_is_pass_only_when_fully_gap_free() {
  assert(compute_import_capability_verdict(10, 0, 0, 0) == ImportCapabilityVerdict::Pass);
  assert(to_string(ImportCapabilityVerdict::Pass) == "PASS");
}

void test_verdict_is_pass_with_fallback_for_stub_or_partial_with_no_missing() {
  // Reviewer feedback: everything a title needs resolves, but not every
  // resolution is a complete, gap-free implementation - distinct from a
  // clean PASS, but still not a FAIL (nothing is actually missing).
  assert(compute_import_capability_verdict(9, 1, 0, 0) ==
         ImportCapabilityVerdict::PassWithFallback);
  assert(compute_import_capability_verdict(9, 0, 1, 0) ==
         ImportCapabilityVerdict::PassWithFallback);
  assert(compute_import_capability_verdict(8, 1, 1, 0) ==
         ImportCapabilityVerdict::PassWithFallback);
  assert(to_string(ImportCapabilityVerdict::PassWithFallback) == "PASS_WITH_FALLBACK");
}

void test_verdict_is_fail_whenever_anything_is_missing() {
  // Missing always wins, even alongside safe stubs/partials - a single
  // unresolved required import must never be masked by unrelated successes.
  assert(compute_import_capability_verdict(10, 0, 0, 1) == ImportCapabilityVerdict::Fail);
  assert(compute_import_capability_verdict(0, 5, 5, 1) == ImportCapabilityVerdict::Fail);
  assert(to_string(ImportCapabilityVerdict::Fail) == "FAIL");
}

xenon::xbox::XexImport make_import(std::string module, std::uint16_t ordinal,
                                   xenon::xbox::XexImportKind kind) {
  xenon::xbox::XexImport import{};
  import.module = std::move(module);
  import.ordinal = ordinal;
  import.kind = kind;
  return import;
}

void register_function(ExportRegistry& registry, std::uint32_t ordinal, bool partial) {
  ExportDescriptor descriptor{};
  descriptor.library = "xboxkrnl";
  descriptor.name = "Function" + std::to_string(ordinal);
  descriptor.ordinal = ordinal;
  descriptor.partial = partial;
  descriptor.handler = [](ExportCallContext&) { return true; };
  assert(registry.register_export(std::move(descriptor)));
}

// Regression: import-scanner reported every xboxkrnl variable import of a
// retail title (KeTimeStampBundle, XboxKrnlVersion, ...) as MISSING because it
// looked variables up in the function namespace, and counted each function
// import twice (its FunctionAddress record and its thunk).
void test_xex_variable_import_resolves_in_the_variable_namespace() {
  using xenon::xbox::XexImportKind;
  ExportRegistry registry;
  VariableExportDescriptor variable{};
  variable.library = "xboxkrnl.exe";
  variable.name = "KeTimeStampBundle";
  variable.ordinal = 0xADu;
  variable.guest_address = 0x80001020u;
  assert(registry.register_variable(variable));

  const auto resolved =
      classify_xex_import(registry, make_import("xboxkrnl.exe", 0xAD, XexImportKind::Variable));
  assert(resolved.has_value());
  assert(resolved->status == ImportClassification::Implemented);
  assert(resolved->function == nullptr);
  assert(resolved->variable_address == 0x80001020u);

  // An unregistered variable is Missing, never resolved through a function
  // export that happens to share its ordinal.
  register_function(registry, 0x158u, false);
  const auto missing =
      classify_xex_import(registry, make_import("xboxkrnl.exe", 0x158, XexImportKind::Variable));
  assert(missing.has_value());
  assert(missing->status == ImportClassification::Missing);
  assert(!missing->variable_address.has_value());
}

void test_xex_function_import_is_counted_once_through_its_thunk() {
  using xenon::xbox::XexImportKind;
  ExportRegistry registry;
  register_function(registry, 0x10u, false);
  register_function(registry, 0x11u, true);

  assert(!classify_xex_import(registry, make_import("xboxkrnl.exe", 0x10,
                                                    XexImportKind::FunctionAddress))
              .has_value());

  const auto thunk = classify_xex_import(
      registry, make_import("xboxkrnl.exe", 0x10, XexImportKind::FunctionThunk));
  assert(thunk.has_value());
  assert(thunk->status == ImportClassification::Implemented);
  assert(thunk->function != nullptr && thunk->function->ordinal == 0x10u);

  const auto partial = classify_xex_import(
      registry, make_import("xboxkrnl.exe", 0x11, XexImportKind::FunctionThunk));
  assert(partial.has_value() && partial->status == ImportClassification::Partial);

  const auto absent = classify_xex_import(
      registry, make_import("xboxkrnl.exe", 0x12, XexImportKind::FunctionThunk));
  assert(absent.has_value() && absent->status == ImportClassification::Missing);

  // A record of unknown kind resolves to nothing at load time either.
  const auto unknown =
      classify_xex_import(registry, make_import("xboxkrnl.exe", 0x10, XexImportKind::Unknown));
  assert(unknown.has_value() && unknown->status == ImportClassification::Missing);
}

}  // namespace

int main() {
  std::cout << "Testing import capability classification...\n";

  test_missing_when_not_resolved();
  test_implemented_for_a_required_non_partial_export();
  test_implemented_for_optional_and_diagnostic_only_too();
  test_safe_stub_for_a_non_partial_stub();
  test_partial_takes_priority_over_requirement();
  test_verdict_is_pass_only_when_fully_gap_free();
  test_verdict_is_pass_with_fallback_for_stub_or_partial_with_no_missing();
  test_verdict_is_fail_whenever_anything_is_missing();
  test_xex_variable_import_resolves_in_the_variable_namespace();
  test_xex_function_import_is_counted_once_through_its_thunk();

  std::cout << "All import capability classification tests passed!\n";
  return 0;
}
