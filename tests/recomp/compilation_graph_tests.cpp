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

}  // namespace


int main() {
  namespace g = xenon::recomp::graph;
  assert(g::digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(g::digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
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
  std::cout<<"Gen 10 graph integration passed; native fixture: "<<fixture.output<<"\n";
}
