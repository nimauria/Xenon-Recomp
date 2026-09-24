#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/cpu/decoder.hpp"
#include "xenon/recomp/ppc_value_analysis.hpp"

namespace cpu = xenon::cpu;
namespace recomp = xenon::recomp;
namespace va = xenon::recomp::value_analysis;
namespace xbox = xenon::xbox;

namespace {

constexpr std::uint32_t kCodeBase = 0x82000000u;
constexpr std::uint32_t kDataBase = 0x83000000u;
constexpr std::uint32_t kBctrl = 0x4E800421u;

void be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  assert(offset + 4u <= bytes.size());
  bytes[offset + 0u] = static_cast<std::byte>(value >> 24u);
  bytes[offset + 1u] = static_cast<std::byte>(value >> 16u);
  bytes[offset + 2u] = static_cast<std::byte>(value >> 8u);
  bytes[offset + 3u] = static_cast<std::byte>(value);
}

std::uint32_t lis_word(std::uint32_t rd, std::uint16_t value) {
  return (15u << 26u) | (rd << 21u) | value;
}

std::uint32_t ori_word(std::uint32_t ra, std::uint32_t rs, std::uint16_t value) {
  return (24u << 26u) | (rs << 21u) | (ra << 16u) | value;
}

std::uint32_t lwz_word(std::uint32_t rt, std::uint32_t ra, std::int16_t displacement) {
  return (32u << 26u) | (rt << 21u) | (ra << 16u) |
         static_cast<std::uint16_t>(displacement);
}

std::uint32_t mtctr_word(std::uint32_t rs) {
  constexpr std::uint32_t kCtrSpr = 9u;
  const auto encoded_spr = ((kCtrSpr & 0x1Fu) << 5u) | ((kCtrSpr >> 5u) & 0x1Fu);
  return (31u << 26u) | (rs << 21u) | (encoded_spr << 11u) | (467u << 1u);
}

std::uint32_t cmplwi_word(std::uint32_t ra, std::uint16_t value) {
  return (10u << 26u) | (ra << 16u) | value;
}

std::uint32_t bl_word(std::uint32_t from, std::uint32_t to) {
  return 0x48000001u | ((to - from) & 0x03FFFFFCu);
}

xbox::XexImage make_image(bool data_writable = false) {
  xbox::XexImage image{};

  xbox::XexSection code{};
  code.name = ".text";
  code.virtual_address = kCodeBase;
  code.virtual_size = 0x2000u;
  code.raw_size = 0x2000u;
  code.executable = true;
  code.readable = true;
  code.writable = false;
  code.bytes.resize(0x2000u);
  image.sections.push_back(std::move(code));

  xbox::XexSection data{};
  data.name = ".rdata";
  data.virtual_address = kDataBase;
  data.virtual_size = 0x1000u;
  data.raw_size = 0x1000u;
  data.executable = false;
  data.readable = true;
  data.writable = data_writable;
  data.bytes.resize(0x1000u);
  image.sections.push_back(std::move(data));

  return image;
}

cpu::DecodedInstruction decode(const cpu::Decoder& decoder, std::uint32_t address,
                               std::uint32_t word) {
  const auto decoded = decoder.decode(address, word);
  assert(decoded.valid());
  return decoded;
}

void feed(va::PpcValueTracker& tracker, std::vector<cpu::DecodedInstruction>& history,
          const cpu::DecodedInstruction& instruction) {
  history.push_back(instruction);
  tracker.step(instruction);
}

}  // namespace

