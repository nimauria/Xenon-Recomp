#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"
#include "../verification/generated_fuzz_interface.hpp"
#include "../verification/semantic_corpus.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::verify::test_corpus;

static const OpcodeInfo* find_opcode(std::string_view mnemonic) {
  for (const auto& op : Decoder::opcode_catalog())
    if (op.mnemonic == mnemonic) return &op;
  return nullptr;
}

struct FuzzRng {
  std::uint64_t state{0x47454E3850555A5Aull};
  std::uint64_t next() noexcept {
    auto x = state;
    x ^= x >> 12u; x ^= x << 25u; x ^= x >> 27u; state = x;
    return x * 0x2545F4914F6CDD1Dull;
  }
  std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(next()); }
};

struct GeneratedBlockSpec {
  std::string name;
  std::vector<InstructionSpec> instructions;
  bool memory_setup{};
};

static std::vector<GeneratedBlockSpec> make_fuzz_blocks() {
  FuzzRng rng;
  std::vector<GeneratedBlockSpec> result;
  result.reserve(24);
  for (unsigned index = 0; index < 24; ++index) {
    std::ostringstream name;
    name << "sem_fuzz_" << std::setw(2) << std::setfill('0') << index;
    const auto base = static_cast<GuestAddress>(0xA000u + index * 0x40u);
    GeneratedBlockSpec block{name.str(), {}, false};

    if (index % 6u == 1u) {
      // Memory forwarding block.  Offsets remain within the verifier's
      // normalized memory window but values and surrounding bytes are random.
      const auto off = static_cast<std::uint16_t>(0x80u + (rng.u32() & 0x3Cu));
      block.memory_setup = true;
      block.instructions = {
          {"lwz", base + 0u, (5u << 21) | (3u << 16) | off},
          {"xori", base + 4u, (5u << 21) | (5u << 16) | (rng.u32() & 0xFFFFu)},
          {"addx", base + 8u, (5u << 21) | (5u << 16) | (4u << 11) | (rng.u32() & 1u)},
          {"stw", base + 12u, (5u << 21) | (3u << 16) | static_cast<std::uint16_t>(off + 4u)},
      };
    } else if (index % 6u == 2u) {
      block.instructions = {
          {"vxor", base + 0u, (5u << 21) | (3u << 16) | (4u << 11)},
          {"vor", base + 4u, (6u << 21) | (5u << 16) | (4u << 11)},
          {"vand", base + 8u, (7u << 21) | (6u << 16) | (3u << 11)},
      };
    } else if (index % 6u == 3u) {
      block.instructions = {
          {"fmrx", base + 0u, (5u << 21) | (3u << 11)},
          {"fnegx", base + 4u, (6u << 21) | (5u << 11)},
          {"fabsx", base + 8u, (7u << 21) | (6u << 11)},
      };
    } else {
      const auto imm0 = static_cast<std::uint16_t>(rng.u32());
      const auto imm1 = static_cast<std::uint16_t>(rng.u32());
      block.instructions.push_back({"addi", base + 0u,
                                    (5u << 21) | (3u << 16) | imm0});
      const unsigned length = 3u + (rng.u32() % 4u); // 3..6 instructions.
      for (unsigned slot = 1; slot < length; ++slot) {
        const auto address = static_cast<GuestAddress>(base + slot * 4u);
        switch (rng.u32() % 7u) {
          case 0: block.instructions.push_back({"ori", address, (5u << 21) | (5u << 16) | static_cast<std::uint16_t>(rng.u32())}); break;
          case 1: block.instructions.push_back({"xori", address, (5u << 21) | (5u << 16) | static_cast<std::uint16_t>(rng.u32())}); break;
          case 2: block.instructions.push_back({"addx", address, (5u << 21) | (5u << 16) | (4u << 11) | (rng.u32() & 1u)}); break;
          case 3: block.instructions.push_back({"subfx", address, (5u << 21) | (4u << 16) | (5u << 11) | (rng.u32() & 1u)}); break;
          case 4: block.instructions.push_back({"xorx", address, (5u << 21) | (5u << 16) | (4u << 11) | (rng.u32() & 1u)}); break;
          case 5: block.instructions.push_back({"andx", address, (5u << 21) | (5u << 16) | (4u << 11) | (rng.u32() & 1u)}); break;
          default: block.instructions.push_back({"extswx", address, (5u << 21) | (5u << 16) | (rng.u32() & 1u)}); break;
        }
      }
      // Make one immediate value part of the generated stream even if the RNG
      // chose only register forms after the first operation.
      if ((index & 1u) == 0u && block.instructions.size() > 1)
        block.instructions[1] = {"xori", base + 4u, (5u << 21) | (5u << 16) | imm1};
    }
    result.push_back(std::move(block));
  }
  return result;
}

static std::uint32_t word_for(const InstructionSpec& instruction) {
  const auto* info = find_opcode(instruction.mnemonic);
  if (!info) throw std::runtime_error("missing opcode " + std::string(instruction.mnemonic));
  return info->pattern | instruction.operand_bits;
}

static bool emit_case(std::ostream& out, Decoder& decoder, Lifter& lifter,
                      backend::CppAotBackend& backend, std::string_view name,
                      const std::vector<InstructionSpec>& instructions) {
  ir::Block block{instructions.front().address, {}};
  ir::Builder builder(block);
  for (const auto& instruction : instructions) {
    const auto word = word_for(instruction);
    const auto decoded = decoder.decode(instruction.address, word);
    if (!decoded.valid() || decoded.mnemonic() != instruction.mnemonic) {
      std::cerr << "decode failed for " << name << ": expected "
                << instruction.mnemonic << " got " << decoded.mnemonic() << "\n";
      return false;
    }
    if (!lifter.lift(decoded, builder)) {
      std::cerr << "lift failed for " << name << ": " << instruction.mnemonic << "\n";
      return false;
    }
  }
  block.end_address = static_cast<GuestAddress>(instructions.back().address + 4u);
  out << backend.emit_function(block, name) << "\n";
  return true;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n#include <span>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\n"
         "#include \"generated_fuzz_interface.hpp\"\nusing namespace xenon::cpu;\n";

  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  try {
    for (const auto& spec : cases())
      if (!emit_case(out, decoder, lifter, backend, spec.name, spec.instructions)) return 5;

    const auto fuzz = make_fuzz_blocks();
    for (const auto& spec : fuzz)
      if (!emit_case(out, decoder, lifter, backend, spec.name, spec.instructions)) return 6;

    out << "\nnamespace xenon::cpu::verify::test_corpus {\n";
    for (const auto& spec : fuzz) {
      out << "static constexpr GeneratedFuzzInstruction " << spec.name << "_instructions[] = {";
      for (const auto& instruction : spec.instructions)
        out << "{" << instruction.address << "u," << word_for(instruction) << "u},";
      out << "};\n";
    }
    out << "static const GeneratedFuzzCase kGeneratedFuzzCases[] = {\n";
    for (const auto& spec : fuzz)
      out << "{\"" << spec.name << "\"," << spec.name << "_instructions,"
          << spec.instructions.size() << "u,&::" << spec.name << ","
          << (spec.memory_setup ? "true" : "false") << "},\n";
    out << "};\nstd::span<const GeneratedFuzzCase> generated_semantic_fuzz_cases() noexcept { return kGeneratedFuzzCases; }\n"
           "} // namespace xenon::cpu::verify::test_corpus\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 7;
  }
  return 0;
}
