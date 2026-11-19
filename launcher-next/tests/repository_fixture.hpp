#pragma once
#include "core/state_file.hpp"
#include "picosha2/picosha2.h"
#include <fstream>
#include <map>
// Publish raw repository files and a package index, without a GitHub release fixture.
inline void repository_fixture(const std::filesystem::path &root,
                               xenon::launcher_next::Json &mapping, const std::string &base,
                               const std::string &id, const std::string &version, bool valid = true,
                               const std::string &content = "verified package") {
  using namespace xenon::launcher_next;
  Json rows = Json::array();
  const auto manifest = Json({{"id", id},
                              {"name", "Fixture module"},
                              {"version", version},
                              {"nativeExtension", "module.dll"},
                              {"settings", Json::array()}})
                            .dump();
  for (const auto &[path, bytes] :
       std::map<std::string, std::string>{{"xenon-module.json", manifest},
                                          {"example.txt", content},
                                          {"module.dll", std::string("test-only\0dll", 13)}}) {
    std::ofstream(root / path, std::ios::binary) << bytes;
    rows.push_back({{"path", path},
                    {"size", bytes.size()},
                    {"sha256", valid ? picosha2::hash256_hex_string(bytes.begin(), bytes.end())
                                     : std::string(64, '0')}});
    mapping[base + "/" + path] = {{"file", path}};
  }
  write_document(root / "repository-package.json", {{"schema", "xenon.repository-package"},
                                                    {"version", 1},
                                                    {"moduleId", id},
                                                    {"moduleVersion", version},
                                                    {"files", rows}});
  mapping[base + "/package.json"] = {{"file", "repository-package.json"}};
  write_document(root / "responses.json", mapping);
}
