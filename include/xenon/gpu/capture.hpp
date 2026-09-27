#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/gpu/shader.hpp"

namespace xenon::gpu {

// Deterministic frontend capture of one PM4 submission. The active shaders are
// retained because a draw may reference instruction memory populated by an
// earlier submission rather than by an IM_LOAD inside this submission.
struct FrontendSubmissionCapture {
  enum class Source : std::uint8_t { Buffer, Ring };

  Source source{Source::Buffer};
  std::uint32_t physical_address{};
  std::uint32_t dword_count{};
  std::uint32_t capacity_dwords{};
  std::uint32_t read_index{};
  std::uint32_t write_index{};
  std::uint32_t resulting_read_index{};
  RegisterFile::Snapshot initial_registers{};
  RegisterFile::Snapshot final_registers{};
  std::optional<ShaderProgram> initial_vertex_program{};
  std::optional<ShaderProgram> initial_pixel_program{};
  std::vector<ir::Command> commands{};
};

enum class CaptureRangeUsage : std::uint32_t {
  None = 0,
  CommandStream = 1u << 0,
  IndirectBuffer = 1u << 1,
  ShaderSource = 1u << 2,
  VertexBuffer = 1u << 3,
  IndexBuffer = 1u << 4,
  Texture = 1u << 5,
  ResolveDestination = 1u << 6,
  MemoryExport = 1u << 7,
  FrontendSideEffect = 1u << 8,
};

[[nodiscard]] constexpr CaptureRangeUsage operator|(CaptureRangeUsage a,
                                                    CaptureRangeUsage b) noexcept {
  return static_cast<CaptureRangeUsage>(static_cast<std::uint32_t>(a) |
                                        static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr CaptureRangeUsage operator&(CaptureRangeUsage a,
                                                    CaptureRangeUsage b) noexcept {
  return static_cast<CaptureRangeUsage>(static_cast<std::uint32_t>(a) &
                                        static_cast<std::uint32_t>(b));
}
inline CaptureRangeUsage& operator|=(CaptureRangeUsage& a,
                                    CaptureRangeUsage b) noexcept {
  a = a | b;
  return a;
}

struct CapturedPhysicalRange {
  std::uint32_t physical_address{};
  CaptureRangeUsage usage{CaptureRangeUsage::None};
  std::vector<std::byte> bytes{};
};

// Portable capture schema v1. The physical snapshots are post-frontend /
// pre-backend state, exactly matching FrontendSubmissionCapture::commands.
// EDRAM is always the complete canonical 10 MiB store so backend replay has no
// hidden dependency on native render-target ownership from a previous frame.
struct PortableSubmissionCapture {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schema_version{kSchemaVersion};
  std::uint64_t memory_epoch{};
  FrontendSubmissionCapture frontend{};
  std::vector<CapturedPhysicalRange> physical_ranges{};
  std::vector<std::byte> edram{};
  bool complete{true};
  std::vector<std::string> diagnostics{};

  [[nodiscard]] std::uint64_t physical_byte_count() const noexcept;
};

[[nodiscard]] bool save_portable_capture(
    const PortableSubmissionCapture& capture, const std::filesystem::path& path,
    std::string* error = nullptr);

[[nodiscard]] std::optional<PortableSubmissionCapture> load_portable_capture(
    const std::filesystem::path& path, std::string* error = nullptr);

}  // namespace xenon::gpu
