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

constexpr std::uint32_t kBctr = 0x4E800420u;

cpu::DecodedInstruction decode(const cpu::Decoder& decoder, std::uint32_t address,
                               std::uint32_t word);

std::uint32_t cmplwi_cr_word(std::uint32_t crf, std::uint32_t ra, std::uint16_t value) {
  return (10u << 26u) | (crf << 23u) | (ra << 16u) | value;
}

std::uint32_t cmpwi_cr_word(std::uint32_t crf, std::uint32_t ra, std::uint16_t value) {
  return (11u << 26u) | (crf << 23u) | (ra << 16u) | value;
}

std::uint32_t bc_word(std::uint32_t bo, std::uint32_t bi, std::uint32_t displacement) {
  return (16u << 26u) | (bo << 21u) | (bi << 16u) | (displacement & 0xFFFCu);
}

std::uint32_t addi_word(std::uint32_t rd, std::uint32_t ra, std::int16_t value) {
  return (14u << 26u) | (rd << 21u) | (ra << 16u) | static_cast<std::uint16_t>(value);
}

std::uint32_t rlwinm_word(std::uint32_t ra, std::uint32_t rs, std::uint32_t sh, std::uint32_t mb,
                          std::uint32_t me) {
  return (21u << 26u) | (rs << 21u) | (ra << 16u) | (sh << 11u) | (mb << 6u) | (me << 1u);
}

std::uint32_t x_word(std::uint32_t rt, std::uint32_t ra, std::uint32_t rb, std::uint32_t xo) {
  return (31u << 26u) | (rt << 21u) | (ra << 16u) | (rb << 11u) | (xo << 1u);
}

constexpr std::uint32_t kLwzxXo = 23u;
constexpr std::uint32_t kLbzxXo = 87u;
constexpr std::uint32_t kAddXo = 266u;
// cr6 GT / LT condition bits and BO "branch if true" / "branch if false".
constexpr std::uint32_t kCr6Gt = 25u;
constexpr std::uint32_t kCr6Lt = 24u;
constexpr std::uint32_t kBranchIfTrue = 12u;
constexpr std::uint32_t kBranchIfFalse = 4u;

// Ace Combat 6's switch dispatch at 0x823DBD24 (MSVC): an unsigned bound,
// the table address, the index scaled by four, an indexed word load, mtctr,
// bctr - with the table placed right after the bctr in .text.
struct SwitchFixture {
  std::vector<cpu::DecodedInstruction> history;
  std::uint32_t bctr_address{};
};

SwitchFixture absolute_switch(const cpu::Decoder& decoder, std::uint32_t compare_word,
                              std::uint32_t bound_branch_word) {
  SwitchFixture fixture;
  const auto table = kCodeBase + 0x40u;
  std::uint32_t pc = kCodeBase;
  const auto push = [&](std::uint32_t word) {
    fixture.history.push_back(decode(decoder, pc, word));
    pc += 4u;
  };
  push(compare_word);
  push(bound_branch_word);
  push(lis_word(12u, static_cast<std::uint16_t>(table >> 16u)));
  push(addi_word(12u, 12u, static_cast<std::int16_t>(table & 0xFFFFu)));
  push(rlwinm_word(0u, 9u, 2u, 0u, 29u));
  push(x_word(0u, 12u, 0u, kLwzxXo));
  push(mtctr_word(0u));
  fixture.bctr_address = pc;
  return fixture;
}

void write_absolute_table(xbox::XexImage& image, const std::vector<std::uint32_t>& targets) {
  for (std::size_t i = 0; i < targets.size(); ++i)
    be32(image.sections[0].bytes, 0x40u + i * 4u, targets[i]);
}

