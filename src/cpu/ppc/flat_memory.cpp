#include "xenon/cpu/flat_memory.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace xenon::cpu {

FlatMemory::FlatMemory(std::size_t size, GuestAddress base)
    : base_(base), bytes_(size) {}

std::size_t FlatMemory::offset(GuestAddress address, std::size_t width) const {
  const std::uint64_t a = address;
  const std::uint64_t b = base_;
  if (a < b || a + width > b + bytes_.size()) {
    throw std::out_of_range("FlatMemory guest access outside backing range");
  }
  return static_cast<std::size_t>(a - b);
}

void FlatMemory::touched() {
  ++generation_;
  if (generation_ == 0) generation_ = 1;
}

std::uint8_t FlatMemory::read8(GuestAddress a) { return bytes_[offset(a,1)]; }
std::uint16_t FlatMemory::read16_be(GuestAddress a) {
  auto p=offset(a,2); return std::uint16_t((std::uint16_t(bytes_[p])<<8)|bytes_[p+1]);
}
std::uint32_t FlatMemory::read32_be(GuestAddress a) {
  auto p=offset(a,4); return (std::uint32_t(bytes_[p])<<24)|(std::uint32_t(bytes_[p+1])<<16)|
      (std::uint32_t(bytes_[p+2])<<8)|bytes_[p+3];
}
std::uint64_t FlatMemory::read64_be(GuestAddress a) {
  auto p=offset(a,8); std::uint64_t v=0; for(unsigned i=0;i<8;++i) v=(v<<8)|bytes_[p+i]; return v;
}
Vector128 FlatMemory::read128(GuestAddress a) {
  auto p=offset(a,16); Vector128 v{}; std::copy_n(bytes_.begin()+static_cast<std::ptrdiff_t>(p),16,v.bytes.begin()); return v;
}

void FlatMemory::write8(GuestAddress a,std::uint8_t v){ bytes_[offset(a,1)]=v; touched(); }
void FlatMemory::write16_be(GuestAddress a,std::uint16_t v){ auto p=offset(a,2); bytes_[p]=std::uint8_t(v>>8); bytes_[p+1]=std::uint8_t(v); touched(); }
void FlatMemory::write32_be(GuestAddress a,std::uint32_t v){ auto p=offset(a,4); for(unsigned i=0;i<4;++i) bytes_[p+i]=std::uint8_t(v>>(24-8*i)); touched(); }
void FlatMemory::write64_be(GuestAddress a,std::uint64_t v){ auto p=offset(a,8); for(unsigned i=0;i<8;++i) bytes_[p+i]=std::uint8_t(v>>(56-8*i)); touched(); }
void FlatMemory::write128(GuestAddress a,const Vector128& v){ auto p=offset(a,16); std::copy(v.bytes.begin(),v.bytes.end(),bytes_.begin()+static_cast<std::ptrdiff_t>(p)); touched(); }

std::uint16_t FlatMemory::read16_le(GuestAddress a){ auto p=offset(a,2); return std::uint16_t(bytes_[p]|(std::uint16_t(bytes_[p+1])<<8)); }
std::uint32_t FlatMemory::read32_le(GuestAddress a){ auto p=offset(a,4); std::uint32_t v=0; for(unsigned i=0;i<4;++i) v|=std::uint32_t(bytes_[p+i])<<(8*i); return v; }
std::uint64_t FlatMemory::read64_le(GuestAddress a){ auto p=offset(a,8); std::uint64_t v=0; for(unsigned i=0;i<8;++i) v|=std::uint64_t(bytes_[p+i])<<(8*i); return v; }
void FlatMemory::write16_le(GuestAddress a,std::uint16_t v){ auto p=offset(a,2); bytes_[p]=std::uint8_t(v); bytes_[p+1]=std::uint8_t(v>>8); touched(); }
void FlatMemory::write32_le(GuestAddress a,std::uint32_t v){ auto p=offset(a,4); for(unsigned i=0;i<4;++i) bytes_[p+i]=std::uint8_t(v>>(8*i)); touched(); }
void FlatMemory::write64_le(GuestAddress a,std::uint64_t v){ auto p=offset(a,8); for(unsigned i=0;i<8;++i) bytes_[p+i]=std::uint8_t(v>>(8*i)); touched(); }

std::uint64_t FlatMemory::reserve32(GuestAddress a,std::uint32_t& v){ v=read32_be(a); return generation_; }
std::uint64_t FlatMemory::reserve64(GuestAddress a,std::uint64_t& v){ v=read64_be(a); return generation_; }
bool FlatMemory::store_conditional32(GuestAddress a,std::uint64_t token,std::uint32_t v){ if(token!=generation_) return false; write32_be(a,v); return true; }
bool FlatMemory::store_conditional64(GuestAddress a,std::uint64_t token,std::uint64_t v){ if(token!=generation_) return false; write64_be(a,v); return true; }

void FlatMemory::barrier(BarrierKind){ std::atomic_thread_fence(std::memory_order_seq_cst); }
void FlatMemory::zero_cache_block(GuestAddress a,std::uint32_t n){ auto aligned=GuestAddress(a & ~(n-1u)); auto p=offset(aligned,n); std::fill_n(bytes_.begin()+static_cast<std::ptrdiff_t>(p),n,std::uint8_t{0}); touched(); }
void FlatMemory::instruction_cache_invalidate(GuestAddress){ /* no code cache in FlatMemory */ }

}  // namespace xenon::cpu
