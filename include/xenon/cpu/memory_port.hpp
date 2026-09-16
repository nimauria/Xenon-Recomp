#pragma once

#include <cstdint>
#include "xenon/cpu/types.hpp"

namespace xenon::cpu {

enum class BarrierKind : std::uint8_t {
  Sync,
  LightweightSync,
  Eieio,
  InstructionSync,
};

// Deliberately tiny CPU-facing contract. This is NOT the RAM subsystem.
class MemoryPort {
 public:
  virtual ~MemoryPort() = default;

  virtual std::uint8_t read8(GuestAddress address) = 0;
  virtual std::uint16_t read16_be(GuestAddress address) = 0;
  virtual std::uint32_t read32_be(GuestAddress address) = 0;
  virtual std::uint64_t read64_be(GuestAddress address) = 0;
  virtual Vector128 read128(GuestAddress address) = 0;

  virtual void write8(GuestAddress address, std::uint8_t value) = 0;
  virtual void write16_be(GuestAddress address, std::uint16_t value) = 0;
  virtual void write32_be(GuestAddress address, std::uint32_t value) = 0;
  virtual void write64_be(GuestAddress address, std::uint64_t value) = 0;
  virtual void write128(GuestAddress address, const Vector128& value) = 0;

  // Byte-reversed PPC instructions are explicitly little-endian relative to
  // normal big-endian guest accesses.
  virtual std::uint16_t read16_le(GuestAddress address) = 0;
  virtual std::uint32_t read32_le(GuestAddress address) = 0;
  virtual std::uint64_t read64_le(GuestAddress address) = 0;
  virtual void write16_le(GuestAddress address, std::uint16_t value) = 0;
  virtual void write32_le(GuestAddress address, std::uint32_t value) = 0;
  virtual void write64_le(GuestAddress address, std::uint64_t value) = 0;

  // Reservation operations return a memory-generation token. The future RAM
  // module decides how reservations are invalidated across hardware threads.
  virtual std::uint64_t reserve32(GuestAddress address, std::uint32_t& value) = 0;
  virtual std::uint64_t reserve64(GuestAddress address, std::uint64_t& value) = 0;
  virtual bool store_conditional32(GuestAddress address, std::uint64_t token,
                                   std::uint32_t value) = 0;
  virtual bool store_conditional64(GuestAddress address, std::uint64_t token,
                                   std::uint64_t value) = 0;

  virtual void barrier(BarrierKind kind) = 0;
  virtual void zero_cache_block(GuestAddress address, std::uint32_t bytes) = 0;
  virtual void instruction_cache_invalidate(GuestAddress address) = 0;
};

}  // namespace xenon::cpu
