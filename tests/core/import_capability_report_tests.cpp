// Phase 6 (AC6 Runtime Readiness pass): whole-XEX import capability audit
// classification. Tests xenon::core::classify_import()/to_string() - the
// small, pure function tools/recomp_tools.cpp's import-scanner uses -
// against synthetic ExportDescriptor values, independent of any real XEX
// file (see tools/recomp_tools.cpp for the end-to-end CLI tool that drives
// this against a real title's import table and a real ExportRegistry).

#include "xenon/core/import_classification.hpp"

#include <cassert>
#include <iostream>

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

}  // namespace

int main() {
  std::cout << "Testing import capability classification...\n";

  test_missing_when_not_resolved();
  test_implemented_for_a_required_non_partial_export();
  test_implemented_for_optional_and_diagnostic_only_too();
  test_safe_stub_for_a_non_partial_stub();
  test_partial_takes_priority_over_requirement();

  std::cout << "All import capability classification tests passed!\n";
  return 0;
}
