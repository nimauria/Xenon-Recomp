// Regression tests for the generated-code deduplication / shard ownership
// fix. These exercise the actual production bug that made a real title's
// (Ace Combat 6) generated module fail to compile with MSVC C2084 "function
// already has a body" errors: generate_project()'s per-function codegen
// cache was keyed ONLY by a content hash of guest instruction bytes, with no
// dependency on which guest address (and therefore which generated C++
// name) those bytes belonged to. Two different, unrelated functions with
// byte-identical machine code - extremely common for trivial stub bodies
// such as a bare `blr` across a real title's tens of thousands of functions
// - collided on that hash: whichever function reached the shared cache path
// first "won", and every other colliding function silently inherited the
// first function's own emitted text (defining the first function's symbols
// a second time under a different shard slot) instead of its own, while its
// own symbols were never emitted at all despite registry.cpp still
// declaring and referencing them.
//
// Uses the same small hand-built synthetic XEX technique as
// tests/recomp/recomp_driver_tests.cpp and
// tests/recomp/analysis_v2_consumption_tests.cpp.

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"
#include "xenon/recomp/compilation_graph.hpp"
#include "xenon/recomp/worker_pool.hpp"
#include <array>
#include <chrono>

using namespace xenon::recomp;
using namespace xenon::recomp::analysis;
namespace xbox = xenon::xbox;

namespace {

void be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 24);
  bytes[offset + 1] = static_cast<std::byte>(value >> 16);
  bytes[offset + 2] = static_cast<std::byte>(value >> 8);
  bytes[offset + 3] = static_cast<std::byte>(value);
}

void le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8);
  bytes[offset + 2] = static_cast<std::byte>(value >> 16);
  bytes[offset + 3] = static_cast<std::byte>(value >> 24);
}

constexpr std::uint32_t kLoadAddress = 0x80000000u;
constexpr std::uint32_t kTextRva = 0x10000u;
constexpr std::uint32_t kDataRva = 0x20000u;

std::vector<std::byte> make_xex(const std::vector<std::uint32_t>& text_words, std::size_t data_size) {
  constexpr std::size_t header = 0x300;
  constexpr std::size_t security = 0x20;
  constexpr std::size_t pe = 0x380;
  constexpr std::size_t coff = pe + 4;
  constexpr std::size_t optional = coff + 20;
  constexpr std::size_t section = optional + 0xE0;
  constexpr std::size_t text_raw = kTextRva;
  const std::size_t data_raw = kDataRva;
  const std::size_t file_size = header + data_raw + std::max<std::size_t>(data_size, 0x10);

  std::vector<std::byte> bytes(file_size, std::byte{0});
  bytes[0] = std::byte{'X'}; bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'X'}; bytes[3] = std::byte{'2'};
  be32(bytes, 8, static_cast<std::uint32_t>(header));
  be32(bytes, 0x10, static_cast<std::uint32_t>(security));
  be32(bytes, 0x14, 0);

  be32(bytes, security + 0x000, 0x184);
  be32(bytes, security + 0x004, static_cast<std::uint32_t>(bytes.size() - header));
  be32(bytes, security + 0x110, kLoadAddress);

  bytes[header] = std::byte{'M'}; bytes[header + 1] = std::byte{'Z'};
  le32(bytes, header + 0x3C, static_cast<std::uint32_t>(pe - header));
  bytes[pe] = std::byte{'P'}; bytes[pe + 1] = std::byte{'E'};
  le16(bytes, coff, 0x14C);
  le16(bytes, coff + 2, 2);
  le16(bytes, coff + 0x10, 0xE0);
  le16(bytes, coff + 0x12, 0x0102);
  le16(bytes, optional, 0x10B);
  le32(bytes, optional + 0x10, kTextRva);
  le32(bytes, optional + 0x1C, kLoadAddress);
  le32(bytes, optional + 0x38, kDataRva + 0x10000u);

  bytes[section + 0] = std::byte{'.'}; bytes[section + 1] = std::byte{'t'};
  bytes[section + 2] = std::byte{'e'}; bytes[section + 3] = std::byte{'x'};
  bytes[section + 4] = std::byte{'t'};
  const auto text_size = static_cast<std::uint32_t>(text_words.size() * 4u + 0x40u);
  le32(bytes, section + 4, text_size);
  le32(bytes, section + 0xC, kTextRva);
  le32(bytes, section + 0x10, text_size);
  le32(bytes, section + 0x14, static_cast<std::uint32_t>(text_raw));
  le32(bytes, section + 0x24, 0x60000020);

  const std::size_t data_section = section + 0x28;
  bytes[data_section + 0] = std::byte{'.'}; bytes[data_section + 1] = std::byte{'d'};
  bytes[data_section + 2] = std::byte{'a'}; bytes[data_section + 3] = std::byte{'t'};
  bytes[data_section + 4] = std::byte{'a'};
  const auto data_section_size = static_cast<std::uint32_t>(std::max<std::size_t>(data_size, 0x10));
  le32(bytes, data_section + 4, data_section_size);
  le32(bytes, data_section + 0xC, kDataRva);
  le32(bytes, data_section + 0x10, data_section_size);
  le32(bytes, data_section + 0x14, static_cast<std::uint32_t>(data_raw));
  le32(bytes, data_section + 0x24, 0xC0000040);

  const std::size_t text_file_base = header + text_raw;
  for (std::size_t i = 0; i < text_words.size(); ++i) be32(bytes, text_file_base + i * 4u, text_words[i]);

  return bytes;
}

