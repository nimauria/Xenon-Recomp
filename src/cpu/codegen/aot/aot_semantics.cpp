#include "xenon/cpu/aot_semantics.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace xenon::cpu::aot {
namespace {

template <class T>
T read_be(const Vector128& v, unsigned lane) noexcept {
  if constexpr (sizeof(T) == 1) {
    return static_cast<T>(v.bytes[lane & 15u]);
  } else if constexpr (sizeof(T) == 2) {
    const auto u = v.u16_be(lane);
    return std::bit_cast<T>(u);
  } else if constexpr (sizeof(T) == 4) {
    const auto u = v.u32_be(lane);
    return std::bit_cast<T>(u);
  }
}

template <class T>
void write_be(Vector128& v, unsigned lane, T value) noexcept {
  if constexpr (sizeof(T) == 1) {
    v.bytes[lane & 15u] = static_cast<std::uint8_t>(value);
  } else if constexpr (sizeof(T) == 2) {
    v.set_u16_be(lane, std::bit_cast<std::uint16_t>(value));
  } else if constexpr (sizeof(T) == 4) {
    v.set_u32_be(lane, std::bit_cast<std::uint32_t>(value));
  }
}

float read_f32(const Vector128& v, unsigned lane) noexcept {
  return std::bit_cast<float>(v.u32_be(lane));
}
void write_f32(Vector128& v, unsigned lane, float f) noexcept {
  v.set_u32_be(lane, std::bit_cast<std::uint32_t>(f));
}

// VMX floating-point operations have their own denormal policy controlled by
// VSCR.NJ (non-Java mode).  Keep that policy local to the vector semantic
// layer rather than mutating the host process MXCSR/FPCR.  This is portable to
// Linux x86-64 now and remains suitable for an eventual ARM64 backend.
float vmx_flush_subnormal(float value, const CpuState& state) noexcept {
  if (!state.vector_non_java()) return value;
  const auto bits = std::bit_cast<std::uint32_t>(value);
  const auto magnitude = bits & 0x7FFFFFFFu;
  if ((magnitude & 0x7F800000u) == 0 && (magnitude & 0x007FFFFFu) != 0) {
    return std::bit_cast<float>(bits & 0x80000000u); // preserve signed zero
  }
  return value;
}
float read_vmx_f32(const Vector128& v, unsigned lane, const CpuState& state) noexcept {
  return vmx_flush_subnormal(read_f32(v, lane), state);
}
void write_vmx_f32(Vector128& v, unsigned lane, float f, const CpuState& state) noexcept {
  write_f32(v, lane, vmx_flush_subnormal(f, state));
}


float round_nearest_even(float value) noexcept {
  fenv_t env{};
  const bool saved = std::fegetenv(&env) == 0;
  std::fesetround(FE_TONEAREST);
  volatile float result = std::nearbyint(value);
  if (saved) std::fesetenv(&env);
  return result;
}

template <typename T>
T vector_fp_to_int_sat(float value, CpuState& state) noexcept {
  if (std::isnan(value)) {
    state.set_vector_saturated();
    if constexpr (std::is_signed_v<T>) return std::numeric_limits<T>::min();
    else return T{};
  }
  const long double x = std::trunc(static_cast<long double>(value));
  const long double lo = static_cast<long double>(std::numeric_limits<T>::min());
  const long double hi = static_cast<long double>(std::numeric_limits<T>::max());
  if (x < lo) { state.set_vector_saturated(); return std::numeric_limits<T>::min(); }
  if (x > hi) { state.set_vector_saturated(); return std::numeric_limits<T>::max(); }
  return static_cast<T>(x);
}

template <class T, class Wide, class Op>
Vector128 lane_binary(const Vector128& a, const Vector128& b, Op op) {
  Vector128 r{};
  constexpr unsigned n = 16u / sizeof(T);
  for (unsigned i = 0; i < n; ++i) {
    const T x = read_be<T>(a, i), y = read_be<T>(b, i);
    write_be<T>(r, i, static_cast<T>(op(static_cast<Wide>(x), static_cast<Wide>(y))));
  }
  return r;
}

template <class T, class Wide>
Vector128 add_sat(const Vector128& a, const Vector128& b, CpuState& state) {
  Vector128 r{}; constexpr unsigned n=16u/sizeof(T);
  for(unsigned i=0;i<n;++i){
    Wide x=read_be<T>(a,i), y=read_be<T>(b,i), z=x+y;
    Wide lo=std::numeric_limits<T>::min(), hi=std::numeric_limits<T>::max();
    if(z<lo){z=lo;state.set_vector_saturated();}
    if(z>hi){z=hi;state.set_vector_saturated();}
    write_be<T>(r,i,static_cast<T>(z));
  }
  return r;
}

template <class T, class Wide>
Vector128 sub_sat(const Vector128& a, const Vector128& b, CpuState& state) {
  Vector128 r{}; constexpr unsigned n=16u/sizeof(T);
  for(unsigned i=0;i<n;++i){
    Wide x=read_be<T>(a,i), y=read_be<T>(b,i), z=x-y;
    Wide lo=std::numeric_limits<T>::min(), hi=std::numeric_limits<T>::max();
    if(z<lo){z=lo;state.set_vector_saturated();}
    if(z>hi){z=hi;state.set_vector_saturated();}
    write_be<T>(r,i,static_cast<T>(z));
  }
  return r;
}

template <class T, class Wide>
Vector128 avg_round(const Vector128& a,const Vector128& b){
  Vector128 r{}; constexpr unsigned n=16u/sizeof(T);
  for(unsigned i=0;i<n;++i){
    Wide x=read_be<T>(a,i), y=read_be<T>(b,i);
    // Architected vector average rounds upward: (a+b+1)>>1.
    Wide z=x+y+1;
    if constexpr (std::is_signed_v<T>) {
      // PowerPC vector average uses an arithmetic right shift of the rounded sum.
      z = z >= 0 ? z / 2 : (z / 2 - ((z & 1) ? 1 : 0));
    } else {
      z /= 2;
    }
    write_be<T>(r,i,static_cast<T>(z));
  }
  return r;
}

template <class T>
Vector128 minmax(const Vector128&a,const Vector128&b,bool max){
  Vector128 r{}; constexpr unsigned n=16u/sizeof(T);
  for(unsigned i=0;i<n;++i){auto x=read_be<T>(a,i),y=read_be<T>(b,i); write_be<T>(r,i,max?(x>y?x:y):(x<y?x:y));}
  return r;
}

template <class T, class Pred>
Vector128 compare_lanes(const Vector128&a,const Vector128&b,Pred pred){
  Vector128 r{}; constexpr unsigned n=16u/sizeof(T); using U=std::make_unsigned_t<T>;
  for(unsigned i=0;i<n;++i){ const bool yes=pred(read_be<T>(a,i),read_be<T>(b,i)); write_be<U>(r,i,yes?std::numeric_limits<U>::max():U{}); }
  return r;
}

Vector128 fp_binary(const Vector128&a,const Vector128&b,char op,const CpuState& state){
  Vector128 r{};
  for(unsigned i=0;i<4;++i){
    float x=read_vmx_f32(a,i,state),y=read_vmx_f32(b,i,state),z{};
    switch(op){case '+':z=x+y;break;case '-':z=x-y;break;case '*':z=x*y;break;case 'x':z=std::fmax(x,y);break;case 'n':z=std::fmin(x,y);break;}
    write_vmx_f32(r,i,z,state);
  }
  return r;
}

std::int32_t sext5(std::uint32_t x){ return static_cast<std::int32_t>(sign_extend<5>(x&31u)); }
std::uint32_t vx128_imm3(std::uint32_t w){return (w>>16)&31u;}
std::uint32_t vx128_imm4(std::uint32_t w){return (w>>16)&31u;}
std::uint32_t vx128_z4(std::uint32_t w){return (w>>6)&3u;}
std::uint32_t vx128_sh5(std::uint32_t w){return (w>>6)&15u;}
std::uint32_t vx128_perm(std::uint32_t w){return ((w>>16)&31u)|(((w>>6)&7u)<<5);}
std::uint32_t vx_imm5(std::uint32_t w){return (w>>16)&31u;}
std::uint32_t va_sh4(std::uint32_t w){return (w>>6)&15u;}

Vector128 permute_bytes(const Vector128&a,const Vector128&b,const Vector128&control){
  Vector128 r{}; std::array<std::uint8_t,32> src{}; for(unsigned i=0;i<16;++i){src[i]=a.bytes[i];src[16+i]=b.bytes[i];}
  for(unsigned i=0;i<16;++i) r.bytes[i]=src[control.bytes[i]&31u];
  return r;
}

Vector128 shift_double(const Vector128&a,const Vector128&b,unsigned sh){
  Vector128 r{}; std::array<std::uint8_t,32> s{}; for(unsigned i=0;i<16;++i){s[i]=a.bytes[i];s[16+i]=b.bytes[i];}
  sh&=15u; for(unsigned i=0;i<16;++i) r.bytes[i]=s[i+sh]; return r;
}

Vector128 whole_shift(const Vector128&a,unsigned bits,bool left){
  Vector128 r{}; bits=std::min(bits,128u); if(bits==128) return r;
  for(unsigned out=0;out<128;++out){
    const int src=left?int(out)+int(bits):int(out)-int(bits);
    if(src<0||src>=128) continue;
    const unsigned sb=unsigned(src)/8, sm=7u-(unsigned(src)%8);
    const unsigned db=out/8, dm=7u-(out%8);
    if((a.bytes[sb]>>sm)&1u) r.bytes[db]|=std::uint8_t(1u<<dm);
  }
  return r;
}

std::uint16_t float_to_half(float f) noexcept {
  const std::uint32_t x=std::bit_cast<std::uint32_t>(f); const std::uint32_t sign=(x>>16)&0x8000u;
  int exp=int((x>>23)&0xFFu)-127+15; std::uint32_t mant=x&0x7FFFFFu;
  if(((x>>23)&0xFFu)==0xFFu) return std::uint16_t(sign|(mant?0x7E00u:0x7C00u));
  if(exp<=0){ if(exp<-10)return std::uint16_t(sign); mant|=0x800000u; const unsigned s=unsigned(14-exp); auto h=(mant+(1u<<(s-1)))>>s; return std::uint16_t(sign|h); }
  if(exp>=31)return std::uint16_t(sign|0x7C00u);
  mant += 0x1000u; if(mant&0x800000u){mant=0;++exp;if(exp>=31)return std::uint16_t(sign|0x7C00u);} return std::uint16_t(sign|(unsigned(exp)<<10)|(mant>>13));
}
float half_to_float(std::uint16_t h) noexcept {
  const std::uint32_t sign=(std::uint32_t(h&0x8000u))<<16; std::uint32_t exp=(h>>10)&31u,mant=h&0x3FFu,bits{};
  if(exp==0){ if(!mant)bits=sign; else{int e=-14;while((mant&0x400u)==0){mant<<=1;--e;}mant&=0x3FFu;bits=sign|(std::uint32_t(e+127)<<23)|(mant<<13);} }
  else if(exp==31)bits=sign|0x7F800000u|(mant<<13); else bits=sign|((exp-15+127)<<23)|(mant<<13);
  return std::bit_cast<float>(bits);
}

template<class T, class U>
T sat_cast(U v,CpuState&state){ auto lo=static_cast<long double>(std::numeric_limits<T>::min()),hi=static_cast<long double>(std::numeric_limits<T>::max()),x=static_cast<long double>(v); if(x<lo){state.set_vector_saturated();return std::numeric_limits<T>::min();} if(x>hi){state.set_vector_saturated();return std::numeric_limits<T>::max();} return static_cast<T>(v); }

Vector128 pack_standard(VectorSemantic s,const Vector128&a,const Vector128&b,CpuState&state){
  Vector128 r{};
  // Explicit lane ordering is simpler and less error-prone than the compact helper above.
  if(s==VectorSemantic::vpkuhum||s==VectorSemantic::vpkuhum128||s==VectorSemantic::vpkuhus||s==VectorSemantic::vpkuhus128||s==VectorSemantic::vpkshss||s==VectorSemantic::vpkshss128||s==VectorSemantic::vpkshus||s==VectorSemantic::vpkshus128){
    // VMX pack concatenates the low-order half of each input's halfword lanes.
    for(unsigned i=0;i<8;++i){
      auto emit=[&](const Vector128&v,unsigned lane,unsigned out){
        if(s==VectorSemantic::vpkuhum||s==VectorSemantic::vpkuhum128) r.bytes[out]=std::uint8_t(read_be<std::uint16_t>(v,lane));
        else if(s==VectorSemantic::vpkuhus||s==VectorSemantic::vpkuhus128) r.bytes[out]=sat_cast<std::uint8_t>(read_be<std::uint16_t>(v,lane),state);
        else if(s==VectorSemantic::vpkshss||s==VectorSemantic::vpkshss128) r.bytes[out]=std::uint8_t(sat_cast<std::int8_t>(read_be<std::int16_t>(v,lane),state));
        else r.bytes[out]=sat_cast<std::uint8_t>(read_be<std::int16_t>(v,lane),state);
      };
      emit(a,i,i); emit(b,i,8+i);
    }
    return r;
  }
  if(s==VectorSemantic::vpkuwum||s==VectorSemantic::vpkuwum128||s==VectorSemantic::vpkuwus||s==VectorSemantic::vpkuwus128||s==VectorSemantic::vpkswss||s==VectorSemantic::vpkswss128||s==VectorSemantic::vpkswus||s==VectorSemantic::vpkswus128){
    for(unsigned i=0;i<4;++i){
      auto emit=[&](const Vector128&v,unsigned lane,unsigned out){
        if(s==VectorSemantic::vpkuwum||s==VectorSemantic::vpkuwum128) r.set_u16_be(out,std::uint16_t(read_be<std::uint32_t>(v,lane)));
        else if(s==VectorSemantic::vpkuwus||s==VectorSemantic::vpkuwus128) r.set_u16_be(out,sat_cast<std::uint16_t>(read_be<std::uint32_t>(v,lane),state));
        else if(s==VectorSemantic::vpkswss||s==VectorSemantic::vpkswss128) r.set_u16_be(out,std::uint16_t(sat_cast<std::int16_t>(read_be<std::int32_t>(v,lane),state)));
        else r.set_u16_be(out,sat_cast<std::uint16_t>(read_be<std::int32_t>(v,lane),state));
      }; emit(a,i,i);emit(b,i,4+i);
    } return r;
  }
  return r;
}

Vector128 unpack_standard(VectorSemantic s,const Vector128&b){
  Vector128 r{};
  if(s==VectorSemantic::vupkhsb||s==VectorSemantic::vupkhsb128){for(unsigned i=0;i<8;++i)write_be<std::int16_t>(r,i,static_cast<std::int8_t>(b.bytes[i]));return r;}
  if(s==VectorSemantic::vupklsb||s==VectorSemantic::vupklsb128){for(unsigned i=0;i<8;++i)write_be<std::int16_t>(r,i,static_cast<std::int8_t>(b.bytes[8+i]));return r;}
  if(s==VectorSemantic::vupkhsh){for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,static_cast<std::int16_t>(b.u16_be(i)));return r;}
  if(s==VectorSemantic::vupklsh){for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,static_cast<std::int16_t>(b.u16_be(4+i)));return r;}
  return r;
}

