// hint-port: carries a module's analysis hint set from the executable
// revision it was written for to another revision of the same title (another
// region's disc, a later build). See xenon/recomp/revision_mapping.hpp.
//
//   hint-port <source-content> <target-content> <source-hints.json> <out-hints.json>
//             [--executable <relative-path>] [--report <report.txt>] [--map <map.json>]
//
// --map writes the alignment itself - {"sourceImageHash", "targetImageHash",
// "segments": [[source, target, words], ...]} - so a module can translate
// its own metadata files through the same verified mapping.
//
// Content is a disc image, an extracted folder or a loose XEX, read the same
// way game preparation reads it. Exits 0 when every hint translated, 3 when
// some were left behind (the ported set is still written, and each missing
// hint is listed), and 1/2 on usage or input errors.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "xenon/recomp/analysis_schema_json.hpp"
#include "xenon/recomp/game_intake.hpp"
#include "xenon/recomp/revision_mapping.hpp"

namespace {

bool load_executable(const std::filesystem::path& content, const std::string& relative_path,
                     xenon::xbox::XexImage& image) {
  std::vector<std::byte> bytes;
  std::string error;
  if (!xenon::recomp::read_game_executable_bytes(content, relative_path, bytes, error)) {
    std::cerr << "hint-port: " << content.string() << ": " << error << "\n";
    return false;
  }
  if (!xenon::xbox::parse_xex_image(bytes, image, &error)) {
    std::cerr << "hint-port: " << content.string() << ": not a supported XEX: " << error << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> positional;
  std::string executable = "default.xex";
  std::filesystem::path report_path;
  std::filesystem::path map_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--executable" && i + 1 < argc) executable = argv[++i];
    else if (arg == "--report" && i + 1 < argc) report_path = argv[++i];
    else if (arg == "--map" && i + 1 < argc) map_path = argv[++i];
    else positional.push_back(arg);
  }
  if (positional.size() != 4) {
    std::cerr << "usage: hint-port <source-content> <target-content> <source-hints.json> "
                 "<out-hints.json> [--executable <relative-path>] [--report <report.txt>] "
                 "[--map <map.json>]\n";
    return 1;
  }

  xenon::xbox::XexImage source, target;
  if (!load_executable(positional[0], executable, source) ||
      !load_executable(positional[1], executable, target))
    return 2;

  std::ifstream hints_file(positional[2], std::ios::binary);
  std::ostringstream hints_text;
  hints_text << hints_file.rdbuf();
  xenon::core::JsonValue hints_json;
  std::string error;
  xenon::recomp::analysis::AnalysisHintSetV2 hints;
  if (!hints_file || !xenon::core::JsonValue::parse(hints_text.str(), hints_json, &error) ||
      !xenon::recomp::analysis::from_json(hints_json, hints, error)) {
    std::cerr << "hint-port: " << positional[2] << ": " << (error.empty() ? "cannot read" : error) << "\n";
    return 2;
  }

  const auto map = xenon::recomp::RevisionAddressMap::build(source, target);
  xenon::recomp::HintPortReport report;
  const auto ported =
      xenon::recomp::port_hint_set(hints, map, xenon::xbox::compute_effective_identity(target), report);

  std::ofstream out(positional[3], std::ios::binary);
  out << xenon::recomp::analysis::to_json(ported).dump(2) << "\n";
  if (!out) {
    std::cerr << "hint-port: cannot write " << positional[3] << "\n";
    return 2;
  }

  if (!map_path.empty()) {
    auto map_json = xenon::core::JsonValue::make_object();
    map_json.set("sourceImageHash", xenon::xbox::format_effective_image_hash(
                                        xenon::xbox::compute_effective_image_hash(source)));
    map_json.set("targetImageHash", xenon::xbox::format_effective_image_hash(
                                        xenon::xbox::compute_effective_image_hash(target)));
    auto segments = xenon::core::JsonValue::make_array();
    for (const auto& segment : map.segments()) {
      auto entry = xenon::core::JsonValue::make_array();
      entry.append(xenon::core::JsonValue(segment.source));
      entry.append(xenon::core::JsonValue(segment.target));
      entry.append(xenon::core::JsonValue(segment.words));
      segments.append(std::move(entry));
    }
    map_json.set("segments", std::move(segments));
    std::ofstream map_file(map_path, std::ios::binary);
    map_file << map_json.dump() << "\n";
    if (!map_file) {
      std::cerr << "hint-port: cannot write " << map_path.string() << "\n";
      return 2;
    }
  }

  std::ostringstream summary;
  summary << "aligned " << map.mapped_words() << " of " << map.source_words() << " source instructions in "
          << map.segments().size() << " segments\n"
          << "hints ported: " << report.mapped << " (" << report.by_reference
          << " entries placed by their references), left behind: " << report.unmapped.size() << "\n";
  for (const auto& line : report.unmapped) summary << "  unmapped " << line << "\n";
  std::cout << summary.str();
  if (!report_path.empty()) std::ofstream(report_path) << summary.str();
  return report.unmapped.empty() ? 0 : 3;
}
