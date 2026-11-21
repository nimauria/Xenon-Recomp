// Generates a guest function that calls through a register (`mtctr r3;
// bctrl; blr`) and one that tail-branches through it (`mtctr r3; bctr`), for
// x86_64/indirect_cache.cpp. Each is emitted with the per-site target cache
// (promoted and not) and, as the control, without it. The cached sources must
// resolve through aot::lookup_cached and the uncached one must not.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/function_compiler.hpp"

using namespace xenon::cpu;

namespace {

bool compile(GuestAddress base, const std::vector<std::uint32_t>& words, ir::Function& out) {
  StaticFunctionCompiler compiler;
  const FunctionCodeRange range{base, words};
  auto result = compiler.compile_ranges(base, std::span(&range, 1u), {}, {});
  if (!result.ok) {
    std::cerr << "compile failed: " << result.error << "\n";
    return false;
  }
  out = std::move(result.function);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  ir::Function call, tail;
  if (!compile(0x10000u, {0x7C6903A6u /*mtctr r3*/, 0x4E800421u /*bctrl*/, 0x4E800020u /*blr*/}, call) ||
      !compile(0x11000u, {0x7C6903A6u /*mtctr r3*/, 0x4E800420u /*bctr*/}, tail))
    return 4;
  struct Variant {
    const char* suffix;
    bool promote;
    bool cache;
  };
  const std::vector<Variant> variants = {
      {"_promoted", true, true}, {"_plain", false, true}, {"_uncached", true, false}};
  const std::vector<std::pair<std::string, const ir::Function*>> functions = {{"ic_call", &call},
                                                                              {"ic_tail", &tail}};
  for (const auto& v : variants) {
    const backend::CppAotBackend backend(v.promote, v.cache);
    for (const auto& [name, function] : functions) {
      const auto source = backend.emit_function(*function, name + v.suffix);
      const bool cached = source.find("aot::lookup_cached") != std::string::npos &&
                          source.find("static thread_local aot::IndirectSite") != std::string::npos;
      const bool plain_lookup = source.find("context.lookup_compiled(") != std::string::npos;
      if (cached != v.cache || plain_lookup == v.cache) {
        std::cerr << name << v.suffix << ": unexpected lookup form (cached=" << cached
                  << ", plain lookup=" << plain_lookup << ")\n";
        return 5;
      }
      out << source << "\n";
    }
  }
  return 0;
}