constexpr std::uint32_t kBlr = 0x4E800020u;

// I-form branch (opcode 18): `offset` is the byte displacement from this
// instruction's own address to its target; `link` sets LK (a `bl`).
std::uint32_t branch_word(std::int32_t offset, bool link) {
  return 0x48000000u | (static_cast<std::uint32_t>(offset) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

std::string hex_of(std::uint32_t address) {
  std::ostringstream out;
  out << std::hex << address;
  return out.str();
}

std::size_t occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0, pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
  return count;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string read_all_shards(const std::filesystem::path& output) {
  std::string text;
  for (const auto& entry : std::filesystem::directory_iterator(output / "functions")) {
    if (entry.path().extension() != ".cpp") continue;
    std::ifstream source(entry.path());
    text.append(std::istreambuf_iterator<char>(source), {});
  }
  return text;
}

// Sets (or, for nullptr, removes) an environment variable for this process.
void set_environment(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value) setenv(name, value, 1);
  else unsetenv(name);
#endif
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path input;
  std::filesystem::path output;
};

Fixture make_fixture(const std::string& name, const std::vector<std::byte>& xex_bytes) {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() / ("xenon_graph_" + name);
  std::filesystem::remove_all(fixture.root);
  std::filesystem::create_directories(fixture.root);
  fixture.input = fixture.root / "fixture.xex";
  fixture.output = fixture.root / "generated";
  std::ofstream(fixture.input, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex_bytes.data()), static_cast<std::streamsize>(xex_bytes.size()));
  return fixture;
}