// VMX128 D3D pack/unpack (vpkd3d128/vupkd3d128). The hardware works on the
// "magic number" float encodings D3D uses: an N-bit signed integer x is the
// float whose bits are 0x40400000 + x (3.0 + x*2^-22), an unsigned w is
// 0x3F800000 | w (1.0 + w*2^-23), and D3DCOLOR bytes are 0x3F800000 | byte.
// Results follow the xenia-project/xenia PPC hardware test vectors
// (instr_vpkd3d128.s / instr_vupkd3d128.s) bit for bit. Types:
//   0 D3DCOLOR  1 NORMSHORT2  2 NORMPACKED32 (2:10:10:10)  3 FLOAT16_2
//   4 NORMSHORT4  5 FLOAT16_4  6 NORMPACKED64 (4:20:20:20)
// Type 6 has no hardware vectors; it follows the 2:10:10:10 rules widened to
// 20-bit fields. Type 7, and a pack with mask 0, are undefined encodings:
// the destination is left as it was.

constexpr std::uint32_t kD3DThree = 0x40400000u;  // 3.0f
constexpr std::uint32_t kD3DOne = 0x3F800000u;    // 1.0f
constexpr std::uint32_t kD3DQuietNaN = 0x7FC00000u;

// x86 maxps/minps order: a NaN in `value` yields `bound`, so NaN packs as the
// saturation limit (games pack 0xFFFFFFFF to get a zero constant).
float d3d_max(float value, std::uint32_t bound) noexcept {
  const float b = std::bit_cast<float>(bound);
  return value > b ? value : b;
}
float d3d_min(float value, std::uint32_t bound) noexcept {
  const float b = std::bit_cast<float>(bound);
  return value < b ? value : b;
}
// Saturates `value` to [lo, hi] in the magic-number domain; returns the bits.
std::uint32_t d3d_saturate(float value, std::uint32_t lo, std::uint32_t hi) noexcept {
  return std::bit_cast<std::uint32_t>(d3d_min(d3d_max(value, lo), hi));
}

