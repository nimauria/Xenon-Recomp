#include "xenon/core/capability_report.hpp"

#include <utility>

namespace xenon::core {

CapabilityReportBuilder& CapabilityReportBuilder::set_section(std::string name, JsonValue value) {
  sections_[std::move(name)] = std::move(value);
  return *this;
}

const JsonValue* CapabilityReportBuilder::find_section(std::string_view name) const {
  const auto it = sections_.find(std::string(name));
  return it == sections_.end() ? nullptr : &it->second;
}

bool CapabilityReportBuilder::has_section(std::string_view name) const {
  return find_section(name) != nullptr;
}

CapabilityReportBuilder& CapabilityReportBuilder::set_run_fingerprint(
    const RunFingerprint& fingerprint) {
  return set_section("runFingerprint", fingerprint.to_json());
}

JsonValue CapabilityReportBuilder::build() const {
  JsonValue sections = JsonValue::make_object();
  for (const auto& [name, value] : sections_) {
    sections.set(name, value);
  }
  JsonValue report = JsonValue::make_object();
  report.set("sections", std::move(sections));
  return report;
}

void CapabilityReportBuilder::clear() { sections_.clear(); }

}  // namespace xenon::core
