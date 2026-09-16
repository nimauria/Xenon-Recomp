#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "xenon/gpu/shader.hpp"
#include "xenon/gpu/types.hpp"

namespace xenon::gpu::ir {

struct RegisterWrite {
  std::uint32_t index{};
  std::uint32_t value{};
};

struct PhysicalMemoryWrite {
  std::uint32_t physical_address{};
  std::uint32_t value{};
  Endian endian{Endian::None};
};

struct IndirectBuffer {
  std::uint32_t physical_address{};
  std::uint32_t dword_count{};
  bool prefetch{};
};

struct DrawPacket {
  Type3Opcode opcode{};
  bool predicate{};
  std::uint64_t register_generation{};
  std::vector<std::uint32_t> payload{};
};

struct ShaderLoad {
  Type3Opcode opcode{};
  bool immediate{};
  std::uint32_t physical_address{};
  ShaderProgram program{};
  std::vector<std::uint32_t> raw_payload{};
};

struct ShaderPacket {
  Type3Opcode opcode{};
  std::vector<std::uint32_t> payload{};
};

struct SynchronizationPacket {
  Type3Opcode opcode{};
  std::vector<std::uint32_t> payload{};
};

struct EventPacket {
  Type3Opcode opcode{};
  std::vector<std::uint32_t> payload{};
};

struct StatePacket {
  Type3Opcode opcode{};
  std::vector<std::uint32_t> payload{};
};

// Lossless representation for recognized PM4 packets whose high-level Xenos
// semantics have not yet been lowered. Keeping the full payload is deliberate:
// packets are never silently discarded merely because the host backend does not
// consume them yet.
struct Type3Packet {
  Type3Opcode opcode{};
  bool predicate{};
  std::vector<std::uint32_t> payload{};
};

using Command = std::variant<RegisterWrite, PhysicalMemoryWrite, IndirectBuffer,
                             DrawPacket, ShaderLoad, ShaderPacket, SynchronizationPacket,
                             EventPacket, StatePacket, Type3Packet>;

class Stream {
 public:
  template <typename T>
  void emit(T command) {
    commands_.emplace_back(std::move(command));
  }

  void clear() { commands_.clear(); }
  [[nodiscard]] const std::vector<Command>& commands() const noexcept {
    return commands_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

 private:
  std::vector<Command> commands_{};
};

}  // namespace xenon::gpu::ir