// float -> IEEE half, rounding toward zero (the F16C mode xenia's PPC pack
// uses): overflow gives the largest finite half, infinities and NaNs keep
// their class.
std::uint16_t float_to_half_toward_zero(float f) noexcept {
  const std::uint32_t x = std::bit_cast<std::uint32_t>(f);
  const std::uint32_t sign = (x >> 16) & 0x8000u;
  const std::uint32_t exponent = (x >> 23) & 0xFFu;
  std::uint32_t mantissa = x & 0x7FFFFFu;
  if (exponent == 0xFFu)
    return std::uint16_t(sign | 0x7C00u | (mantissa ? 0x200u | (mantissa >> 13) : 0u));
  const int e = int(exponent) - 127 + 15;
  if (e >= 31) return std::uint16_t(sign | 0x7BFFu);
  if (e <= 0) {
    if (e < -10) return std::uint16_t(sign);
    mantissa |= 0x800000u;
    return std::uint16_t(sign | (mantissa >> unsigned(14 - e)));
  }
  return std::uint16_t(sign | (unsigned(e) << 10) | (mantissa >> 13));
}

// An N-bit signed field to the magic-number float; the most negative value
// (which has no magic encoding) unpacks as a quiet NaN.
std::uint32_t d3d_unpack_signed(std::uint64_t field, unsigned bits) noexcept {
  const std::int32_t value = std::int32_t(std::uint32_t(field << (32u - bits))) >> (32u - bits);
  if (value == -(std::int32_t(1) << (bits - 1u))) return kD3DQuietNaN;
  return kD3DThree + std::uint32_t(value);
}

