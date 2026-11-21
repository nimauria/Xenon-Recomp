#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

#include "xenon/gpu/shader.hpp"
#include "xenon/gpu/shader_ir.hpp"
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

struct ShaderReference {
  bool valid{};
  std::uint64_t hash{};
  std::uint32_t start_slot{};
};

struct IndexBufferReference {
  bool valid{};
  std::uint32_t physical_address{};
  std::uint32_t length_bytes{};
  std::uint32_t index_count{};
  IndexFormat format{IndexFormat::UInt16};
  Endian endian{Endian::None};
};

// Xenos bin/visibility metadata carried by DRAW_INDX_BIN / DRAW_INDX_2_BIN.
// Native host renderers do not replay the original hardware binning pass, but
// retaining the fully decoded state keeps the frontend lossless and makes it
// available to diagnostics or title research without exposing PM4 to backends.
struct BinningReference {
  bool valid{};
  std::uint32_t base{};
  std::uint32_t size{};
  std::uint32_t base_offset{};
  std::uint32_t effective_base{};
  std::uint64_t mask{};
  std::uint64_t select{};
};

// Host-independent lowering of a Xenos draw packet. The backend gets the
// actual draw semantics and shader identities directly rather than re-decoding
// PM4. raw_payload is retained only for diagnostics/research; normal rendering
// must use the normalized fields below.
struct DrawPacket {
  Type3Opcode opcode{};
  bool predicate{};
  std::uint64_t register_generation{};

  PrimitiveType primitive_type{PrimitiveType::None};
  DrawSource source{DrawSource::Reserved};
  MajorMode major_mode{MajorMode::Implicit};
  bool explicit_major_mode{};
  IndexFormat index_format{IndexFormat::UInt16};
  bool not_eop{};
  bool binned{};
  std::uint32_t index_count{};
  std::uint32_t viz_query_condition{};

  IndexBufferReference index_buffer{};
  BinningReference binning{};
  ShaderReference vertex_shader{};
  ShaderReference pixel_shader{};

  // Immediate index data stays packed exactly as supplied by PM4 until the
  // primitive-conversion stage consumes it. This avoids losing unusual Xenos
  // index packing while still keeping PM4 parsing out of host backends.
  std::vector<std::uint32_t> immediate_index_dwords{};
  std::vector<std::uint32_t> raw_payload{};
};

struct ShaderLoad {
  Type3Opcode opcode{};
  bool immediate{};
  std::uint32_t physical_address{};
  ShaderProgram program{};
  // Decoding depends only on the program, so every load of the same program
  // shares one immutable decode (titles reload the same microcode many times
  // a frame). Never null in a command the frontend emits.
  std::shared_ptr<const DecodedShader> decoded{};
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
  // Moves the commands out (for a hand-off to the backend thread), leaving
  // the stream empty with `recycled`'s storage, so steady-state hand-offs
  // allocate nothing.
  [[nodiscard]] std::vector<Command> take(std::vector<Command> recycled = {}) {
    recycled.clear();
    commands_.swap(recycled);
    return recycled;
  }
  [[nodiscard]] const std::vector<Command>& commands() const noexcept {
    return commands_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

 private:
  std::vector<Command> commands_{};
};

// Storage for command batches handed to the backend thread and back. The
// thread that consumes a batch releases its vector here, and the next
// Stream::take() reuses its capacity: without it every batch grew a new
// vector from empty, moving each command at every reallocation (2% of the
// GPU pump thread in AC6 flight).
class CommandBufferPool {
 public:
  [[nodiscard]] std::vector<Command> acquire() {
    std::scoped_lock lock(mutex_);
    if (free_.empty()) return {};
    auto commands = std::move(free_.back());
    free_.pop_back();
    return commands;
  }
  // Destroys the commands, outside the lock, and keeps the storage.
  void release(std::vector<Command>&& commands) {
    commands.clear();
    if (commands.capacity() == 0u) return;
    std::scoped_lock lock(mutex_);
    if (free_.size() < kKept) free_.push_back(std::move(commands));
  }
  [[nodiscard]] std::size_t kept() const {
    std::scoped_lock lock(mutex_);
    return free_.size();
  }

 private:
  // Batches in flight at once: one being filled, one queued, one consuming.
  static constexpr std::size_t kKept = 4;
  mutable std::mutex mutex_;
  std::vector<std::vector<Command>> free_;
};

}  // namespace xenon::gpu::ir