va::IndirectResolution resolve_switch(const cpu::Decoder& decoder, const SwitchFixture& fixture,
                                      const xbox::XexImage& image) {
  va::PpcValueTracker tracker(image);
  for (const auto& instruction : fixture.history) tracker.step(instruction);
  return va::resolve_indirect_flow(decode(decoder, fixture.bctr_address, kBctr), tracker,
                                   fixture.history, image);
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

  // Regression (Ace Combat 6 startup): switch jump tables were never
  // resolved, so their case blocks were never compiled and each dispatch
  // ran in the interpreter. `cmplwi cr6, r9, 3; bgt cr6` admits indices 0-3:
  // exactly four entries are read, in table order, duplicates folded.
  {
    auto image = make_image();
    const std::vector<std::uint32_t> cases{kCodeBase + 0x100u, kCodeBase + 0x180u,
                                           kCodeBase + 0x100u, kCodeBase + 0x200u};
    write_absolute_table(image, cases);
    // A fifth word past the bound that must not be read as a case.
    be32(image.sections[0].bytes, 0x50u, kCodeBase + 0x300u);
    const auto fixture = absolute_switch(decoder, cmplwi_cr_word(6u, 9u, 3u),
                                         bc_word(kBranchIfTrue, kCr6Gt, 0x400u));
    const auto resolution = resolve_switch(decoder, fixture, image);
    assert(resolution.kind == va::IndirectResolverKind::JumpTable);
    assert(resolution.table_address == kCodeBase + 0x40u);
    assert((resolution.targets ==
            std::vector<std::uint32_t>{kCodeBase + 0x100u, kCodeBase + 0x180u, kCodeBase + 0x200u}));
  }
  std::cout << "  [PASS] bounded absolute switch tables resolve every case\n";

  // `bge` exits for index >= N, so N entries (0..N-1) are live.
  {
    auto image = make_image();
    write_absolute_table(image, {kCodeBase + 0x100u, kCodeBase + 0x180u, kCodeBase + 0x200u});
    const auto fixture = absolute_switch(decoder, cmplwi_cr_word(6u, 9u, 2u),
                                         bc_word(kBranchIfFalse, kCr6Lt, 0x400u));
    const auto resolution = resolve_switch(decoder, fixture, image);
    assert(resolution.kind == va::IndirectResolverKind::JumpTable);
    assert((resolution.targets == std::vector<std::uint32_t>{kCodeBase + 0x100u, kCodeBase + 0x180u}));
  }
  std::cout << "  [PASS] bge bounds admit indices below the compared value\n";

  // Byte-offset tables: target = base + (lbzx(table + index) << 2).
  {
    auto image = make_image();
    const auto table = kDataBase + 0x80u;
    const auto base = kCodeBase + 0x400u;
    const std::uint8_t offsets[] = {0u, 3u, 8u};
    for (std::size_t i = 0; i < 3; ++i)
      image.sections[1].bytes[0x80u + i] = static_cast<std::byte>(offsets[i]);
    std::vector<cpu::DecodedInstruction> history;
    std::uint32_t pc = kCodeBase;
    const auto push = [&](std::uint32_t word) {
      history.push_back(decode(decoder, pc, word));
      pc += 4u;
    };
    push(cmplwi_cr_word(6u, 11u, 2u));
    push(bc_word(kBranchIfTrue, kCr6Gt, 0x400u));
    push(lis_word(10u, static_cast<std::uint16_t>(table >> 16u)));
    push(addi_word(10u, 10u, static_cast<std::int16_t>(table & 0xFFFFu)));
    push(x_word(0u, 10u, 11u, kLbzxXo));
    push(lis_word(12u, static_cast<std::uint16_t>(base >> 16u)));
    push(addi_word(12u, 12u, static_cast<std::int16_t>(base & 0xFFFFu)));
    push(rlwinm_word(0u, 0u, 2u, 0u, 29u));
    push(x_word(0u, 12u, 0u, kAddXo));
    push(mtctr_word(0u));
    const auto resolution = resolve_switch(decoder, {history, pc}, image);
    assert(resolution.kind == va::IndirectResolverKind::JumpTable);
    assert(resolution.table_address == table);
    assert((resolution.targets == std::vector<std::uint32_t>{base, base + 12u, base + 32u}));
  }
  std::cout << "  [PASS] byte-offset switch tables resolve relative to their base\n";

  // Without a dominating unsigned bound the table's length is unknown, and a
  // signed compare admits negative indices: neither may be guessed at.
  {
    auto image = make_image();
    write_absolute_table(image, {kCodeBase + 0x100u, kCodeBase + 0x180u, kCodeBase + 0x200u,
                                 kCodeBase + 0x280u});
    const auto unbounded = absolute_switch(decoder, cmplwi_cr_word(6u, 9u, 3u),
                                           addi_word(3u, 3u, 1));  // no bound branch
    assert(resolve_switch(decoder, unbounded, image).targets.empty());
    const auto signed_bound = absolute_switch(decoder, cmpwi_cr_word(6u, 9u, 3u),
                                              bc_word(kBranchIfTrue, kCr6Gt, 0x400u));
    assert(resolve_switch(decoder, signed_bound, image).targets.empty());
    // ble leaves for in-range indices: the fallthrough is the out-of-range path.
    const auto inverted = absolute_switch(decoder, cmplwi_cr_word(6u, 9u, 3u),
                                          bc_word(kBranchIfFalse, kCr6Gt, 0x400u));
    assert(resolve_switch(decoder, inverted, image).targets.empty());
  }
  std::cout << "  [PASS] switch tables need a dominating unsigned bound\n";

  // A writable table is runtime state, and an entry that is not code means
  // this is not a jump table: the whole table is rejected, never trimmed.
  {
    auto writable = make_image(true);
    const auto table = kDataBase + 0x40u;
    for (std::uint32_t i = 0; i < 4u; ++i)
      be32(writable.sections[1].bytes, 0x40u + i * 4u, kCodeBase + 0x100u + i * 0x40u);
    SwitchFixture fixture;
    std::uint32_t pc = kCodeBase;
    for (const auto word : {cmplwi_cr_word(6u, 9u, 3u), bc_word(kBranchIfTrue, kCr6Gt, 0x400u),
                            lis_word(12u, static_cast<std::uint16_t>(table >> 16u)),
                            addi_word(12u, 12u, static_cast<std::int16_t>(table & 0xFFFFu)),
                            rlwinm_word(0u, 9u, 2u, 0u, 29u), x_word(0u, 12u, 0u, kLwzxXo),
                            mtctr_word(0u)}) {
      fixture.history.push_back(decode(decoder, pc, word));
      pc += 4u;
    }
    fixture.bctr_address = pc;
    assert(resolve_switch(decoder, fixture, writable).targets.empty());

    auto image = make_image();
    write_absolute_table(image, {kCodeBase + 0x100u, kCodeBase + 0x180u, kDataBase + 0x10u,
                                 kCodeBase + 0x200u});
    const auto data_entry = absolute_switch(decoder, cmplwi_cr_word(6u, 9u, 3u),
                                            bc_word(kBranchIfTrue, kCr6Gt, 0x400u));
    assert(resolve_switch(decoder, data_entry, image).targets.empty());
  }
  std::cout << "  [PASS] writable or non-code switch tables are rejected whole\n";

  std::cout << "All Gen 6 PPC value-analysis tests passed!\n";
  return 0;
}