Vector128 d3d_pack(std::uint32_t w, const Vector128& old, const Vector128& src) {
  const unsigned imm = vx128_imm4(w), type = imm >> 2, pack = imm & 3u, shift = vx128_z4(w);
  if (type == 7u || pack == 0u) return old;
  const auto lane = [&](unsigned i) { return read_f32(src, i); };
  std::uint64_t packed = 0;  // The low 32 bits are the low guest word.
  switch (type) {
    case 0: {  // RGBA lanes -> ARGB bytes, each saturated to [3.0, 3.0+255ulp].
      std::uint32_t c[4];
      for (unsigned i = 0; i < 4; ++i) c[i] = d3d_saturate(lane(i), kD3DThree, 0x404000FFu) & 0xFFu;
      packed = (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
      break;
    }
    case 1:
      packed = (d3d_saturate(lane(0), 0x403F8001u, 0x40407FFFu) & 0xFFFFu) << 16 |
               (d3d_saturate(lane(1), 0x403F8001u, 0x40407FFFu) & 0xFFFFu);
      break;
    case 2:
      for (unsigned i = 0; i < 3; ++i)
        packed |= std::uint64_t(d3d_saturate(lane(i), 0x403FFE01u, 0x404001FFu) & 0x3FFu) << (10u * i);
      packed |= std::uint64_t(d3d_saturate(lane(3), kD3DThree, 0x40400003u) & 3u) << 30;
      break;
    case 3:
      packed = std::uint32_t(float_to_half_toward_zero(lane(0))) << 16 |
               float_to_half_toward_zero(lane(1));
      break;
    case 4:
      for (unsigned i = 0; i < 4; ++i)
        packed = (packed << 16) | (d3d_saturate(lane(i), 0x403F8001u, 0x40407FFFu) & 0xFFFFu);
      break;
    case 5:
      for (unsigned i = 0; i < 4; ++i) packed = (packed << 16) | float_to_half_toward_zero(lane(i));
      break;
    case 6:
      for (unsigned i = 0; i < 3; ++i)
        packed |= std::uint64_t(d3d_saturate(lane(i), 0x40380001u, 0x4047FFFFu) & 0xFFFFFu) << (20u * i);
      packed |= std::uint64_t(d3d_saturate(lane(3), kD3DThree, 0x4040000Fu) & 0xFu) << 60;
      break;
  }
  const auto high = std::uint32_t(packed >> 32), low = std::uint32_t(packed);
  // The mask selects a word (1) or a word pair (2, 3); the shift moves it
  // left by whole words. Every other word of vD is kept.
  Vector128 r = old;
  if (pack == 1u) {
    r.set_u32_be(3u - shift, low);
  } else if (shift < 3u) {
    r.set_u32_be(2u - shift, high);
    r.set_u32_be(3u - shift, low);
  } else if (pack == 2u) {
    r.set_u32_be(0, low);
  } else {
    r.set_u32_be(3, high);
  }
  return r;
}

Vector128 d3d_unpack(std::uint32_t w, const Vector128& old, const Vector128& src) {
  const unsigned type = vx128_imm3(w) >> 2;
  const std::uint32_t word = src.u32_be(3);
  const std::uint64_t pair = (std::uint64_t(src.u32_be(2)) << 32) | word;
  Vector128 r{};
  switch (type) {
    case 0:  // ARGB bytes -> RGBA lanes.
      r.set_u32_be(0, kD3DOne | ((word >> 16) & 0xFFu));
      r.set_u32_be(1, kD3DOne | ((word >> 8) & 0xFFu));
      r.set_u32_be(2, kD3DOne | (word & 0xFFu));
      r.set_u32_be(3, kD3DOne | (word >> 24));
      break;
    case 1:  // (x, y, 0.0, 1.0): titles splat w to get a vector of ones.
      r.set_u32_be(0, d3d_unpack_signed(word >> 16, 16));
      r.set_u32_be(1, d3d_unpack_signed(word, 16));
      r.set_u32_be(3, kD3DOne);
      break;
    case 2:
      for (unsigned i = 0; i < 3; ++i) r.set_u32_be(i, d3d_unpack_signed(word >> (10u * i), 10));
      r.set_u32_be(3, kD3DOne | (word >> 30));
      break;
    case 3:
      write_f32(r, 0, half_to_float(std::uint16_t(word >> 16)));
      write_f32(r, 1, half_to_float(std::uint16_t(word)));
      r.set_u32_be(3, kD3DOne);
      break;
    case 4:
      for (unsigned i = 0; i < 4; ++i) r.set_u32_be(i, d3d_unpack_signed(pair >> (48u - 16u * i), 16));
      break;
    case 5:
      for (unsigned i = 0; i < 4; ++i) write_f32(r, i, half_to_float(std::uint16_t(pair >> (48u - 16u * i))));
      break;
    case 6:
      for (unsigned i = 0; i < 3; ++i) r.set_u32_be(i, d3d_unpack_signed(pair >> (20u * i), 20));
      r.set_u32_be(3, kD3DOne | std::uint32_t(pair >> 60));
      break;
    default:
      return old;
  }
  return r;
}

} // namespace

