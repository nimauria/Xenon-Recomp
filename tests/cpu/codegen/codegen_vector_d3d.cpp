// Generates real AOT code for VMX128 vpkd3d128/vupkd3d128 encodings, with the
// xenia-project/xenia PPC hardware test vectors (instr_vpkd3d128.s,
// instr_vupkd3d128.s) as expected results, for x86_64/vector_d3d.cpp.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/lifter.hpp"

using namespace xenon::cpu;

namespace {

constexpr std::uint32_t CD = 0xCDCDCDCDu;

struct Case {
  const char* name;
  const char* mnemonic;
  std::uint32_t word;
  unsigned src, dst;
  std::uint32_t src_in[4];
  std::uint32_t out[4];  // vD afterwards; it starts as CDCDCDCD in every word.
};

// Unpack: vupkd3d128 v3, v3, type. Pack: vpkd3d128 v4, v3, type, mask, shift.
constexpr std::uint32_t kPack[4] = {0x40400001u, 0x40400002u, 0x40400003u, 0x40400004u};
const Case kCases[] = {
    {"unpack_d3dcolor", "vupkd3d128", 0x18601FF0u, 3, 3, {CD, CD, CD, 0x04010203u},
     {0x3F800001u, 0x3F800002u, 0x3F800003u, 0x3F800004u}},
    {"unpack_short2_0", "vupkd3d128", 0x18641FF0u, 3, 3, {CD, CD, CD, 0x7FFF8001u},
     {0x40407FFFu, 0x403F8001u, 0u, 0x3F800000u}},
    {"unpack_short2_1", "vupkd3d128", 0x18641FF0u, 3, 3, {CD, CD, CD, 0x4000C000u},
     {0x40404000u, 0x403FC000u, 0u, 0x3F800000u}},
    {"unpack_short2_2", "vupkd3d128", 0x18641FF0u, 3, 3, {CD, CD, CD, 0x7FFFF333u},
     {0x40407FFFu, 0x403FF333u, 0u, 0x3F800000u}},
    {"unpack_short2_3", "vupkd3d128", 0x18641FF0u, 3, 3, {CD, CD, CD, 0x00008000u},
     {0x40400000u, 0x7FC00000u, 0u, 0x3F800000u}},
    {"unpack_short4_0", "vupkd3d128", 0x18701FF0u, 3, 3, {CD, CD, 0x7FFFFFFFu, 0x007FFFF8u},
     {0x40407FFFu, 0x403FFFFFu, 0x4040007Fu, 0x403FFFF8u}},
    {"unpack_float16_2_0", "vupkd3d128", 0x186C1FF0u, 3, 3, {CD, CD, CD, 0x3800B800u},
     {0x3F000000u, 0xBF000000u, 0u, 0x3F800000u}},
    {"unpack_float16_4_0", "vupkd3d128", 0x18741FF0u, 3, 3, {CD, CD, 0x3800B801u, 0x3802B803u},
     {0x3F000000u, 0xBF002000u, 0x3F004000u, 0xBF006000u}},
    {"unpack_2101010_0", "vupkd3d128", 0x18681FF0u, 3, 3, {CD, CD, CD, 0x400001FFu},
     {0x404001FFu, 0x40400000u, 0x40400000u, 0x3F800001u}},
    {"unpack_2101010_1", "vupkd3d128", 0x18681FF0u, 3, 3, {CD, CD, CD, 0x40000201u},
     {0x403FFE01u, 0x40400000u, 0x40400000u, 0x3F800001u}},
    {"unpack_2101010_2", "vupkd3d128", 0x18681FF0u, 3, 3, {CD, CD, CD, 0x40000200u},
     {0x7FC00000u, 0x40400000u, 0x40400000u, 0x3F800001u}},
    // AC6's bone code (0x822C3640): vupkd3d128 v13, v0, 4 after vspltisw v0, 0
    // - the (3.0, 3.0, 0.0, 1.0) whose w it splats into its quaternion-to-
    // matrix constants. Zero here made every cutscene bone matrix zero.
    {"unpack_ac6_ones", "vupkd3d128", 0x19A407F0u, 0, 13, {0u, 0u, 0u, 0u},
     {0x40400000u, 0x40400000u, 0u, 0x3F800000u}},

    {"pack_d3dcolor_invalid_0", "vpkd3d128", 0x18811E10u, 3, 4, {1u, 2u, 3u, 4u}, {CD, CD, CD, 0u}},
    {"pack_d3dcolor_invalid_1", "vpkd3d128", 0x18811E10u, 3, 4,
     {0x40800000u, 0x40000000u, 0xC2F60000u, 0x4B3BDF83u}, {CD, CD, CD, 0xFFFF0000u}},
    {"pack_d3dcolor_invalid_2", "vpkd3d128", 0x18811E10u, 3, 4,
     {0x40800000u, 0x40000000u, 0xC2F60000u, 0xFFFFFFFFu}, {CD, CD, CD, 0x00FF0000u}},
    {"pack_d3dcolor_1_0", "vpkd3d128", 0x18811E10u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, CD, CD, 0x04010203u}},
    {"pack_d3dcolor_1_1", "vpkd3d128", 0x18811E50u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, CD, 0x04010203u, CD}},
    {"pack_d3dcolor_1_2", "vpkd3d128", 0x18811E90u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, 0x04010203u, CD, CD}},
    {"pack_d3dcolor_1_3", "vpkd3d128", 0x18811ED0u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {0x04010203u, CD, CD, CD}},
    {"pack_d3dcolor_2_0", "vpkd3d128", 0x18821E10u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, CD, 0u, 0x04010203u}},
    {"pack_d3dcolor_2_1", "vpkd3d128", 0x18821E50u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, 0u, 0x04010203u, CD}},
    {"pack_d3dcolor_2_2", "vpkd3d128", 0x18821E90u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {0u, 0x04010203u, CD, CD}},
    {"pack_d3dcolor_2_3", "vpkd3d128", 0x18821ED0u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {0x04010203u, CD, CD, CD}},
    {"pack_d3dcolor_3_0", "vpkd3d128", 0x18831E10u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, CD, 0u, 0x04010203u}},
    {"pack_d3dcolor_3_1", "vpkd3d128", 0x18831E50u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, 0u, 0x04010203u, CD}},
    {"pack_d3dcolor_3_2", "vpkd3d128", 0x18831E90u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {0u, 0x04010203u, CD, CD}},
    {"pack_d3dcolor_3_3", "vpkd3d128", 0x18831ED0u, 3, 4, {kPack[0], kPack[1], kPack[2], kPack[3]},
     {CD, CD, CD, 0u}},
    {"pack_short2_invalid_0", "vpkd3d128", 0x18851E10u, 3, 4,
     {0x43817E00u, 0xC37CFC00u, 0x42A23EC8u, 0x403DB757u}, {CD, CD, CD, 0x7FFF8001u}},
    {"pack_short2_invalid_1", "vpkd3d128", 0x18851E10u, 3, 4,
     {0x412FDF00u, 0xC09FBE00u, 0x42A23EC8u, 0x403DB757u}, {CD, CD, CD, 0x7FFF8001u}},
    {"pack_short2_0", "vpkd3d128", 0x18851E10u, 3, 4, {0x40407FFFu, 0x403F8001u, 0u, 0u},
     {CD, CD, CD, 0x7FFF8001u}},
    {"pack_short2_1", "vpkd3d128", 0x18851E10u, 3, 4,
     {0x40404000u, 0x403FC000u, 0x40400003u, 0x403F8001u}, {CD, CD, CD, 0x4000C000u}},
    {"pack_short2_2", "vpkd3d128", 0x18851E10u, 3, 4,
     {0x4040FFFEu, 0x403FF333u, 0x42A23EC8u, 0x403DB757u}, {CD, CD, CD, 0x7FFFF333u}},
    {"pack_short4_0", "vpkd3d128", 0x18921E10u, 3, 4,
     {0x403F8001u, 0x403FFFF8u, 0x4040007Fu, 0x40400000u}, {CD, CD, 0x8001FFF8u, 0x007F0000u}},
    {"pack_2101010_0", "vpkd3d128", 0x18891E10u, 3, 4,
     {0xB8FF8000u, 0xB8FF8000u, 0xC04001FFu, 0x4E9A5A5Au}, {CD, CD, CD, 0xE0180601u}},
    {"pack_2101010_1", "vpkd3d128", 0x18891E10u, 3, 4,
     {0x42C80000u, 0xC2C80000u, 0x40400000u, 0x3F800000u}, {CD, CD, CD, 0x000805FFu}},
    {"pack_2101010_2", "vpkd3d128", 0x18891E10u, 3, 4,
     {0x3F000000u, 0xBF000000u, 0x3F800000u, 0u}, {CD, CD, CD, 0x20180601u}},
    {"pack_float16_2_invalid_0", "vpkd3d128", 0x188D1E10u, 3, 4,
     {0x3FC00000u, 0xBFC00000u, 0x42A23EC8u, 0x403DB757u}, {CD, CD, CD, 0x3E00BE00u}},
    {"pack_float16_2_0", "vpkd3d128", 0x188D1E10u, 3, 4, {0x3F000000u, 0xBF000000u, 0u, 0u},
     {CD, CD, CD, 0x3800B800u}},
    {"pack_float16_4_invalid_0", "vpkd3d128", 0x18961E10u, 3, 4,
     {0x3FC00000u, 0xBFC00000u, 0x3FC00000u, 0xBFC00000u}, {CD, CD, 0x3E00BE00u, 0x3E00BE00u}},
    {"pack_float16_4_0", "vpkd3d128", 0x18961E10u, 3, 4,
     {0x3F000000u, 0xBF000000u, 0x3F800000u, 0xBF800000u}, {CD, CD, 0x3800B800u, 0x3C00BC00u}},
};

