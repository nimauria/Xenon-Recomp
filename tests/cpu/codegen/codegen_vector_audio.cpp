#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const struct { const char* name; std::uint32_t word; } cases[] = {
      {"vnmsubfp128", 0x14000150u | (1u << 21) | (2u << 16) | (3u << 11)},
      {"vnmsubfp", 0x1000002Fu | (1u << 21) | (2u << 16) | (3u << 11) | (4u << 6)},
      {"vcfsx", 0x1000034Au | (1u << 21) | (15u << 16) | (3u << 11)},
      {"vupkhsh", 0x1000024Eu | (1u << 21) | (3u << 11)},
      {"vupklsh", 0x100002CEu | (1u << 21) | (3u << 11)},
      {"vrefp", 0x1000010Au | (1u << 21) | (3u << 11)},
      {"vspltw", 0x1000028Cu | (1u << 21) | (1u << 16) | (3u << 11)},
      {"vspltw128", 0x18000730u | (1u << 21) | (1u << 16) | (3u << 11)},
      {"vpermwi128", 0x18000210u | (1u << 21) | (3u << 11) | (4u << 16) | (7u << 6)},
  };
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  for (const auto& c : cases) {
    const auto decoded = decoder.decode(0x6000u, c.word);
    if (!decoded.valid() || decoded.mnemonic() != c.name) {
      std::cerr << c.name << " decoded as " << decoded.mnemonic() << '\n';
      return 4;
    }
    ir::Block block{0x6000u, {}};
    ir::Builder builder(block);
    if (!lifter.lift(decoded, builder)) return 5;
    out << backend.emit_function(block, std::string("audio_") + c.name) << '\n';
  }
  out << "struct AudioVectorCase { const char* name; std::uint32_t word;"
         " ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&); };\n"
         "extern const AudioVectorCase kAudioVectorCases[] = {\n";
  for (const auto& c : cases)
    out << "{\"" << c.name << "\", " << c.word << "u, &audio_" << c.name << "},\n";
  out << "};\nextern const unsigned kAudioVectorCaseCount = " << std::size(cases) << "u;\n";
}