// Regression test for shard_max_bytes (byte-size shard budget, an
// alternative to the fixed-function-count default that produced wildly
// uneven shards on a real title - see gracemeria_recomp.cpp). Self-
// calibrates against the actual measured per-function generated size rather
// than a hardcoded byte count, so it stays correct if the C++ AOT backend's
// emitted boilerplate ever changes size.
void test_shard_byte_budget() {
  const auto xex = make_xex(
      {branch_word(12, true), branch_word(16, true), kBlr, 0x38600001, kBlr, 0x38600002, kBlr}, 0x40);

  // Baseline: one function per shard tells us each function's own generated
  // size with no bundling involved.
  const auto baseline = make_fixture("shard_budget_baseline", xex);
  DriverOptions baseline_options;
  baseline_options.input = baseline.input;
  baseline_options.output = baseline.output;
  baseline_options.graph_cache = baseline.root / "cas";
  baseline_options.shard_function_count = 1;
  baseline_options.analysis_jobs = 1;
  baseline_options.codegen_jobs = 1;
  std::string error;
  AnalysisReport baseline_report;
  assert(load_and_analyze(baseline_options, baseline_report, error));
  assert(baseline_report.functions.size() == 3);
  assert(generate_project(baseline_options, baseline_report, error));
  std::vector<std::size_t> sizes;
  for (const auto& entry : std::filesystem::directory_iterator(baseline.output / "functions")) {
    if (entry.path().extension() == ".cpp") sizes.push_back(std::filesystem::file_size(entry.path()));
  }
  assert(sizes.size() == 3);
  std::sort(sizes.begin(), sizes.end());
  const std::string header = "// Generated by xenon codegen. Do not edit.\n";
  const auto header_size = header.size();
  // Each per-function shard is exactly one function's source plus the header.
  const auto smallest_function = sizes[0] - header_size;
  const auto two_smallest_functions = (sizes[0] - header_size) + (sizes[1] - header_size);
  // Order-independent content check: strip every shard header, then compare
  // as a sorted character multiset - directory_iterator's per-directory file
  // order isn't guaranteed to match processing order once file *counts*
  // differ between two runs, so byte-for-byte concatenation order cannot be
  // relied on here (unlike the same-file-set re-read a few lines above).
  const auto normalize = [&](std::string text) {
    for (std::size_t pos; (pos = text.find(header)) != std::string::npos;) text.erase(pos, header.size());
    std::sort(text.begin(), text.end());
    return text;
  };

  // Budget between the two smallest functions' combined size and adding the
  // third: the two smallest should bundle into one shard, the third gets a
  // shard of its own - genuine byte-based bundling, not just 1-per-function.
  const auto bundling = make_fixture("shard_budget_bundling", xex);
  DriverOptions bundling_options = baseline_options;
  bundling_options.input = bundling.input;
  bundling_options.output = bundling.output;
  bundling_options.graph_cache = bundling.root / "cas";
  bundling_options.shard_max_bytes = two_smallest_functions + header_size + 1;
  AnalysisReport bundling_report;
  assert(load_and_analyze(bundling_options, bundling_report, error));
  assert(generate_project(bundling_options, bundling_report, error));
  std::size_t bundling_shard_count = 0;
  std::size_t bundled_shard_functions = 0;
  for (const auto& entry : std::filesystem::directory_iterator(bundling.output / "functions")) {
    if (entry.path().extension() != ".cpp") continue;
    ++bundling_shard_count;
    if (occurrences(read_file(entry.path()), "static ExecutionResult ") > 1) ++bundled_shard_functions;
  }
  assert(bundling_shard_count == 2);
  assert(bundled_shard_functions == 1);
  // No function's own generated text is lost or duplicated by bundling.
  assert(normalize(read_all_shards(bundling.output)) == normalize(read_all_shards(baseline.output)));

  // Budget smaller than even the smallest function: every function still
  // gets its own (oversized-relative-to-budget) shard rather than an empty
  // shard, an infinite loop, or a silently-dropped function.
  const auto tiny = make_fixture("shard_budget_tiny", xex);
  DriverOptions tiny_options = baseline_options;
  tiny_options.input = tiny.input;
  tiny_options.output = tiny.output;
  tiny_options.graph_cache = tiny.root / "cas";
  tiny_options.shard_max_bytes = smallest_function > 8 ? smallest_function - 8 : 1;
  AnalysisReport tiny_report;
  assert(load_and_analyze(tiny_options, tiny_report, error));
  assert(generate_project(tiny_options, tiny_report, error));
  std::size_t tiny_shard_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(tiny.output / "functions")) {
    if (entry.path().extension() == ".cpp") ++tiny_shard_count;
  }
  assert(tiny_shard_count == 3);
  assert(normalize(read_all_shards(tiny.output)) == normalize(read_all_shards(baseline.output)));
  assert(tiny_report.diagnostics.codegen_max_shard_bytes > 0);

  std::cout << "Shard byte-budget bundling/oversized-function tests passed\n";
}

}  // namespace


