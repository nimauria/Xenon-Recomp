#include "xenon/core/import_classification.hpp"

namespace xenon::core {

ImportClassification classify_import(const ExportDescriptor* descriptor) noexcept {
  if (descriptor == nullptr) {
    return ImportClassification::Missing;
  }
  if (descriptor->partial) {
    return ImportClassification::Partial;
  }
  if (descriptor->requirement == ExportRequirement::Stubbed) {
    return ImportClassification::SafeStub;
  }
  return ImportClassification::Implemented;
}

std::string_view to_string(ImportClassification classification) noexcept {
  switch (classification) {
    case ImportClassification::Implemented: return "IMPLEMENTED";
    case ImportClassification::SafeStub: return "SAFE_STUB";
    case ImportClassification::Partial: return "PARTIAL";
    case ImportClassification::Missing: return "MISSING";
  }
  return "MISSING";
}

ImportCapabilityVerdict compute_import_capability_verdict(
    std::size_t implemented, std::size_t safe_stub, std::size_t partial,
    std::size_t missing) noexcept {
  static_cast<void>(implemented);
  if (missing > 0u) return ImportCapabilityVerdict::Fail;
  if (safe_stub > 0u || partial > 0u) return ImportCapabilityVerdict::PassWithFallback;
  return ImportCapabilityVerdict::Pass;
}

std::string_view to_string(ImportCapabilityVerdict verdict) noexcept {
  switch (verdict) {
    case ImportCapabilityVerdict::Pass: return "PASS";
    case ImportCapabilityVerdict::PassWithFallback: return "PASS_WITH_FALLBACK";
    case ImportCapabilityVerdict::Fail: return "FAIL";
  }
  return "FAIL";
}

}  // namespace xenon::core
