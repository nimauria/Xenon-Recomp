// Execute real generated code and the interpreter against independent VMX
// results. Poison unused registers to detect accidental immediate-as-register
// reads in the PCM conversion and mixing instruction families.
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>

#include "xenon/cpu/flat_memory.hpp"
#if XENON_TEST_VECTOR_FALLBACK
#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/memory/address_space.hpp"
#endif

using namespace xenon::cpu;
struct AudioVectorCase {
  const char* name;
  std::uint32_t word;
  ExecutionResult (*fn)(CpuState&, MemoryPort&, RuntimeServices&);
};
extern const AudioVectorCase kAudioVectorCases[];
extern const unsigned kAudioVectorCaseCount;

void set_float(Vector128& v, unsigned lane, float value) {
  v.set_u32_be(lane, std::bit_cast<std::uint32_t>(value));
}

int main() {
  FlatMemory memory(0x100u, 0x6000u);
  NullRuntimeServices runtime;
#if XENON_TEST_VECTOR_FALLBACK
  xenon::memory::AddressSpace guest;
  assert(guest.initialize());
  assert(guest.commit_fixed(0x00640000u, xenon::memory::kBasePageSize,
                            xenon::memory::kReadWriteExecute));
#endif
  for (unsigned n = 0; n < kAudioVectorCaseCount; ++n) {
    const auto& c = kAudioVectorCases[n];
    const std::string_view name = c.name;
    CpuState initial{};
    for (auto& v : initial.vr) v.bytes.fill(0xCD);
    Vector128 expected{};
    for (unsigned lane = 0; lane < 4; ++lane) {
      set_float(initial.vr[1], lane, 10.0f + lane);
      set_float(initial.vr[2], lane, 2.0f);
      set_float(initial.vr[3], lane, 3.0f + lane);
      set_float(initial.vr[4], lane, 4.0f);
      if (name == "vnmsubfp128") set_float(expected, lane, (10.0f + lane) - 2.0f * (3.0f + lane));
      if (name == "vnmsubfp") set_float(expected, lane, (3.0f + lane) - 2.0f * 4.0f);
      if (name == "vrefp") set_float(expected, lane, 1.0f / (3.0f + lane));
      if (name == "vspltw" || name == "vspltw128") set_float(expected, lane, 4.0f);
      if (name == "vpermwi128") set_float(expected, lane, 6.0f - lane);
      if (name == "vcfsx") {
        const auto value = -32768 + static_cast<std::int32_t>(lane) * 16384;
        initial.vr[3].set_u32_be(lane, std::bit_cast<std::uint32_t>(value));
        set_float(expected, lane, static_cast<float>(value) / 32768.0f);
      }
    }
    if (name == "vupkhsh" || name == "vupklsh") {
      constexpr std::array<std::int16_t, 8> pcm{-32768, -1234, 0, 32767, -1, 1, -2222, 2222};
      for (unsigned lane = 0; lane < 8; ++lane)
        initial.vr[3].set_u16_be(lane, static_cast<std::uint16_t>(pcm[lane]));
      for (unsigned lane = 0; lane < 4; ++lane)
        expected.set_u32_be(lane, static_cast<std::uint32_t>(
            static_cast<std::int32_t>(pcm[lane + (name == "vupklsh" ? 4 : 0)])));
    }
    // Cancellation that needs a fused multiply-subtract: separate float
    // multiply/subtract would round the product to 1 and lose the residual.
    if (name.starts_with("vnmsub")) {
      const float a = 1.0f + std::ldexp(1.0f, -23);
      const float b = 1.0f - std::ldexp(1.0f, -23);
      set_float(initial.vr[2], 3, a);
      set_float(initial.vr[name == "vnmsubfp128" ? 3 : 4], 3, b);
      set_float(initial.vr[name == "vnmsubfp128" ? 1 : 3], 3, 1.0f);
      set_float(expected, 3, std::ldexp(1.0f, -46));
    }
    auto native = initial;
    c.fn(native, memory, runtime);
    if (native.vr[1].bytes != expected.bytes) std::cerr << "native " << name << '\n';
    assert(native.vr[1].bytes == expected.bytes);
#if XENON_TEST_VECTOR_FALLBACK
    guest.write32_be(0x00640000u, c.word);
    guest.write32_be(0x00640004u, 0x4E800020u);
    auto interpreted = initial;
    interpreted.lr = 0x0064F000u;
    DynamicFallbackExecutor fallback;
    ExecutionContext context(interpreted, guest, runtime);
    fallback.bind(context);
    const auto result = context.try_dynamic_fallback(0x00640000u, CompiledLookupKind::Call);
    assert(result.handled && result.result.reason == FlowReason::Return);
    if (interpreted.vr[1].bytes != expected.bytes) std::cerr << "fallback " << name << '\n';
    assert(interpreted.vr[1].bytes == expected.bytes);
#endif
  }
  std::cout << "xenon_cpu_vector_audio: ok\n";
}
