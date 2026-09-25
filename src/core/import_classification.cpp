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

}  // namespace xenon::core
