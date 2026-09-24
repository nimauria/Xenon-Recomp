#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "xenon/cpu/function_compiler.hpp"

namespace xenon::recomp::graph {
// Bump only the producer whose contract changed. Native compilation additionally
// fingerprints the compiler, command line, headers and environment.
struct Versions {
  std::string decoder{"ppc-decoder-1"};
  std::string analysis{"cfg-9"};
  std::string semantics{"cpu-8"};
  std::string ir{"xenon-ir-1"};
  std::string codegen{"cpp-aot-10"};
};
[[nodiscard]] std::string digest(std::string_view bytes);
[[nodiscard]] std::string producer_identity(std::string_view stage);
// Content-addressed fingerprint of the exact producer/toolchain/header/library
// environment a native xenon_game_module would be compiled with: Xenon's own
// include/src/cmake trees, the CMake driver script, the compiler itself (and
// its adjacent helper binaries), and toolchain-relevant environment variables
// (including every header/import-library directory INCLUDE/LIB name). Shared
// between tools/xenon_prepare.cpp (which sets ArtifactCacheKey::preparation_identity
// from it) and tests that need to reconstruct the exact same cache key
// independently, so the two can never silently drift apart.
[[nodiscard]] std::string preparation_identity(const std::filesystem::path& source_root,
                                               const std::filesystem::path& cmake_command,
                                               const std::filesystem::path& native_compiler);
struct Node {
  std::string stage;
  std::string producer;
  std::map<std::string, std::string> inputs;
  std::vector<std::string> dependencies;
  [[nodiscard]] std::string canonical() const;
  [[nodiscard]] std::string key() const;
};
struct Result { std::string bytes; std::string output_hash; bool hit{}; };
// Immutable entries. A complete private directory is renamed into place; a
// concurrent winner is accepted only after verifying its full manifest/payload.
class Store {
 public:
  explicit Store(std::filesystem::path root) : root_(std::move(root)) {}
  [[nodiscard]] std::optional<Result> lookup(const Node& node) const;
  [[nodiscard]] Result publish(const Node& node, std::string bytes) const;
  [[nodiscard]] const std::filesystem::path& root() const { return root_; }
 private:
  std::filesystem::path root_;
};
[[nodiscard]] std::string serialize_ir(const cpu::ir::Function& function);
[[nodiscard]] bool deserialize_ir(std::string_view bytes, cpu::ir::Function& function);
struct RegionResult {
  cpu::FunctionCompileResult compiled;
  std::vector<Node> nodes;
  bool ir_hit{};
};
// Discovery still runs against the current image/hints. Only the exact ranges
// it proves to be owned by this region are admitted to the reusable IR boundary.
[[nodiscard]] RegionResult compile_region(const Store& store, const Versions& versions,
    cpu::GuestAddress entry, std::span<const cpu::FunctionCodeRange> ranges);
} // namespace xenon::recomp::graph
