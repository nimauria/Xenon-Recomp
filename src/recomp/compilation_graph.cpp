#include "xenon/recomp/compilation_graph.hpp"
#include "xenon/core/json.hpp"
#include "xenon/cpu/ir_verifier.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>

namespace xenon::recomp::graph {
namespace {
using Json = core::JsonValue;
std::string read(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void write(const std::filesystem::path& p, std::string_view data) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size())); f.close();
  if (!f) throw std::runtime_error("graph cache write failed: " + p.string());
}
void number(std::string& s, std::uint64_t v) {
  for (unsigned i=0;i<8;++i) s.push_back(static_cast<char>(v >> (i*8)));
}
struct Reader {
  std::string_view s; std::size_t pos{};
  std::uint64_t get() {
    if (s.size()-pos<8) throw std::runtime_error("truncated IR");
    std::uint64_t n=0; for(unsigned i=0;i<8;++i) n|=std::uint64_t(static_cast<unsigned char>(s[pos++]))<<(i*8);
    return n;
  }
  std::size_t count() { auto n=get(); if(n>s.size()/8) throw std::runtime_error("invalid IR count"); return static_cast<std::size_t>(n); }
};
}
std::string digest(std::string_view bytes) {
  constexpr std::array<std::uint32_t,64> k={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  std::array<std::uint32_t,8> h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::string data(bytes); data.push_back(static_cast<char>(0x80));
  while(data.size()%64!=56) data.push_back(0);
  auto bits=std::uint64_t(bytes.size())*8;
  for(int i=7;i>=0;--i) data.push_back(static_cast<char>(bits>>(i*8)));
  for(std::size_t off=0;off<data.size();off+=64) {
    std::array<std::uint32_t,64> w{};
    for(unsigned i=0;i<16;++i) for(unsigned j=0;j<4;++j) w[i]=(w[i]<<8)|static_cast<unsigned char>(data[off+i*4+j]);
    for(unsigned i=16;i<64;++i) {
      auto a=w[i-15],b=w[i-2];
      w[i]=w[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+w[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));
    }
    auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
    for(unsigned i=0;i<64;++i) {
      auto t1=z+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
      auto t2=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
      z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
  }
  std::ostringstream out; out<<std::hex<<std::setfill('0'); for(auto v:h) out<<std::setw(8)<<v; return out.str();
}
std::string producer_identity(std::string_view stage) {
  return stage == "source" ? XENON_GRAPH_CODEGEN_PRODUCER : XENON_GRAPH_IR_PRODUCER;
}
std::string preparation_identity(const std::filesystem::path& source_root,
                                 const std::filesystem::path& cmake_command,
                                 const std::filesystem::path& native_compiler) {
  std::map<std::string, std::string> files;
  const auto add_file = [&](const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot fingerprint build input: " + p.string());
    const std::string bytes((std::istreambuf_iterator<char>(f)), {});
    files[p.generic_string()] = digest(bytes);
  };
  const auto add_tree = [&](const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return;
    for (const auto& e : std::filesystem::recursive_directory_iterator(p))
      if (e.is_regular_file()) add_file(e.path());
  };
  for (const auto* dir : {"include", "src", "cmake"}) add_tree(source_root / dir);
  add_file(source_root / "CMakeLists.txt");
  add_file(source_root / "tools/compilation_cache.py");
  add_file(cmake_command);
  add_tree(native_compiler.parent_path());
  // System headers and import libraries are semantic toolchain inputs too.
  for (const auto* name : {"INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "CXXFLAGS", "LDFLAGS", "PATH"}) {
    const auto* value = std::getenv(name);
    files[std::string("env:") + name] = value ? value : "";
    if (value && (std::string_view(name) == "INCLUDE" || std::string_view(name) == "LIB")) {
      std::istringstream paths(value);
      std::string path;
      while (std::getline(paths, path, ';')) if (!path.empty()) add_tree(path);
    }
  }
  Node node{"prepared-environment", "prepare-10", std::move(files), {}};
  return node.key();
}
std::string Node::canonical() const {
  Json j=Json::make_object(), in=Json::make_object(), deps=Json::make_array();
  for(const auto& [k,v]:inputs) in.set(k,v);
  for(const auto& d:dependencies) deps.append(d);
  j.set("schema",1); j.set("stage",stage); j.set("producer",producer); j.set("inputs",in); j.set("dependencies",deps);
  return j.dump();
}
std::string Node::key() const { return digest(canonical()); }
std::optional<Result> Store::lookup(const Node& node) const {
  const auto dir=root_/"nodes"/node.key();
  auto manifest=read(dir/"manifest.json");
  if(manifest.empty()) return {};
  Json j; std::string error;
  if(!Json::parse(manifest,j,&error) || j.get_string("request")!=node.canonical() || !j.get_bool("verified")) return {};
  auto bytes=read(dir/"payload");
  if(!std::filesystem::is_regular_file(dir/"payload") || j.get_string("sha256")!=digest(bytes)) return {};
  return Result{std::move(bytes),j.get_string("sha256"),true};
}
Result Store::publish(const Node& node, std::string bytes) const {
  if(auto hit=lookup(node)) return *hit;
  auto hash=digest(bytes); const auto final=root_/"nodes"/node.key();
  std::filesystem::create_directories(root_/"staging"); std::filesystem::create_directories(root_/"nodes");
  static std::atomic<std::uint64_t> serial{};
  std::random_device rng;
  auto stage=root_/"staging"/(node.key()+"-"+std::to_string(rng())+"-"+std::to_string(serial++));
  if(!std::filesystem::create_directory(stage)) throw std::runtime_error("graph staging collision");
  try {
    write(stage/"payload",bytes);
    if(read(stage/"payload")!=bytes) throw std::runtime_error("graph staging verification failed");
    Json j=Json::make_object(); j.set("request",node.canonical()); j.set("sha256",hash); j.set("verified",true);
    write(stage/"manifest.json",j.dump());
    std::error_code ec; std::filesystem::rename(stage,final,ec);
    if(ec) {
      if(auto hit=lookup(node)) { std::filesystem::remove_all(stage); return *hit; }
      // Never replace a corrupt entry in place: readers must not observe a
      // partially repaired entry. Quarantine it, then try atomic promotion.
      auto quarantine=root_/"staging"/(stage.filename().string()+"-corrupt");
      std::filesystem::rename(final,quarantine,ec);
      ec.clear(); std::filesystem::rename(stage,final,ec);
      if(ec) { if(auto hit=lookup(node)) { std::filesystem::remove_all(stage); return *hit; } throw std::runtime_error("graph promotion failed: "+ec.message()); }
    }
    return {std::move(bytes),std::move(hash),false};
  } catch(...) { std::error_code ec; std::filesystem::remove_all(stage,ec); throw; }
}
std::string serialize_ir(const cpu::ir::Function& fn) {
  std::string s; number(s,1);number(s,fn.guest_address);number(s,fn.blocks.size());
  for(const auto& b:fn.blocks) {
    number(s,b.guest_address);number(s,b.end_address);number(s,b.has_external_exit);number(s,b.has_indirect_exit);number(s,b.has_indirect_call);
    number(s,b.successors.size());for(auto e:b.successors){number(s,e.target);number(s,static_cast<unsigned>(e.kind));number(s,e.local);}
    number(s,b.predecessors.size());for(auto p:b.predecessors)number(s,p);
    number(s,b.instructions.size());for(const auto& i:b.instructions){
      number(s,static_cast<unsigned>(i.op));number(s,static_cast<unsigned>(i.type));number(s,i.result);number(s,i.imm0);number(s,i.imm1);
      number(s,i.guest_address);number(s,i.guest_word);number(s,i.guest_opcode.value);number(s,i.args.size());for(auto a:i.args)number(s,a);
    }
  }
  return s;
}
bool deserialize_ir(std::string_view bytes,cpu::ir::Function& fn) {
  try {
    Reader r{bytes};if(r.get()!=1)return false;cpu::ir::Function out;out.guest_address=static_cast<cpu::GuestAddress>(r.get());out.blocks.resize(r.count());
    for(auto& b:out.blocks){
      b.guest_address=static_cast<cpu::GuestAddress>(r.get());b.end_address=static_cast<cpu::GuestAddress>(r.get());b.has_external_exit=r.get()!=0;b.has_indirect_exit=r.get()!=0;b.has_indirect_call=r.get()!=0;
      b.successors.resize(r.count());for(auto& e:b.successors){e.target=static_cast<cpu::GuestAddress>(r.get());auto kind=r.get();if(kind>2)return false;e.kind=static_cast<cpu::ir::EdgeKind>(kind);e.local=r.get()!=0;}
      b.predecessors.resize(r.count());for(auto& p:b.predecessors)p=static_cast<cpu::GuestAddress>(r.get());
      b.instructions.resize(r.count());for(auto& i:b.instructions){
        auto op=r.get(),type=r.get();if(op>static_cast<unsigned>(cpu::ir::Op::Syscall)||type>static_cast<unsigned>(cpu::ir::Type::V128))return false;
        i.op=static_cast<cpu::ir::Op>(op);i.type=static_cast<cpu::ir::Type>(type);i.result=static_cast<cpu::ir::ValueId>(r.get());i.imm0=r.get();i.imm1=r.get();
        i.guest_address=static_cast<cpu::GuestAddress>(r.get());i.guest_word=static_cast<std::uint32_t>(r.get());i.guest_opcode=cpu::OpcodeId{static_cast<std::uint32_t>(r.get())};
        auto n=r.count();if(n>4)return false;std::vector<cpu::ir::ValueId> args(n);for(auto& a:args)a=static_cast<cpu::ir::ValueId>(r.get());i.args.assign(args);
      }
    }
    if(r.pos!=bytes.size() || serialize_ir(out)!=bytes || !cpu::ir::Verifier{}.verify(out).ok)return false;
    fn=std::move(out);return true;
  }catch(...){return false;}
}
RegionResult compile_region(const Store& store,const Versions& v,cpu::GuestAddress entry,std::span<const cpu::FunctionCodeRange> ranges){
  std::string raw;number(raw,entry);number(raw,ranges.size());
  for(auto r:ranges){number(raw,r.base);number(raw,r.words.size());for(auto w:r.words)number(raw,w);}
  RegionResult result;
  Node decoded{"decoded-region",v.decoder,{{"guest_ranges_sha256",digest(raw)}},{}};
  // The range boundary is the analysis output contract: title/DB identity is
  // provenance, not a semantic dependency of the range compiler.
  Node analysis{"analysis-cfg",v.analysis,{{"owned_ranges",digest(raw)}},{decoded.key()}};
  Node ir{"ir",v.ir,{{"cpu_semantics",v.semantics},{"producer_build",producer_identity("ir")}},{analysis.key()}};
  result.nodes={decoded,analysis,ir};
  if(auto cached=store.lookup(ir);cached && deserialize_ir(cached->bytes,result.compiled.function)) {result.compiled.ok=true;result.ir_hit=true;return result;}
  result.compiled=cpu::StaticFunctionCompiler{}.compile_ranges(entry,ranges);
  if(result.compiled.ok){
    (void)store.publish(decoded,raw);(void)store.publish(analysis,raw);
    (void)store.publish(ir,serialize_ir(result.compiled.function));
  }
  return result;
}
} // namespace xenon::recomp::graph