Vector128 execute_vector(VectorSemantic s, std::uint32_t w, CpuState& state,
                         const Vector128& a, const Vector128& b,
                         const Vector128& c, const Vector128& d) {
  (void)d;
  Vector128 r{};
#define CASE2(x,y,expr) case VectorSemantic::x: case VectorSemantic::y: return (expr)
  switch(s){
    CASE2(vaddfp,vaddfp128,fp_binary(a,b,'+',state));
    CASE2(vsubfp,vsubfp128,fp_binary(a,b,'-',state));
    case VectorSemantic::vmulfp128:return fp_binary(a,b,'*',state);
    CASE2(vmaxfp,vmaxfp128,fp_binary(a,b,'x',state));
    CASE2(vminfp,vminfp128,fp_binary(a,b,'n',state));

    CASE2(vand,vand128,vector_logic<VectorSemantic::vand>(a,b));
    CASE2(vandc,vandc128,vector_logic<VectorSemantic::vandc>(a,b));
    CASE2(vor,vor128,vector_logic<VectorSemantic::vor>(a,b));
    CASE2(vxor,vxor128,vector_logic<VectorSemantic::vxor>(a,b));
    CASE2(vnor,vnor128,vector_logic<VectorSemantic::vnor>(a,b));

    case VectorSemantic::vaddubm:return lane_binary<std::uint8_t,std::uint16_t>(a,b,[](auto x,auto y){return x+y;});
    case VectorSemantic::vadduhm:return lane_binary<std::uint16_t,std::uint32_t>(a,b,[](auto x,auto y){return x+y;});
    case VectorSemantic::vadduwm:return lane_binary<std::uint32_t,std::uint64_t>(a,b,[](auto x,auto y){return x+y;});
    case VectorSemantic::vsububm:return lane_binary<std::uint8_t,std::uint16_t>(a,b,[](auto x,auto y){return x-y;});
    case VectorSemantic::vsubuhm:return lane_binary<std::uint16_t,std::uint32_t>(a,b,[](auto x,auto y){return x-y;});
    case VectorSemantic::vsubuwm:return lane_binary<std::uint32_t,std::uint64_t>(a,b,[](auto x,auto y){return x-y;});
    case VectorSemantic::vaddsbs:return add_sat<std::int8_t,std::int16_t>(a,b,state);
    case VectorSemantic::vaddshs:return add_sat<std::int16_t,std::int32_t>(a,b,state);
    case VectorSemantic::vaddsws:return add_sat<std::int32_t,std::int64_t>(a,b,state);
    case VectorSemantic::vaddubs:return add_sat<std::uint8_t,std::uint16_t>(a,b,state);
    case VectorSemantic::vadduhs:return add_sat<std::uint16_t,std::uint32_t>(a,b,state);
    case VectorSemantic::vadduws:return add_sat<std::uint32_t,std::uint64_t>(a,b,state);
    case VectorSemantic::vsubsbs:return sub_sat<std::int8_t,std::int16_t>(a,b,state);
    case VectorSemantic::vsubshs:return sub_sat<std::int16_t,std::int32_t>(a,b,state);
    case VectorSemantic::vsubsws:return sub_sat<std::int32_t,std::int64_t>(a,b,state);
    case VectorSemantic::vsububs:return sub_sat<std::uint8_t,std::int16_t>(a,b,state);
    case VectorSemantic::vsubuhs:return sub_sat<std::uint16_t,std::int32_t>(a,b,state);
    case VectorSemantic::vsubuws:return sub_sat<std::uint32_t,std::int64_t>(a,b,state);
    case VectorSemantic::vaddcuw:{for(unsigned i=0;i<4;++i){auto x=a.u32_be(i),y=b.u32_be(i);r.set_u32_be(i,std::uint32_t(std::uint64_t(x)+y>0xFFFFFFFFull));}return r;}
    case VectorSemantic::vsubcuw:{for(unsigned i=0;i<4;++i)r.set_u32_be(i,a.u32_be(i)>=b.u32_be(i));return r;}

    case VectorSemantic::vavgsb:return avg_round<std::int8_t,std::int16_t>(a,b);
    case VectorSemantic::vavgsh:return avg_round<std::int16_t,std::int32_t>(a,b);
    case VectorSemantic::vavgsw:return avg_round<std::int32_t,std::int64_t>(a,b);
    case VectorSemantic::vavgub:return avg_round<std::uint8_t,std::uint16_t>(a,b);
    case VectorSemantic::vavguh:return avg_round<std::uint16_t,std::uint32_t>(a,b);
    case VectorSemantic::vavguw:return avg_round<std::uint32_t,std::uint64_t>(a,b);

#define MINMAX_CASE(name,T,ismax) case VectorSemantic::name:return minmax<T>(a,b,ismax)
    MINMAX_CASE(vmaxsb,std::int8_t,true);MINMAX_CASE(vmaxsh,std::int16_t,true);MINMAX_CASE(vmaxsw,std::int32_t,true);MINMAX_CASE(vmaxub,std::uint8_t,true);MINMAX_CASE(vmaxuh,std::uint16_t,true);MINMAX_CASE(vmaxuw,std::uint32_t,true);
    MINMAX_CASE(vminsb,std::int8_t,false);MINMAX_CASE(vminsh,std::int16_t,false);MINMAX_CASE(vminsw,std::int32_t,false);MINMAX_CASE(vminub,std::uint8_t,false);MINMAX_CASE(vminuh,std::uint16_t,false);MINMAX_CASE(vminuw,std::uint32_t,false);
#undef MINMAX_CASE

#define CMP_EQ(name,T) case VectorSemantic::name:return compare_lanes<T>(a,b,[](auto x,auto y){return x==y;})
#define CMP_GT(name,T) case VectorSemantic::name:return compare_lanes<T>(a,b,[](auto x,auto y){return x>y;})
    CMP_EQ(vcmpequb,std::uint8_t);CMP_EQ(vcmpequh,std::uint16_t);CMP_EQ(vcmpequw,std::uint32_t);CMP_EQ(vcmpequw128,std::uint32_t);
    CMP_GT(vcmpgtsb,std::int8_t);CMP_GT(vcmpgtsh,std::int16_t);CMP_GT(vcmpgtsw,std::int32_t);CMP_GT(vcmpgtub,std::uint8_t);CMP_GT(vcmpgtuh,std::uint16_t);CMP_GT(vcmpgtuw,std::uint32_t);
#undef CMP_EQ
#undef CMP_GT
    case VectorSemantic::vcmpeqfp:case VectorSemantic::vcmpeqfp128:for(unsigned i=0;i<4;++i)r.set_u32_be(i,read_vmx_f32(a,i,state)==read_vmx_f32(b,i,state)?~0u:0u);return r;
    case VectorSemantic::vcmpgefp:case VectorSemantic::vcmpgefp128:for(unsigned i=0;i<4;++i)r.set_u32_be(i,read_vmx_f32(a,i,state)>=read_vmx_f32(b,i,state)?~0u:0u);return r;
    case VectorSemantic::vcmpgtfp:case VectorSemantic::vcmpgtfp128:for(unsigned i=0;i<4;++i)r.set_u32_be(i,read_vmx_f32(a,i,state)>read_vmx_f32(b,i,state)?~0u:0u);return r;
    case VectorSemantic::vcmpbfp:case VectorSemantic::vcmpbfp128:for(unsigned i=0;i<4;++i){float x=read_vmx_f32(a,i,state),bound=read_vmx_f32(b,i,state);std::uint32_t z=0;if(std::isnan(x)||std::isnan(bound))z=0xC0000000u;else{if(x>bound)z|=0x80000000u;if(x<-bound)z|=0x40000000u;}r.set_u32_be(i,z);}return r;

    case VectorSemantic::vrefp:case VectorSemantic::vrefp128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,1.0f/read_vmx_f32(a,i,state),state);return r;
    case VectorSemantic::vrsqrtefp:case VectorSemantic::vrsqrtefp128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,1.0f/std::sqrt(read_vmx_f32(a,i,state)),state);return r;
    case VectorSemantic::vexptefp:case VectorSemantic::vexptefp128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::exp2(read_vmx_f32(a,i,state)),state);return r;
    case VectorSemantic::vlogefp:case VectorSemantic::vlogefp128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::log2(read_vmx_f32(a,i,state)),state);return r;
    case VectorSemantic::vrfim:case VectorSemantic::vrfim128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::floor(read_vmx_f32(a,i,state)),state);return r;
    case VectorSemantic::vrfin:case VectorSemantic::vrfin128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,round_nearest_even(read_vmx_f32(a,i,state)),state);return r;
    case VectorSemantic::vrfip:case VectorSemantic::vrfip128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::ceil(read_vmx_f32(a,i,state)),state);return r;
    case VectorSemantic::vrfiz:case VectorSemantic::vrfiz128:for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::trunc(read_vmx_f32(a,i,state)),state);return r;

    case VectorSemantic::vcfsx:case VectorSemantic::vcsxwfp128:{unsigned sh=(s==VectorSemantic::vcfsx)?vx_imm5(w):vx128_imm3(w);float scale=std::ldexp(1.0f,-int(sh));for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,float(read_be<std::int32_t>(a,i))*scale,state);return r;}
    case VectorSemantic::vcfux:case VectorSemantic::vcuxwfp128:{unsigned sh=(s==VectorSemantic::vcfux)?vx_imm5(w):vx128_imm3(w);float scale=std::ldexp(1.0f,-int(sh));for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,float(read_be<std::uint32_t>(a,i))*scale,state);return r;}
    case VectorSemantic::vctsxs:case VectorSemantic::vcfpsxws128:{unsigned sh=(s==VectorSemantic::vctsxs)?vx_imm5(w):vx128_imm3(w);float scale=std::ldexp(1.0f,int(sh));for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,vector_fp_to_int_sat<std::int32_t>(read_vmx_f32(a,i,state)*scale,state));return r;}
    case VectorSemantic::vctuxs:case VectorSemantic::vcfpuxws128:{unsigned sh=(s==VectorSemantic::vctuxs)?vx_imm5(w):vx128_imm3(w);float scale=std::ldexp(1.0f,int(sh));for(unsigned i=0;i<4;++i)write_be<std::uint32_t>(r,i,vector_fp_to_int_sat<std::uint32_t>(read_vmx_f32(a,i,state)*scale,state));return r;}

    case VectorSemantic::vmaddfp:{for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::fma(read_vmx_f32(a,i,state),read_vmx_f32(c,i,state),read_vmx_f32(b,i,state)),state);return r;}
    case VectorSemantic::vmaddfp128:{for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::fma(read_vmx_f32(b,i,state),read_vmx_f32(c,i,state),read_vmx_f32(a,i,state)),state);return r;}
    case VectorSemantic::vmaddcfp128:{for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,std::fma(read_vmx_f32(b,i,state),read_vmx_f32(a,i,state),read_vmx_f32(c,i,state)),state);return r;}
    case VectorSemantic::vnmsubfp:{for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,-std::fma(read_vmx_f32(a,i,state),read_vmx_f32(c,i,state),-read_vmx_f32(b,i,state)),state);return r;}
    // VMX128 inputs are (old vD, vA, vB): vD = -(vA * vB - old vD).
    case VectorSemantic::vnmsubfp128:{for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,-std::fma(read_vmx_f32(b,i,state),read_vmx_f32(c,i,state),-read_vmx_f32(a,i,state)),state);return r;}
    case VectorSemantic::vmsum3fp128:{float z=0;for(unsigned i=0;i<3;++i)z+=read_vmx_f32(b,i,state)*read_vmx_f32(c,i,state);for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,z,state);return r;}
    case VectorSemantic::vmsum4fp128:{float z=0;for(unsigned i=0;i<4;++i)z+=read_vmx_f32(b,i,state)*read_vmx_f32(c,i,state);for(unsigned i=0;i<4;++i)write_vmx_f32(r,i,z,state);return r;}

    case VectorSemantic::vmladduhm:{for(unsigned i=0;i<8;++i)r.set_u16_be(i,std::uint16_t(std::uint32_t(a.u16_be(i))*b.u16_be(i)+c.u16_be(i)));return r;}
    case VectorSemantic::vmhaddshs:case VectorSemantic::vmhraddshs:{for(unsigned i=0;i<8;++i){std::int32_t p=std::int32_t(read_be<std::int16_t>(a,i))*read_be<std::int16_t>(b,i);if(s==VectorSemantic::vmhraddshs)p+=0x4000;p=(p>>15)+read_be<std::int16_t>(c,i);write_be<std::int16_t>(r,i,sat_cast<std::int16_t>(p,state));}return r;}

    case VectorSemantic::vmuleub:case VectorSemantic::vmuloub:{const unsigned off=s==VectorSemantic::vmuleub?0:1;for(unsigned i=0;i<8;++i)r.set_u16_be(i,std::uint16_t(a.bytes[2*i+off])*b.bytes[2*i+off]);return r;}
    case VectorSemantic::vmulesb:case VectorSemantic::vmulosb:{const unsigned off=s==VectorSemantic::vmulesb?0:1;for(unsigned i=0;i<8;++i)write_be<std::int16_t>(r,i,std::int16_t(std::int8_t(a.bytes[2*i+off]))*std::int8_t(b.bytes[2*i+off]));return r;}
    case VectorSemantic::vmuleuh:case VectorSemantic::vmulouh:{const unsigned off=s==VectorSemantic::vmuleuh?0:1;for(unsigned i=0;i<4;++i)r.set_u32_be(i,std::uint32_t(a.u16_be(2*i+off))*b.u16_be(2*i+off));return r;}
    case VectorSemantic::vmulesh:case VectorSemantic::vmulosh:{const unsigned off=s==VectorSemantic::vmulesh?0:1;for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,std::int32_t(read_be<std::int16_t>(a,2*i+off))*read_be<std::int16_t>(b,2*i+off));return r;}

    case VectorSemantic::vmrghb:case VectorSemantic::vmrglb:{unsigned base=s==VectorSemantic::vmrghb?0:8;for(unsigned i=0;i<8;++i){r.bytes[2*i]=a.bytes[base+i];r.bytes[2*i+1]=b.bytes[base+i];}return r;}
    case VectorSemantic::vmrghh:case VectorSemantic::vmrglh:{unsigned base=s==VectorSemantic::vmrghh?0:4;for(unsigned i=0;i<4;++i){r.set_u16_be(2*i,a.u16_be(base+i));r.set_u16_be(2*i+1,b.u16_be(base+i));}return r;}
    case VectorSemantic::vmrghw:case VectorSemantic::vmrghw128:case VectorSemantic::vmrglw:case VectorSemantic::vmrglw128:{bool hi=s==VectorSemantic::vmrghw||s==VectorSemantic::vmrghw128;unsigned base=hi?0:2;for(unsigned i=0;i<2;++i){r.set_u32_be(2*i,a.u32_be(base+i));r.set_u32_be(2*i+1,b.u32_be(base+i));}return r;}

    case VectorSemantic::vperm:case VectorSemantic::vperm128:return permute_bytes(a,b,c);
    case VectorSemantic::vpermwi128:{unsigned p=vx128_perm(w);for(unsigned i=0;i<4;++i)r.set_u32_be(i,a.u32_be((p>>(6-2*i))&3u));return r;}
    case VectorSemantic::vsel:case VectorSemantic::vsel128:return vector_select(a,b,c);

    case VectorSemantic::vspltb:{unsigned n=vx_imm5(w)&15u;std::fill(r.bytes.begin(),r.bytes.end(),a.bytes[n]);return r;}
    case VectorSemantic::vsplth:{unsigned n=vx_imm5(w)&7u;for(unsigned i=0;i<8;++i)r.set_u16_be(i,a.u16_be(n));return r;}
    case VectorSemantic::vspltw:{unsigned n=vx_imm5(w)&3u;for(unsigned i=0;i<4;++i)r.set_u32_be(i,a.u32_be(n));return r;}
    case VectorSemantic::vspltw128:{unsigned n=vx128_imm3(w)&3u;for(unsigned i=0;i<4;++i)r.set_u32_be(i,a.u32_be(n));return r;}
    case VectorSemantic::vspltisb:{auto z=std::int8_t(sext5(vx_imm5(w)));std::fill(r.bytes.begin(),r.bytes.end(),std::uint8_t(z));return r;}
    case VectorSemantic::vspltish:{auto z=std::int16_t(sext5(vx_imm5(w)));for(unsigned i=0;i<8;++i)write_be<std::int16_t>(r,i,z);return r;}
    case VectorSemantic::vspltisw:{auto z=std::int32_t(sext5(vx_imm5(w)));for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,z);return r;}
    case VectorSemantic::vspltisw128:{auto z=std::int32_t(sext5(vx128_imm3(w)));for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,z);return r;}

    case VectorSemantic::vrlb:for(unsigned i=0;i<16;++i)r.bytes[i]=std::rotl(a.bytes[i],int(b.bytes[i]&7u));return r;
    case VectorSemantic::vrlh:for(unsigned i=0;i<8;++i)r.set_u16_be(i,std::rotl(a.u16_be(i),int(b.u16_be(i)&15u)));return r;
    case VectorSemantic::vrlw:case VectorSemantic::vrlw128:for(unsigned i=0;i<4;++i)r.set_u32_be(i,std::rotl(a.u32_be(i),int(b.u32_be(i)&31u)));return r;
    case VectorSemantic::vrlimi128:{
      r=a; const unsigned rotate=vx128_z4(w)&3u; const unsigned select=vx128_imm4(w)&15u;
      for(unsigned i=0;i<4;++i) if(select&(1u<<(3-i))) r.set_u32_be(i,b.u32_be((i+rotate)&3u));
      return r;
    }

    case VectorSemantic::vslb:for(unsigned i=0;i<16;++i)r.bytes[i]=std::uint8_t(a.bytes[i]<<(b.bytes[i]&7u));return r;
    case VectorSemantic::vslh:for(unsigned i=0;i<8;++i)r.set_u16_be(i,std::uint16_t(a.u16_be(i)<<(b.u16_be(i)&15u)));return r;
    case VectorSemantic::vslw:case VectorSemantic::vslw128:for(unsigned i=0;i<4;++i)r.set_u32_be(i,a.u32_be(i)<<(b.u32_be(i)&31u));return r;
    case VectorSemantic::vsrb:for(unsigned i=0;i<16;++i)r.bytes[i]=std::uint8_t(a.bytes[i]>>(b.bytes[i]&7u));return r;
    case VectorSemantic::vsrh:for(unsigned i=0;i<8;++i)r.set_u16_be(i,std::uint16_t(a.u16_be(i)>>(b.u16_be(i)&15u)));return r;
    case VectorSemantic::vsrw:case VectorSemantic::vsrw128:for(unsigned i=0;i<4;++i)r.set_u32_be(i,a.u32_be(i)>>(b.u32_be(i)&31u));return r;
    case VectorSemantic::vsrab:for(unsigned i=0;i<16;++i)r.bytes[i]=std::uint8_t(std::int8_t(a.bytes[i])>>(b.bytes[i]&7u));return r;
    case VectorSemantic::vsrah:for(unsigned i=0;i<8;++i)write_be<std::int16_t>(r,i,std::int16_t(read_be<std::int16_t>(a,i)>>(b.u16_be(i)&15u)));return r;
    case VectorSemantic::vsraw:case VectorSemantic::vsraw128:for(unsigned i=0;i<4;++i)write_be<std::int32_t>(r,i,read_be<std::int32_t>(a,i)>>(b.u32_be(i)&31u));return r;
    case VectorSemantic::vsl:return whole_shift(a,b.bytes[15]&7u,true);
    case VectorSemantic::vsr:return whole_shift(a,b.bytes[15]&7u,false);
    case VectorSemantic::vslo:case VectorSemantic::vslo128:{unsigned n=(b.bytes[15]&0x78u)>>3;for(unsigned i=0;i<16;++i)r.bytes[i]=i+n<16?a.bytes[i+n]:0;return r;}
    case VectorSemantic::vsro:case VectorSemantic::vsro128:{unsigned n=(b.bytes[15]&0x78u)>>3;for(unsigned i=0;i<16;++i)r.bytes[i]=i>=n?a.bytes[i-n]:0;return r;}
    case VectorSemantic::vsldoi:return shift_double(a,b,va_sh4(w));
    case VectorSemantic::vsldoi128:return shift_double(a,b,vx128_sh5(w));

    case VectorSemantic::vpkuhum:case VectorSemantic::vpkuhum128:case VectorSemantic::vpkuhus:case VectorSemantic::vpkuhus128:case VectorSemantic::vpkshss:case VectorSemantic::vpkshss128:case VectorSemantic::vpkshus:case VectorSemantic::vpkshus128:
    case VectorSemantic::vpkuwum:case VectorSemantic::vpkuwum128:case VectorSemantic::vpkuwus:case VectorSemantic::vpkuwus128:case VectorSemantic::vpkswss:case VectorSemantic::vpkswss128:case VectorSemantic::vpkswus:case VectorSemantic::vpkswus128:return pack_standard(s,a,b,state);
    case VectorSemantic::vupkhsb:case VectorSemantic::vupkhsb128:case VectorSemantic::vupklsb:case VectorSemantic::vupklsb128:case VectorSemantic::vupkhsh:case VectorSemantic::vupklsh:return unpack_standard(s,a);
    // (old vD, vB), as lifter/vector.cpp and the interpreter wire them.
    case VectorSemantic::vpkd3d128:return d3d_pack(w,a,b);
    case VectorSemantic::vupkd3d128:return d3d_unpack(w,a,b);
    case VectorSemantic::vpkpx:{for(unsigned i=0;i<4;++i){auto conv=[](std::uint32_t x){return std::uint16_t(((x>>16)&0x8000u)|((x>>9)&0x7C00u)|((x>>6)&0x03E0u)|((x>>3)&0x001Fu));};r.set_u16_be(i,conv(a.u32_be(i)));r.set_u16_be(4+i,conv(b.u32_be(i)));}return r;}
    case VectorSemantic::vupkhpx:case VectorSemantic::vupklpx:{unsigned base=s==VectorSemantic::vupkhpx?0:4;for(unsigned i=0;i<4;++i){auto x=a.u16_be(base+i);std::uint32_t y=((x&0x8000u)<<16)|((x&0x7C00u)<<9)|((x&0x03E0u)<<6)|((x&0x001Fu)<<3);r.set_u32_be(i,y);}return r;}

    case VectorSemantic::vsum4ubs:{for(unsigned i=0;i<4;++i){std::uint64_t z=b.u32_be(i);for(unsigned j=0;j<4;++j)z+=a.bytes[i*4+j];r.set_u32_be(i,sat_cast<std::uint32_t>(z,state));}return r;}
    case VectorSemantic::vsum4sbs:{for(unsigned i=0;i<4;++i){std::int64_t z=read_be<std::int32_t>(b,i);for(unsigned j=0;j<4;++j)z+=std::int8_t(a.bytes[i*4+j]);write_be<std::int32_t>(r,i,sat_cast<std::int32_t>(z,state));}return r;}
    case VectorSemantic::vsum4shs:{for(unsigned i=0;i<4;++i){std::int64_t z=read_be<std::int32_t>(b,i)+read_be<std::int16_t>(a,2*i)+read_be<std::int16_t>(a,2*i+1);write_be<std::int32_t>(r,i,sat_cast<std::int32_t>(z,state));}return r;}
    case VectorSemantic::vsum2sws:{for(unsigned i=0;i<2;++i){std::int64_t z=read_be<std::int32_t>(b,2*i+1)+read_be<std::int32_t>(a,2*i)+read_be<std::int32_t>(a,2*i+1);write_be<std::int32_t>(r,2*i+1,sat_cast<std::int32_t>(z,state));}return r;}
    case VectorSemantic::vsumsws:{std::int64_t z=read_be<std::int32_t>(b,3);for(unsigned i=0;i<4;++i)z+=read_be<std::int32_t>(a,i);write_be<std::int32_t>(r,3,sat_cast<std::int32_t>(z,state));return r;}

    case VectorSemantic::vmsummbm:{for(unsigned i=0;i<4;++i){std::int64_t z=read_be<std::int32_t>(c,i);for(unsigned j=0;j<4;++j)z+=std::int8_t(a.bytes[4*i+j])*b.bytes[4*i+j];write_be<std::int32_t>(r,i,std::int32_t(z));}return r;}
    case VectorSemantic::vmsumubm:{for(unsigned i=0;i<4;++i){std::uint64_t z=c.u32_be(i);for(unsigned j=0;j<4;++j)z+=std::uint64_t(a.bytes[4*i+j])*b.bytes[4*i+j];r.set_u32_be(i,std::uint32_t(z));}return r;}
    case VectorSemantic::vmsumuhm:case VectorSemantic::vmsumuhs:{for(unsigned i=0;i<4;++i){std::uint64_t z=c.u32_be(i)+std::uint64_t(a.u16_be(2*i))*b.u16_be(2*i)+std::uint64_t(a.u16_be(2*i+1))*b.u16_be(2*i+1);r.set_u32_be(i,s==VectorSemantic::vmsumuhs?sat_cast<std::uint32_t>(z,state):std::uint32_t(z));}return r;}
    case VectorSemantic::vmsumshm:case VectorSemantic::vmsumshs:{for(unsigned i=0;i<4;++i){std::int64_t z=read_be<std::int32_t>(c,i)+std::int64_t(read_be<std::int16_t>(a,2*i))*read_be<std::int16_t>(b,2*i)+std::int64_t(read_be<std::int16_t>(a,2*i+1))*read_be<std::int16_t>(b,2*i+1);write_be<std::int32_t>(r,i,s==VectorSemantic::vmsumshs?sat_cast<std::int32_t>(z,state):std::int32_t(z));}return r;}

    // lvsl/lvsr are represented by dedicated address-derived helpers and should not reach here.
    case VectorSemantic::lvsl:case VectorSemantic::lvsl128:case VectorSemantic::lvsr:case VectorSemantic::lvsr128:return a;
  }
