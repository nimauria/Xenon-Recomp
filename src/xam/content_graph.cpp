#include "xenon/xam/content_graph.hpp"

namespace xenon::xam {

std::string_view to_string(ContentNodeType type) noexcept {
  switch (type) {
    case ContentNodeType::BaseGame: return "BaseGame";
    case ContentNodeType::TitleUpdate: return "TitleUpdate";
    case ContentNodeType::DLC: return "DLC";
    case ContentNodeType::SaveData: return "SaveData";
    case ContentNodeType::Profile: return "Profile";
    case ContentNodeType::WritableStorage: return "WritableStorage";
    default: return "Unknown";
  }
}

std::string_view to_string(ContentStatus status) noexcept {
  switch (status) {
    case ContentStatus::Unknown: return "Unknown";
    case ContentStatus::Available: return "Available";
    case ContentStatus::Mounted: return "Mounted";
    case ContentStatus::Missing: return "Missing";
    case ContentStatus::Invalid: return "Invalid";
    case ContentStatus::Incompatible: return "Incompatible";
    default: return "Unknown";
  }
}

}  // namespace xenon::xam