int main() {
  cpu::Decoder decoder{};

  // The lattice must preserve useful small finite alternatives instead of
  // immediately collapsing two exact values to Unknown.
  {
    const auto joined = va::join(va::AbstractValue::constant(0x10u),
                                 va::AbstractValue::constant(0x20u));
    assert(joined.kind == va::AbstractValueKind::FiniteSet);
    const auto values = joined.possible_u32();
    assert(values.size() == 2u);
    assert(std::find(values.begin(), values.end(), 0x10u) != values.end());
    assert(std::find(values.begin(), values.end(), 0x20u) != values.end());
  }
  std::cout << "  [PASS] abstract-value join retains bounded finite alternatives\n";

  // Cheap forward value propagation survives unrelated compares. The old
  // tracker discarded every GPR fact on almost any unrecognised instruction.
  {
    auto image = make_image();
    const auto target = kCodeBase + 0x1000u;
    va::PpcValueTracker tracker(image);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;

    feed(tracker, history, decode(decoder, pc, lis_word(11u, static_cast<std::uint16_t>(target >> 16u))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, ori_word(11u, 11u, static_cast<std::uint16_t>(target))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, cmplwi_word(3u, 7u)));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, mtctr_word(11u)));
    pc += 4u;

    const auto branch = decode(decoder, pc, kBctrl);
    const auto resolution = va::resolve_indirect_flow(branch, tracker, history, image);
    assert(resolution.kind == va::IndirectResolverKind::FastValue);
    assert(resolution.targets == std::vector<std::uint32_t>{target});
  }
  std::cout << "  [PASS] block-local PPC values survive unrelated instructions\n";

  // A statically readable, non-writable pointer table may be folded into an
  // executable target and is reported distinctly for diagnostics/provenance.
  {
    auto image = make_image();
    const auto target = kCodeBase + 0x1200u;
    const auto table = kDataBase + 0x40u;
    be32(image.sections[1].bytes, table - kDataBase, target);

    va::PpcValueTracker tracker(image);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;
    feed(tracker, history, decode(decoder, pc, lis_word(10u, static_cast<std::uint16_t>(table >> 16u))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, ori_word(10u, 10u, static_cast<std::uint16_t>(table))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, lwz_word(11u, 10u, 0)));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, mtctr_word(11u)));
    pc += 4u;

    const auto resolution = va::resolve_indirect_flow(
        decode(decoder, pc, kBctrl), tracker, history, image);
    assert(resolution.kind == va::IndirectResolverKind::ReadOnlyTable);
    assert(resolution.table_address == table);
    assert(resolution.targets == std::vector<std::uint32_t>{target});
  }
  std::cout << "  [PASS] read-only function-pointer loads resolve indirect calls\n";

  // Writable tables are runtime state. Static analysis must never pretend the
  // on-disk bytes are authoritative executable targets.
  {
    auto image = make_image(true);
    const auto target = kCodeBase + 0x1400u;
    const auto table = kDataBase + 0x40u;
    be32(image.sections[1].bytes, table - kDataBase, target);

    va::PpcValueTracker tracker(image);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;
    feed(tracker, history, decode(decoder, pc, lis_word(10u, static_cast<std::uint16_t>(table >> 16u))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, ori_word(10u, 10u, static_cast<std::uint16_t>(table))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, lwz_word(11u, 10u, 0)));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, mtctr_word(11u)));
    pc += 4u;

    const auto resolution = va::resolve_indirect_flow(
        decode(decoder, pc, kBctrl), tracker, history, image);
    assert(resolution.targets.empty());
    assert(resolution.kind == va::IndirectResolverKind::None);
  }
  std::cout << "  [PASS] writable function-pointer tables remain unresolved\n";

  // Calls are hard clobber boundaries for volatile registers and CTR. A
  // pre-call r11 constant must not leak through either the fast tracker or
  // the fallback slicer.
  {
    auto image = make_image();
    const auto target = kCodeBase + 0x1500u;
    va::PpcValueTracker tracker(image);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;
    feed(tracker, history, decode(decoder, pc, lis_word(11u, static_cast<std::uint16_t>(target >> 16u))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, ori_word(11u, 11u, static_cast<std::uint16_t>(target))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, bl_word(pc, pc + 4u)));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, mtctr_word(11u)));
    pc += 4u;

    const auto resolution = va::resolve_indirect_flow(
        decode(decoder, pc, kBctrl), tracker, history, image);
    assert(resolution.targets.empty());
  }
  std::cout << "  [PASS] calls invalidate volatile-register and CTR facts\n";

  // Nonvolatile GPRs remain useful across a call, matching the ABI while
  // avoiding the old all-register reset.
  {
    auto image = make_image();
    const auto target = kCodeBase + 0x1500u;
    va::PpcValueTracker tracker(image);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;
    feed(tracker, history, decode(decoder, pc, lis_word(14u, static_cast<std::uint16_t>(target >> 16u))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, ori_word(14u, 14u, static_cast<std::uint16_t>(target))));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, bl_word(pc, pc + 4u)));
    pc += 4u;
    feed(tracker, history, decode(decoder, pc, mtctr_word(14u)));
    pc += 4u;

    const auto resolution = va::resolve_indirect_flow(
        decode(decoder, pc, kBctrl), tracker, history, image);
    assert(resolution.kind == va::IndirectResolverKind::FastValue);
    assert(resolution.targets == std::vector<std::uint32_t>{target});
  }
  std::cout << "  [PASS] calls preserve nonvolatile-register facts\n";

  // Expensive analysis is selective: if the cheap state is absent, the
  // bounded slicer reconstructs the last writer chain from local history.
  {
    auto image = make_image();
    const auto target = kCodeBase + 0x1600u;
    va::PpcValueTracker tracker(image);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;
    history.push_back(decode(decoder, pc, lis_word(11u, static_cast<std::uint16_t>(target >> 16u))));
    pc += 4u;
    history.push_back(decode(decoder, pc, ori_word(11u, 11u, static_cast<std::uint16_t>(target))));
    pc += 4u;
    history.push_back(decode(decoder, pc, mtctr_word(11u)));
    pc += 4u;

    // tracker intentionally remains empty, forcing tier 2.
    const auto resolution = va::resolve_indirect_flow(
        decode(decoder, pc, kBctrl), tracker, history, image);
    assert(resolution.kind == va::IndirectResolverKind::BackwardSlice);
    assert(resolution.targets == std::vector<std::uint32_t>{target});
    assert(resolution.instructions_examined > 0u);
  }
  std::cout << "  [PASS] bounded backward slicing is used only after fast-state failure\n";

  std::cout << "All Gen 6 PPC value-analysis tests passed!\n";
  return 0;
}