#undef CASE2
  return r;
}

Vector128 vector_load_shift_left(std::uint64_t ea) noexcept { Vector128 r{};unsigned n=unsigned(ea&15u);for(unsigned i=0;i<16;++i)r.bytes[i]=std::uint8_t((n+i)&31u);return r; }
Vector128 vector_load_shift_right(std::uint64_t ea) noexcept { Vector128 r{};unsigned n=unsigned(ea&15u);for(unsigned i=0;i<16;++i)r.bytes[i]=std::uint8_t((16u-n+i)&31u);return r; }

Vector128 vector_load_element(const Vector128& old, MemoryAccessContext& m, GuestAddress ea,unsigned width){Vector128 r=old;unsigned p=unsigned(ea&15u);if(width==1)r.bytes[p]=m.read8(ea);else if(width==2){ea&=~GuestAddress{1};unsigned lane=(p&14u)/2;r.set_u16_be(lane,m.read16_be(ea));}else{ea&=~GuestAddress{3};unsigned lane=(p&12u)/4;r.set_u32_be(lane,m.read32_be(ea));}return r;}
void vector_store_element(const Vector128& v,MemoryAccessContext&m,GuestAddress ea,unsigned width){unsigned p=unsigned(ea&15u);if(width==1)m.write8(ea,v.bytes[p]);else if(width==2){ea&=~GuestAddress{1};m.write16_be(ea,v.u16_be((p&14u)/2));}else{ea&=~GuestAddress{3};m.write32_be(ea,v.u32_be((p&12u)/4));}}
Vector128 vector_load_left(const Vector128& old, MemoryAccessContext& m,
                           GuestAddress ea) {
  (void)old;
  Vector128 result{};
  const auto n = static_cast<unsigned>(ea & 15u);
  auto bytes = std::as_writable_bytes(std::span(result.bytes));
  m.read_bytes(ea, bytes.first(16u - n));
  return result;
}

