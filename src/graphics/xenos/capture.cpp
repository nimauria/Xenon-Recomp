#include "xenon/gpu/capture.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <type_traits>

#include "xenon/gpu/shader_ir.hpp"

namespace xenon::gpu {
namespace {

constexpr std::array<char, 8> kMagic{'X', 'E', 'N', 'G', 'P', 'U', 'C', '1'};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::uint64_t kMaximumVectorElements = UINT64_C(1) << 28;
constexpr std::uint64_t kMaximumBlobBytes = UINT64_C(1) << 34;

void set_error(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

template <typename T, bool IsEnum = std::is_enum_v<T>>
struct IntegerStorage {
  using type = T;
};

template <typename T>
struct IntegerStorage<T, true> {
  using type = std::underlying_type_t<T>;
};

template <typename T>
using IntegerStorageT = typename IntegerStorage<T>::type;

class Writer {
 public:
  explicit Writer(const std::filesystem::path& path)
      : file_(path, std::ios::binary | std::ios::trunc) {}

  [[nodiscard]] bool good() const noexcept { return file_.good(); }

  bool bytes(const void* data, std::size_t size) {
    if (!size) return true;
    file_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file_.good();
  }

  template <typename T>
  bool integer(T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using Raw = IntegerStorageT<T>;
    using U = std::make_unsigned_t<Raw>;
    U raw = static_cast<U>(value);
    std::array<std::byte, sizeof(U)> encoded{};
    for (std::size_t i = 0; i < encoded.size(); ++i) {
      encoded[i] = static_cast<std::byte>(raw >> (i * 8u));
    }
    return bytes(encoded.data(), encoded.size());
  }

  bool boolean(bool value) { return integer<std::uint8_t>(value ? 1u : 0u); }

  bool string(const std::string& value) {
    if (!integer<std::uint64_t>(value.size())) return false;
    return bytes(value.data(), value.size());
  }

  template <typename T>
  bool pod_vector(const std::vector<T>& values) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    if (!integer<std::uint64_t>(values.size())) return false;
    for (const auto value : values) {
      if (!integer<T>(value)) return false;
    }
    return true;
  }

  bool blob(std::span<const std::byte> value) {
    return integer<std::uint64_t>(value.size()) && bytes(value.data(), value.size());
  }

 private:
  std::ofstream file_;
};

class Reader {
 public:
  explicit Reader(const std::filesystem::path& path) : file_(path, std::ios::binary) {}

  [[nodiscard]] bool good() const noexcept { return file_.good(); }

  bool bytes(void* data, std::size_t size) {
    if (!size) return true;
    file_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return file_.good();
  }

  template <typename T>
  bool integer(T& value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using Raw = IntegerStorageT<T>;
    using U = std::make_unsigned_t<Raw>;
    std::array<std::byte, sizeof(U)> encoded{};
    if (!bytes(encoded.data(), encoded.size())) return false;
    U raw{};
    for (std::size_t i = 0; i < encoded.size(); ++i) {
      raw |= static_cast<U>(std::to_integer<std::uint8_t>(encoded[i])) << (i * 8u);
    }
    if constexpr (std::is_enum_v<T>) {
      value = static_cast<T>(static_cast<Raw>(raw));
    } else {
      value = static_cast<T>(static_cast<Raw>(raw));
    }
    return true;
  }

  bool boolean(bool& value) {
    std::uint8_t raw{};
    if (!integer(raw) || raw > 1u) return false;
    value = raw != 0;
    return true;
  }

  bool string(std::string& value) {
    std::uint64_t size{};
    if (!integer(size) || size > kMaximumBlobBytes ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return false;
    }
    value.resize(static_cast<std::size_t>(size));
    return bytes(value.data(), value.size());
  }

  template <typename T>
  bool pod_vector(std::vector<T>& values) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    std::uint64_t count{};
    if (!integer(count) || count > kMaximumVectorElements ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return false;
    }
    values.resize(static_cast<std::size_t>(count));
    for (auto& value : values) {
      if (!integer(value)) return false;
    }
    return true;
  }

  bool blob(std::vector<std::byte>& value) {
    std::uint64_t size{};
    if (!integer(size) || size > kMaximumBlobBytes ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return false;
    }
    value.resize(static_cast<std::size_t>(size));
    return bytes(value.data(), value.size());
  }

  [[nodiscard]] bool at_end() {
    char value{};
    file_.read(&value, 1);
    return file_.eof();
  }

