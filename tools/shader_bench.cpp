// shader-bench: recompiles the shaders a Xenon shader cache holds (the
// .xsc entries keep each shader's full HLSL and options) with DXC, and
// reports how long each compile setting takes. Development tool for tuning
// compile options against real game shaders.
//
//   shader-bench <shader cache directory> [--limit N] [--format dxil|spirv|all]
//                [--both-formats]   (compile every shader as DXIL and SPIR-V)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace {

using xenon::gpu::ShaderBinaryFormat;

bool get_blob(std::string_view& in, std::string_view& bytes) {
  std::uint64_t size{};
  if (in.size() < sizeof(size)) return false;
  std::memcpy(&size, in.data(), sizeof(size));
  in.remove_prefix(sizeof(size));
  if (in.size() < size) return false;
  bytes = in.substr(0, static_cast<std::size_t>(size));
  in.remove_prefix(static_cast<std::size_t>(size));
  return true;
}

struct Entry {
  xenon::gpu::LoweredShader shader;
  xenon::gpu::ShaderCompileOptions options;
};

// A disk entry: magic, then the key (compiler version, NUL, stage, format,
// flags, SPIR-V environment, NUL, profile, NUL, entry point, NUL, HLSL) and
// the binary, each length-prefixed (see src/graphics/dxc/shader_compiler.cpp).
bool parse_entry(const std::filesystem::path& path, Entry& entry) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::string_view in(data);
  if (in.size() < 8 || in.substr(0, 8) != std::string_view("XSHCACH1", 8)) return false;
  in.remove_prefix(8);
  std::string_view key;
  if (!get_blob(in, key)) return false;
  const auto version_end = key.find('\0');
  if (version_end == std::string_view::npos || key.size() < version_end + 4) return false;
  key.remove_prefix(version_end + 1);
  entry.shader.stage = static_cast<xenon::gpu::ShaderStage>(key[0]);
  entry.options.format = static_cast<ShaderBinaryFormat>(key[1]);
  const auto flags = static_cast<unsigned char>(key[2]);
  entry.options.debug = flags & 1;
  entry.options.optimize = flags & 2;
  entry.options.warnings_as_errors = flags & 4;
  entry.options.spirv_stencil_export = flags & 8;
  entry.shader.complete = flags & 16;
  key.remove_prefix(3);
  std::string_view parts[3];
  for (auto& part : parts) {
    const auto end = key.find('\0');
    if (end == std::string_view::npos) return false;
    part = key.substr(0, end);
    key.remove_prefix(end + 1);
  }
  entry.options.spirv_environment = std::string(parts[0]);
  entry.shader.profile = std::string(parts[1]);
  entry.shader.entry_point = std::string(parts[2]);
  entry.shader.hlsl = std::string(key);
  return !entry.shader.hlsl.empty();
}

struct Totals {
  std::size_t shaders{}, failures{};
  double total_ms{}, max_ms{};
  std::uint64_t bytes{};
  std::vector<double> samples;
};

void report(const char* label, Totals& totals) {
  if (!totals.shaders) return;
  std::sort(totals.samples.begin(), totals.samples.end());
  const auto median = totals.samples[totals.samples.size() / 2];
  std::printf("%-18s %5zu shaders  total %8.0f ms  avg %6.1f ms  median %6.1f ms  max %7.1f ms  "
              "%zu failed  %.1f MB\n",
              label, totals.shaders, totals.total_ms, totals.total_ms / double(totals.shaders),
              median, totals.max_ms, totals.failures, double(totals.bytes) / 1048576.0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: shader-bench <shader cache directory> [--limit N] "
                 "[--format dxil|spirv|all]\n");
    return 2;
  }
  const std::filesystem::path directory = argv[1];
  std::size_t limit = 0;
  std::string format = "all";
  bool both_formats = false;
  for (int i = 2; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--both-formats") both_formats = true;
    if (i + 1 >= argc) continue;
    if (argument == "--limit") limit = std::strtoul(argv[++i], nullptr, 10);
    else if (argument == "--format") format = argv[++i];
  }
  std::vector<Entry> entries;
  std::error_code error;
  for (const auto& file : std::filesystem::directory_iterator(directory, error)) {
    if (file.path().extension() != ".xsc") continue;
    Entry entry;
    if (!parse_entry(file.path(), entry)) continue;
    const bool spirv = entry.options.format == ShaderBinaryFormat::Spirv;
    if ((format == "dxil" && spirv) || (format == "spirv" && !spirv)) continue;
    entries.push_back(std::move(entry));
    if (limit && entries.size() >= limit) break;
  }
  std::printf("%zu cached shaders from %s\n", entries.size(), directory.string().c_str());
  xenon::gpu::DxcShaderCompiler compiler;
  Totals totals[2][2];  // [spirv][optimize]
  for (const auto& entry : entries) {
   for (const bool spirv : {false, true}) {
    if (!both_formats && spirv != (entry.options.format == ShaderBinaryFormat::Spirv)) continue;
    for (const bool optimize : {true, false}) {
      auto options = entry.options;
      options.format = spirv ? ShaderBinaryFormat::Spirv : ShaderBinaryFormat::Dxil;
      options.optimize = optimize;
      const auto start = std::chrono::steady_clock::now();
      const auto compiled = compiler.compile(entry.shader, options);
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
      auto& t = totals[spirv][optimize];
      ++t.shaders;
      t.total_ms += ms;
      t.max_ms = std::max(t.max_ms, ms);
      t.samples.push_back(ms);
      if (!compiled.succeeded) ++t.failures;
      t.bytes += compiled.binary.size();
    }
   }
  }
  report("DXIL -O3", totals[0][1]);
  report("DXIL -Od", totals[0][0]);
  report("SPIR-V -O3", totals[1][1]);
  report("SPIR-V -Od", totals[1][0]);
  return 0;
}