Vector128 vector_load_right(const Vector128& old, MemoryAccessContext& m,
                            GuestAddress ea) {
  (void)old;
  Vector128 result{};
  const auto base = GuestAddress(ea & ~15u);
  const auto n = static_cast<unsigned>(ea & 15u);
  auto bytes = std::as_writable_bytes(std::span(result.bytes));
  if (n) m.read_bytes(base, bytes.subspan(16u - n, n));
  return result;
}

void vector_store_left(const Vector128& value, MemoryAccessContext& m,
                       GuestAddress ea) {
  const auto n = static_cast<unsigned>(ea & 15u);
  const auto bytes = std::as_bytes(std::span(value.bytes));
  m.write_bytes(ea, bytes.first(16u - n));
}

void vector_store_right(const Vector128& value, MemoryAccessContext& m,
                        GuestAddress ea) {
  const auto base = GuestAddress(ea & ~15u);
  const auto n = static_cast<unsigned>(ea & 15u);
  const auto bytes = std::as_bytes(std::span(value.bytes));
  if (n) m.write_bytes(base, bytes.subspan(16u - n, n));
}

void string_load(CpuState& state, MemoryAccessContext& memory, GuestAddress ea,
                 std::uint32_t count, unsigned first) {
  std::array<std::byte, 128> buffer{};
  std::uint32_t offset = 0;
  while (offset < count) {
    const auto chunk = std::min<std::uint32_t>(
        count - offset, static_cast<std::uint32_t>(buffer.size()));
    auto bytes = std::span<std::byte>(buffer).first(chunk);
    memory.read_bytes(ea + offset, bytes);
    for (std::uint32_t i = 0; i < chunk; ++i) {
      const auto n = offset + i;
      const auto reg = (first + n / 4u) & 31u;
      const auto pos = n & 3u;
      if (pos == 0u) state.gpr[reg] = 0;
      state.gpr[reg] |= std::uint64_t(std::to_integer<std::uint8_t>(bytes[i]))
                        << (24u - 8u * pos);
    }
    offset += chunk;
  }
}

void string_store(CpuState& state, MemoryAccessContext& memory, GuestAddress ea,
                  std::uint32_t count, unsigned first) {
  std::array<std::byte, 128> buffer{};
  std::uint32_t offset = 0;
  while (offset < count) {
    const auto chunk = std::min<std::uint32_t>(
        count - offset, static_cast<std::uint32_t>(buffer.size()));
    auto bytes = std::span<std::byte>(buffer).first(chunk);
    for (std::uint32_t i = 0; i < chunk; ++i) {
      const auto n = offset + i;
      const auto reg = (first + n / 4u) & 31u;
      const auto pos = n & 3u;
      bytes[i] = static_cast<std::byte>(
          static_cast<std::uint8_t>(state.gpr[reg] >> (24u - 8u * pos)));
    }
    memory.write_bytes(ea + offset, bytes);
    offset += chunk;
  }
}

} // namespace xenon::cpu::aot