int main() {
  namespace g = xenon::recomp::graph;
  assert(g::digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(g::digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  {
    const auto root = std::filesystem::temp_directory_path() / "xenon_preparation_identity_inputs";
    std::filesystem::remove_all(root);
    const auto source = root / "source";
    const auto toolchain = root / "toolchain";
    for (const auto* dir : {"include", "src", "cmake", "tools"})
      std::filesystem::create_directories(source / dir);
    std::filesystem::create_directories(toolchain);
    std::ofstream(source / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.25)\n";
    std::ofstream(source / "tools/compilation_cache.py") << "# fixture\n";
    const auto cmake = toolchain / "cmake";
    const auto compiler = toolchain / "compiler";
    const auto unrelated = toolchain / "unrelated-tool";
    std::ofstream(cmake) << "cmake v1\n";
    std::ofstream(compiler) << "compiler v1\n";
    std::ofstream(unrelated) << "unrelated v1\n";
    const auto first = g::preparation_identity(source, cmake, compiler);
    std::ofstream(unrelated) << "unrelated v2\n";
    assert(g::preparation_identity(source, cmake, compiler) == first);
    std::ofstream(compiler) << "compiler v2\n";
    assert(g::preparation_identity(source, cmake, compiler) != first);

    // Header/library trees named by INCLUDE and LIB are fingerprinted by
    // path, size and last-write time, not read: under an MSVC environment
    // they hold gigabytes, and hashing their contents exceeded the CTest
    // timeout on Windows CI.
    const auto* saved_include_env = std::getenv("INCLUDE");
    const std::string saved_include = saved_include_env ? saved_include_env : "";
    const auto sdk = root / "sdk include";
    std::filesystem::create_directories(sdk / "nested");
    const auto header_path = sdk / "nested" / "sdk.h";
    std::ofstream(header_path) << "#define SDK 1\n";
    set_environment("INCLUDE", (sdk.string() + ";").c_str());
    const auto with_sdk = g::preparation_identity(source, cmake, compiler);
    assert(g::preparation_identity(source, cmake, compiler) == with_sdk);
    const auto stamp = std::filesystem::last_write_time(header_path);
    std::ofstream(header_path) << "#define SDK 22\n";  // size changes
    std::filesystem::last_write_time(header_path, stamp);
    const auto resized = g::preparation_identity(source, cmake, compiler);
    assert(resized != with_sdk);
    std::filesystem::last_write_time(header_path, stamp + std::chrono::seconds(10));
    assert(g::preparation_identity(source, cmake, compiler) != resized);
    std::ofstream(sdk / "added.h") << "\n";
    const auto with_added = g::preparation_identity(source, cmake, compiler);
    assert(with_added != resized);
#if !defined(_WIN32)
    // An unreadable SDK file is still fingerprinted, which shows contents are
    // never read. Skipped when the process can read it anyway (root).
    std::filesystem::permissions(sdk / "added.h", std::filesystem::perms::none);
    if (!std::ifstream(sdk / "added.h")) {
      assert(g::preparation_identity(source, cmake, compiler) == with_added);
    }
    std::filesystem::permissions(sdk / "added.h", std::filesystem::perms::owner_all);
#endif
    set_environment("INCLUDE", saved_include_env ? saved_include.c_str() : nullptr);
    std::filesystem::remove_all(root);
  }
  const auto fixture = make_fixture("incremental",make_xex({branch_word(12,true),branch_word(16,true),kBlr,0x38600001,kBlr,0x38600002,kBlr},0x40));
  g::Store store(fixture.root/"cas");
  g::Node n{"source","test",{{"a","b"}}, {}};
  assert(!store.lookup(n));
  auto result=store.publish(n,"payload");assert(!result.hit);
  assert(store.lookup(n)->bytes=="payload");
  std::ofstream(store.root()/"nodes"/n.key()/"payload")<<"corrupt";
  assert(!store.lookup(n));
  assert(store.publish(n,"payload").bytes=="payload");
  // An abandoned private stage is never a valid entry.
  std::filesystem::create_directories(store.root()/"staging"/"incomplete");
  g::Node incomplete{"ir","test",{{"partial","yes"}}, {}};
  std::ofstream(store.root()/"staging"/"incomplete"/"payload")<<"partial";
  assert(!store.lookup(incomplete));
  // Concurrent same-key promotion produces one complete verified entry.
  g::Node shared{"source","test",{{"race","same"}}, {}};
  WorkerPool(8).parallel_for(24,[&](std::size_t){ assert(store.publish(shared,"same bytes").bytes=="same bytes"); });
  assert(store.lookup(shared)->bytes=="same bytes");

  DriverOptions options;options.input=fixture.input;options.output=fixture.output;
  options.graph_cache=store.root();options.shard_function_count=1;options.analysis_jobs=1;options.codegen_jobs=1;
  std::string progress,error;
  options.progress=[&](const std::string& s){progress+=s+"\n";};
  AnalysisReport first;assert(load_and_analyze(options,first,error));assert(first.functions.size()==3);
  for(auto& f:first.functions) assert(!f.ir_cache_hit);
  assert(generate_project(options,first,error));
  auto manifest=read_file(options.output/"compilation-graph.json");auto sources=read_all_shards(options.output);
  auto registry_time=std::filesystem::last_write_time(options.output/"registry.cpp");
  options.analysis_jobs=4;options.codegen_jobs=4;
  AnalysisReport second;assert(load_and_analyze(options,second,error));
  for(auto& f:second.functions) assert(f.ir_cache_hit);
  progress.clear();assert(generate_project(options,second,error));
  assert(progress.find("source hits=3 misses=0")!=std::string::npos);
  assert(read_file(options.output/"compilation-graph.json")==manifest);
  assert(read_all_shards(options.output)==sources);
  assert(std::filesystem::last_write_time(options.output/"registry.cpp")==registry_time);
  // A different revision/title container with identical region bytes reuses IR.
  auto changed=make_xex({branch_word(12,true),branch_word(16,true),kBlr,0x38600009,kBlr,0x38600002,kBlr},0x60);
  std::ofstream(fixture.input,std::ios::binary).write(reinterpret_cast<const char*>(changed.data()),changed.size());
  AnalysisReport third;assert(load_and_analyze(options,third,error));
  assert(std::count_if(third.functions.begin(),third.functions.end(),[](auto& f){return f.ir_cache_hit;})==2);
  progress.clear();assert(generate_project(options,third,error));assert(progress.find("source hits=2 misses=1")!=std::string::npos);
  // Unrelated knowledge changes the global config, but cannot invalidate exact IR/source.
  KnowledgeRecord record; record.id="unrelated"; record.label="metadata"; options.knowledge_records.push_back(record);
  AnalysisReport knowledge;assert(load_and_analyze(options,knowledge,error));
  for(auto& f:knowledge.functions)assert(f.ir_cache_hit);
  progress.clear();assert(generate_project(options,knowledge,error));assert(progress.find("source hits=3 misses=0")!=std::string::npos);
  options.graph_versions.codegen="changed-codegen-abi";
  progress.clear();assert(generate_project(options,knowledge,error));assert(progress.find("source hits=0 misses=3")!=std::string::npos);
  options.graph_versions.semantics="changed-cpu-abi";
  AnalysisReport cpu;assert(load_and_analyze(options,cpu,error));for(auto& f:cpu.functions)assert(!f.ir_cache_hit);
  // Roundtrip every IR field; truncated and out-of-schema data is refused.
  auto encoded=g::serialize_ir(cpu.functions.front().ir);xenon::cpu::ir::Function decoded;
  assert(g::deserialize_ir(encoded,decoded));assert(g::serialize_ir(decoded)==encoded);
  assert(!g::deserialize_ir(encoded.substr(0,encoded.size()-1),decoded));
  // Address-bearing native symbols prohibit normalized-fingerprint-only reuse.
  std::array<std::uint32_t,1> words{kBlr};std::array<xenon::cpu::FunctionCodeRange,1> ranges{{{0x80010000,words}}};
  auto a=g::compile_region(store,{},0x80010000,ranges);ranges[0].base+=0x100;
  auto b=g::compile_region(store,{},0x80010100,ranges);assert(a.nodes.back().key()!=b.nodes.back().key());
  test_shard_byte_budget();
  std::cout<<"Gen 10 graph integration passed; native fixture: "<<fixture.output<<"\n";
}