void words(std::ostream& out, const std::uint32_t (&w)[4]) {
  out << '{';
  for (unsigned i = 0; i < 4; ++i) out << (i ? "," : "") << w[i] << 'u';
  out << '}';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream out(argv[1]);
  if (!out) return 3;
  out << "#include <bit>\n#include <cmath>\n#include <cfenv>\n#include <cstdint>\n#include <limits>\n"
         "#include \"xenon/cpu/aot_semantics.hpp\"\nusing namespace xenon::cpu;\n";
  Decoder decoder;
  Lifter lifter;
  backend::CppAotBackend backend;
  for (const auto& c : kCases) {
    const auto decoded = decoder.decode(0x6000u, c.word);
    if (!decoded.valid() || decoded.mnemonic() != c.mnemonic) {
      std::cerr << c.name << " decoded as " << decoded.mnemonic() << '\n';
      return 4;
    }
    ir::Block block{0x6000u, {}};
    ir::Builder builder(block);
    if (!lifter.lift(decoded, builder)) return 5;
    out << backend.emit_function(block, std::string("d3d_") + c.name) << '\n';
  }
  out << "struct D3DVectorCase { const char* name; std::uint32_t word; unsigned src, dst;"
         " std::uint32_t src_in[4]; std::uint32_t out[4];"
         " ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&); };\n"
         "extern const D3DVectorCase kD3DVectorCases[] = {\n";
  for (const auto& c : kCases) {
    out << "{\"" << c.name << "\", " << c.word << "u, " << c.src << "u, " << c.dst << "u, ";
    words(out, c.src_in);
    out << ", ";
    words(out, c.out);
    out << ", &d3d_" << c.name << "},\n";
  }
  out << "};\nextern const unsigned kD3DVectorCaseCount = " << std::size(kCases) << "u;\n";
}