 private:
  std::ifstream file_;
};

bool write_register_snapshot(Writer& out, const RegisterFile::Snapshot& snapshot) {
  if (!out.integer(snapshot.generation)) return false;
  for (const auto value : snapshot.values) {
    if (!out.integer(value)) return false;
  }
  return true;
}

bool read_register_snapshot(Reader& in, RegisterFile::Snapshot& snapshot) {
  if (!in.integer(snapshot.generation)) return false;
  for (auto& value : snapshot.values) {
    if (!in.integer(value)) return false;
  }
  return true;
}

bool write_shader_program(Writer& out, const ShaderProgram& program) {
  if (!out.integer(program.stage()) || !out.integer(program.start_slot()) ||
      !out.integer(program.hash())) {
    return false;
  }
  const auto dwords = program.dwords();
  if (!out.integer<std::uint64_t>(dwords.size())) return false;
  for (const auto dword : dwords) {
    if (!out.integer(dword)) return false;
  }
  return true;
}

bool read_shader_program(Reader& in, ShaderProgram& program) {
  ShaderStage stage{};
  std::uint32_t start_slot{};
  std::uint64_t expected_hash{};
  std::uint64_t count{};
  if (!in.integer(stage) || !in.integer(start_slot) || !in.integer(expected_hash) ||
      !in.integer(count) || count > kMaximumVectorElements ||
      count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  std::vector<std::uint32_t> dwords(static_cast<std::size_t>(count));
  for (auto& dword : dwords) {
    if (!in.integer(dword)) return false;
  }
  ShaderProgram decoded(stage, dwords, start_slot);
  if (decoded.hash() != expected_hash) return false;
  program = std::move(decoded);
  return true;
}

bool write_optional_shader(Writer& out, const std::optional<ShaderProgram>& program) {
  if (!out.boolean(program.has_value())) return false;
  return !program || write_shader_program(out, *program);
}

bool read_optional_shader(Reader& in, std::optional<ShaderProgram>& program) {
  bool present{};
  if (!in.boolean(present)) return false;
  if (!present) {
    program.reset();
    return true;
  }
  ShaderProgram value;
  if (!read_shader_program(in, value)) return false;
  program = std::move(value);
  return true;
}

bool write_shader_reference(Writer& out, const ir::ShaderReference& value) {
  return out.boolean(value.valid) && out.integer(value.hash) &&
         out.integer(value.start_slot);
}

bool read_shader_reference(Reader& in, ir::ShaderReference& value) {
  return in.boolean(value.valid) && in.integer(value.hash) &&
         in.integer(value.start_slot);
}

bool write_draw_packet(Writer& out, const ir::DrawPacket& draw) {
  return out.integer(draw.opcode) && out.boolean(draw.predicate) &&
         out.integer(draw.register_generation) &&
         out.integer(draw.primitive_type) && out.integer(draw.source) &&
         out.integer(draw.major_mode) && out.boolean(draw.explicit_major_mode) &&
         out.integer(draw.index_format) && out.boolean(draw.not_eop) &&
         out.boolean(draw.binned) && out.integer(draw.index_count) &&
         out.integer(draw.viz_query_condition) &&
         out.boolean(draw.index_buffer.valid) &&
         out.integer(draw.index_buffer.physical_address) &&
         out.integer(draw.index_buffer.length_bytes) &&
         out.integer(draw.index_buffer.index_count) &&
         out.integer(draw.index_buffer.format) &&
         out.integer(draw.index_buffer.endian) &&
         out.boolean(draw.binning.valid) && out.integer(draw.binning.base) &&
         out.integer(draw.binning.size) && out.integer(draw.binning.base_offset) &&
         out.integer(draw.binning.effective_base) &&
         out.integer(draw.binning.mask) && out.integer(draw.binning.select) &&
         write_shader_reference(out, draw.vertex_shader) &&
         write_shader_reference(out, draw.pixel_shader) &&
         out.pod_vector(draw.immediate_index_dwords) &&
         out.pod_vector(draw.raw_payload);
}

bool read_draw_packet(Reader& in, ir::DrawPacket& draw) {
  return in.integer(draw.opcode) && in.boolean(draw.predicate) &&
         in.integer(draw.register_generation) &&
         in.integer(draw.primitive_type) && in.integer(draw.source) &&
         in.integer(draw.major_mode) && in.boolean(draw.explicit_major_mode) &&
         in.integer(draw.index_format) && in.boolean(draw.not_eop) &&
         in.boolean(draw.binned) && in.integer(draw.index_count) &&
         in.integer(draw.viz_query_condition) &&
         in.boolean(draw.index_buffer.valid) &&
         in.integer(draw.index_buffer.physical_address) &&
         in.integer(draw.index_buffer.length_bytes) &&
         in.integer(draw.index_buffer.index_count) &&
         in.integer(draw.index_buffer.format) &&
         in.integer(draw.index_buffer.endian) &&
         in.boolean(draw.binning.valid) && in.integer(draw.binning.base) &&
         in.integer(draw.binning.size) && in.integer(draw.binning.base_offset) &&
         in.integer(draw.binning.effective_base) &&
         in.integer(draw.binning.mask) && in.integer(draw.binning.select) &&
         read_shader_reference(in, draw.vertex_shader) &&
         read_shader_reference(in, draw.pixel_shader) &&
         in.pod_vector(draw.immediate_index_dwords) &&
         in.pod_vector(draw.raw_payload);
}

enum class CommandTag : std::uint8_t {
  RegisterWrite,
  PhysicalMemoryWrite,
  IndirectBuffer,
  DrawPacket,
  ShaderLoad,
  ShaderPacket,
  SynchronizationPacket,
  EventPacket,
  StatePacket,
  Type3Packet,
};

bool write_command(Writer& out, const ir::Command& command) {
  return std::visit(
      [&](const auto& value) -> bool {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ir::RegisterWrite>) {
          return out.integer(CommandTag::RegisterWrite) && out.integer(value.index) &&
                 out.integer(value.value);
        } else if constexpr (std::is_same_v<T, ir::PhysicalMemoryWrite>) {
          return out.integer(CommandTag::PhysicalMemoryWrite) &&
                 out.integer(value.physical_address) && out.integer(value.value) &&
                 out.integer(value.endian);
        } else if constexpr (std::is_same_v<T, ir::IndirectBuffer>) {
          return out.integer(CommandTag::IndirectBuffer) &&
                 out.integer(value.physical_address) && out.integer(value.dword_count) &&
                 out.boolean(value.prefetch);
        } else if constexpr (std::is_same_v<T, ir::DrawPacket>) {
          return out.integer(CommandTag::DrawPacket) && write_draw_packet(out, value);
        } else if constexpr (std::is_same_v<T, ir::ShaderLoad>) {
          return out.integer(CommandTag::ShaderLoad) && out.integer(value.opcode) &&
                 out.boolean(value.immediate) && out.integer(value.physical_address) &&
                 write_shader_program(out, value.program) &&
                 out.pod_vector(value.raw_payload);
        } else if constexpr (std::is_same_v<T, ir::ShaderPacket>) {
          return out.integer(CommandTag::ShaderPacket) && out.integer(value.opcode) &&
                 out.pod_vector(value.payload);
        } else if constexpr (std::is_same_v<T, ir::SynchronizationPacket>) {
          return out.integer(CommandTag::SynchronizationPacket) &&
                 out.integer(value.opcode) && out.pod_vector(value.payload);
        } else if constexpr (std::is_same_v<T, ir::EventPacket>) {
          return out.integer(CommandTag::EventPacket) && out.integer(value.opcode) &&
                 out.pod_vector(value.payload);
        } else if constexpr (std::is_same_v<T, ir::StatePacket>) {
          return out.integer(CommandTag::StatePacket) && out.integer(value.opcode) &&
                 out.pod_vector(value.payload);
        } else if constexpr (std::is_same_v<T, ir::Type3Packet>) {
          return out.integer(CommandTag::Type3Packet) && out.integer(value.opcode) &&
                 out.boolean(value.predicate) && out.pod_vector(value.payload);
        }
        return false;
      },
      command);
}

bool read_command(Reader& in, ir::Command& command) {
  CommandTag tag{};
  if (!in.integer(tag)) return false;
  switch (tag) {
    case CommandTag::RegisterWrite: {
      ir::RegisterWrite value{};
      if (!in.integer(value.index) || !in.integer(value.value)) return false;
      command = value;
      return true;
    }
    case CommandTag::PhysicalMemoryWrite: {
      ir::PhysicalMemoryWrite value{};
      if (!in.integer(value.physical_address) || !in.integer(value.value) ||
          !in.integer(value.endian)) {
        return false;
      }
      command = value;
      return true;
    }
    case CommandTag::IndirectBuffer: {
      ir::IndirectBuffer value{};
      if (!in.integer(value.physical_address) || !in.integer(value.dword_count) ||
          !in.boolean(value.prefetch)) {
        return false;
      }
      command = value;
      return true;
    }
    case CommandTag::DrawPacket: {
      ir::DrawPacket value{};
      if (!read_draw_packet(in, value)) return false;
      command = std::move(value);
      return true;
    }
    case CommandTag::ShaderLoad: {
      Type3Opcode opcode{};
      bool immediate{};
      std::uint32_t physical_address{};
      ShaderProgram program;
      std::vector<std::uint32_t> raw_payload;
      if (!in.integer(opcode) || !in.boolean(immediate) ||
          !in.integer(physical_address) || !read_shader_program(in, program) ||
          !in.pod_vector(raw_payload)) {
        return false;
      }
      command = ir::ShaderLoad{opcode, immediate, physical_address, program,
                               ShaderDecoder::decode(program),
                               std::move(raw_payload)};
      return true;
    }
    case CommandTag::ShaderPacket: {
      ir::ShaderPacket value{};
      if (!in.integer(value.opcode) || !in.pod_vector(value.payload)) return false;
      command = std::move(value);
      return true;
    }
    case CommandTag::SynchronizationPacket: {
      ir::SynchronizationPacket value{};
      if (!in.integer(value.opcode) || !in.pod_vector(value.payload)) return false;
      command = std::move(value);
      return true;
    }
    case CommandTag::EventPacket: {
      ir::EventPacket value{};
      if (!in.integer(value.opcode) || !in.pod_vector(value.payload)) return false;
      command = std::move(value);
      return true;
    }
    case CommandTag::StatePacket: {
      ir::StatePacket value{};
      if (!in.integer(value.opcode) || !in.pod_vector(value.payload)) return false;
      command = std::move(value);
      return true;
    }
    case CommandTag::Type3Packet: {
      ir::Type3Packet value{};
      if (!in.integer(value.opcode) || !in.boolean(value.predicate) ||
          !in.pod_vector(value.payload)) {
        return false;
      }
      command = std::move(value);
      return true;
    }
  }
  return false;
}

bool write_frontend(Writer& out, const FrontendSubmissionCapture& capture) {
  if (!out.integer(capture.source) || !out.integer(capture.physical_address) ||
      !out.integer(capture.dword_count) || !out.integer(capture.capacity_dwords) ||
      !out.integer(capture.read_index) || !out.integer(capture.write_index) ||
      !out.integer(capture.resulting_read_index) ||
      !write_register_snapshot(out, capture.initial_registers) ||
      !write_register_snapshot(out, capture.final_registers) ||
      !write_optional_shader(out, capture.initial_vertex_program) ||
      !write_optional_shader(out, capture.initial_pixel_program) ||
      !out.integer<std::uint64_t>(capture.commands.size())) {
    return false;
  }
  for (const auto& command : capture.commands) {
    if (!write_command(out, command)) return false;
  }
  return true;
}

bool read_frontend(Reader& in, FrontendSubmissionCapture& capture) {
  std::uint64_t command_count{};
  if (!in.integer(capture.source) || !in.integer(capture.physical_address) ||
      !in.integer(capture.dword_count) || !in.integer(capture.capacity_dwords) ||
      !in.integer(capture.read_index) || !in.integer(capture.write_index) ||
      !in.integer(capture.resulting_read_index) ||
      !read_register_snapshot(in, capture.initial_registers) ||
      !read_register_snapshot(in, capture.final_registers) ||
      !read_optional_shader(in, capture.initial_vertex_program) ||
      !read_optional_shader(in, capture.initial_pixel_program) ||
      !in.integer(command_count) || command_count > kMaximumVectorElements ||
      command_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  capture.commands.resize(static_cast<std::size_t>(command_count));
  for (auto& command : capture.commands) {
    if (!read_command(in, command)) return false;
  }
  return true;
}

}  // namespace

std::uint64_t PortableSubmissionCapture::physical_byte_count() const noexcept {
  std::uint64_t total{};
  for (const auto& range : physical_ranges) total += range.bytes.size();
  return total;
}

bool save_portable_capture(const PortableSubmissionCapture& capture,
                           const std::filesystem::path& path,
                           std::string* error) {
  if (capture.schema_version != PortableSubmissionCapture::kSchemaVersion) {
    set_error(error, "unsupported portable GPU capture schema version");
    return false;
  }
  if (capture.edram.size() != Edram::kSize) {
    set_error(error, "portable GPU capture requires the complete 10 MiB EDRAM image");
    return false;
  }

  Writer out(path);
  if (!out.good()) {
    set_error(error, "could not create portable GPU capture file");
    return false;
  }
  if (!out.bytes(kMagic.data(), kMagic.size()) ||
      !out.integer(capture.schema_version) || !out.integer(kEndianMarker) ||
      !out.integer<std::uint32_t>(RegisterFile::kRegisterCount) ||
      !out.integer(capture.memory_epoch) || !out.boolean(capture.complete) ||
      !write_frontend(out, capture.frontend) ||
      !out.integer<std::uint64_t>(capture.physical_ranges.size())) {
    set_error(error, "failed while writing portable GPU capture header/frontend");
    return false;
  }

  for (const auto& range : capture.physical_ranges) {
    if (std::uint64_t(range.physical_address) + range.bytes.size() >
        UINT64_C(0x20000000)) {
      set_error(error, "portable GPU capture contains an out-of-range physical snapshot");
      return false;
    }
    if (!out.integer(range.physical_address) || !out.integer(range.usage) ||
        !out.blob(range.bytes)) {
      set_error(error, "failed while writing portable GPU physical snapshots");
      return false;
    }
  }
  if (!out.blob(capture.edram) ||
      !out.integer<std::uint64_t>(capture.diagnostics.size())) {
    set_error(error, "failed while writing portable GPU EDRAM/diagnostics");
    return false;
  }
  for (const auto& diagnostic : capture.diagnostics) {
    if (!out.string(diagnostic)) {
      set_error(error, "failed while writing portable GPU diagnostics");
      return false;
    }
  }
  return out.good();
}

std::optional<PortableSubmissionCapture> load_portable_capture(
    const std::filesystem::path& path, std::string* error) {
  Reader in(path);
  if (!in.good()) {
    set_error(error, "could not open portable GPU capture file");
    return std::nullopt;
  }
  std::array<char, kMagic.size()> magic{};
  std::uint32_t endian_marker{};
  std::uint32_t register_count{};
  PortableSubmissionCapture capture{};
  if (!in.bytes(magic.data(), magic.size()) || magic != kMagic ||
      !in.integer(capture.schema_version) ||
      capture.schema_version != PortableSubmissionCapture::kSchemaVersion ||
      !in.integer(endian_marker) || endian_marker != kEndianMarker ||
      !in.integer(register_count) || register_count != RegisterFile::kRegisterCount ||
      !in.integer(capture.memory_epoch) || !in.boolean(capture.complete) ||
      !read_frontend(in, capture.frontend)) {
    set_error(error, "portable GPU capture header/schema is invalid or incompatible");
    return std::nullopt;
  }

  std::uint64_t range_count{};
  if (!in.integer(range_count) || range_count > kMaximumVectorElements ||
      range_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    set_error(error, "portable GPU capture physical range table is invalid");
    return std::nullopt;
  }
  capture.physical_ranges.resize(static_cast<std::size_t>(range_count));
  for (auto& range : capture.physical_ranges) {
    if (!in.integer(range.physical_address) || !in.integer(range.usage) ||
        !in.blob(range.bytes) ||
        std::uint64_t(range.physical_address) + range.bytes.size() >
            UINT64_C(0x20000000)) {
      set_error(error, "portable GPU capture physical snapshot is invalid");
      return std::nullopt;
    }
  }
  if (!in.blob(capture.edram) || capture.edram.size() != Edram::kSize) {
    set_error(error, "portable GPU capture EDRAM image is invalid");
    return std::nullopt;
  }
  std::uint64_t diagnostic_count{};
  if (!in.integer(diagnostic_count) || diagnostic_count > kMaximumVectorElements ||
      diagnostic_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    set_error(error, "portable GPU capture diagnostic table is invalid");
    return std::nullopt;
  }
  capture.diagnostics.resize(static_cast<std::size_t>(diagnostic_count));
  for (auto& diagnostic : capture.diagnostics) {
    if (!in.string(diagnostic)) {
      set_error(error, "portable GPU capture diagnostic string is invalid");
      return std::nullopt;
    }
  }
  if (!in.at_end()) {
    set_error(error, "portable GPU capture has trailing or truncated data");
    return std::nullopt;
  }
  return capture;
}

}  // namespace xenon::gpu
