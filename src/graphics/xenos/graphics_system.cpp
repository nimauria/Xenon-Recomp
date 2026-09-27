#include "xenon/gpu/graphics_system.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <limits>
#include <unordered_map>

#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::gpu {
namespace {

struct PendingRange {
  std::uint32_t begin{};
  std::uint32_t end{};
  CaptureRangeUsage usage{CaptureRangeUsage::None};
};

void set_error(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool add_range(std::vector<PendingRange>& ranges, std::uint32_t address,
               std::uint64_t size, CaptureRangeUsage usage,
               std::string* error) {
  if (!size) return true;
  if (size > UINT32_MAX ||
      std::uint64_t(address) + size > memory::kPhysicalMemorySize) {
    set_error(error, "portable GPU capture dependency is outside physical RAM");
    return false;
  }
  ranges.push_back(
      {address, static_cast<std::uint32_t>(std::uint64_t(address) + size), usage});
  return true;
}

std::vector<PendingRange> merge_ranges(std::vector<PendingRange> ranges) {
  std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
    return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
  });
  std::vector<PendingRange> merged;
  for (const auto& range : ranges) {
    if (range.begin >= range.end) continue;
    if (merged.empty() || range.begin > merged.back().end) {
      merged.push_back(range);
      continue;
    }
    merged.back().end = std::max(merged.back().end, range.end);
    merged.back().usage |= range.usage;
  }
  return merged;
}

void seed_resource_state(ResourceStateTracker& tracker,
                         const RegisterFile::Snapshot& registers) {
  for (std::uint32_t index = 0; index < RegisterFile::kRegisterCount; ++index) {
    tracker.apply(ir::RegisterWrite{index, registers.values[index]});
  }
}

void add_program(std::unordered_map<std::uint64_t, DecodedShader>& shaders,
                 const std::optional<ShaderProgram>& program) {
  if (!program) return;
  shaders.insert_or_assign(program->hash(), ShaderDecoder::decode(*program));
}

bool add_texture_ranges(std::vector<PendingRange>& ranges,
                        const TextureDescriptor& descriptor,
                        std::string* error) {
  const auto layout = build_texture_layout(descriptor);
  if (!layout.valid) {
    set_error(error, "portable GPU capture could not build an active texture layout: " +
                         layout.error);
    return false;
  }
  for (const auto& subresource : layout.subresources) {
    if (!add_range(ranges, subresource.guest_address,
                   subresource.guest_size_bytes, CaptureRangeUsage::Texture,
                   error)) {
      return false;
    }
  }
  return true;
}

bool add_resolve_destination_range(std::vector<PendingRange>& ranges,
                                   const DrawResourceState& state,
                                   const ResolveRectangle& rectangle,
                                   bool depth, std::string* error) {
  if (!rectangle.valid || rectangle.empty()) return true;
  if (rectangle.left < 0 || rectangle.top < 0 || !state.copy.destination_pitch) {
    set_error(error, "portable GPU capture encountered an invalid resolve rectangle");
    return false;
  }
  std::uint8_t format = state.copy.destination_format;
  if (depth) {
    format = depth_resolve_texture_format(
        static_cast<DepthRenderTargetFormat>(state.depth_target.format));
  }
  const auto& format_info = texture_format_info(format);
  const auto bytes_per_pixel = format_info.bytes_per_block();
  if (format_info.storage != TextureStorage::Uncompressed ||
      format_info.block_width != 1u || format_info.block_height != 1u ||
      !bytes_per_pixel) {
    set_error(error, "portable GPU capture cannot size the resolve destination format");
    return false;
  }
  const auto pitch_aligned =
      (std::uint32_t(state.copy.destination_pitch) + 31u) & ~31u;
  const auto required_height = std::max<std::uint32_t>(
      1u, state.copy.destination_array
              ? std::uint32_t(state.copy.destination_height)
              : static_cast<std::uint32_t>(rectangle.bottom));
  const auto height_aligned = (required_height + 31u) & ~31u;
  // Xenos array resolves select one layer inside a Z/8 group. Capturing the
  // complete group is conservative and avoids embedding the 3D tiling formula
  // into the portable-capture schema itself.
  const std::uint64_t layer_factor = state.copy.destination_array ? 8u : 1u;
  const std::uint64_t bytes = std::uint64_t(pitch_aligned) * height_aligned *
                              bytes_per_pixel * layer_factor;
  return add_range(ranges, state.copy.destination_base, bytes,
                   CaptureRangeUsage::ResolveDestination, error);
}

void replay_shader_preamble(Backend& backend,
                            const FrontendSubmissionCapture& capture) {
  const auto emit_program = [&](const std::optional<ShaderProgram>& program) {
    if (!program) return;
    const Type3Opcode opcode = Type3Opcode::ImLoadImmediate;
    backend.consume(ir::ShaderLoad{opcode, true, 0u, *program,
                                   ShaderDecoder::decode(*program), {}});
  };
  emit_program(capture.initial_vertex_program);
  emit_program(capture.initial_pixel_program);
}

bool build_portable_capture(memory::AddressSpace& memory, Edram& edram,
                            Backend& backend,
                            FrontendSubmissionCapture&& frontend,
                            PortableSubmissionCapture& out,
                            std::string* error) {
  auto capture = std::make_unique<PortableSubmissionCapture>();
  capture->frontend = std::move(frontend);

  // Bind the backend to the canonical objects before asking it to expose any
  // native-authored memory. No IR is consumed by this maintenance submission.
  backend.begin_submission(memory, edram);
  const auto finish_backend = [&]() { backend.end_submission(); };
  if (!backend.make_edram_canonical()) {
    finish_backend();
    set_error(error, "portable GPU capture could not canonicalize EDRAM");
    return false;
  }

  std::vector<PendingRange> ranges;
  const auto& frontend_capture = capture->frontend;
  if (frontend_capture.source == FrontendSubmissionCapture::Source::Buffer) {
    if (!add_range(ranges, frontend_capture.physical_address,
                   std::uint64_t(frontend_capture.dword_count) * 4u,
                   CaptureRangeUsage::CommandStream, error)) {
      finish_backend();
      return false;
    }
  } else if (frontend_capture.capacity_dwords) {
    const auto read = frontend_capture.read_index % frontend_capture.capacity_dwords;
    const auto write = frontend_capture.write_index % frontend_capture.capacity_dwords;
    if (read <= write) {
      if (!add_range(ranges, frontend_capture.physical_address + read * 4u,
                     std::uint64_t(write - read) * 4u,
                     CaptureRangeUsage::CommandStream, error)) {
        finish_backend();
        return false;
      }
    } else {
      if (!add_range(ranges, frontend_capture.physical_address + read * 4u,
                     std::uint64_t(frontend_capture.capacity_dwords - read) * 4u,
                     CaptureRangeUsage::CommandStream, error) ||
          !add_range(ranges, frontend_capture.physical_address,
                     std::uint64_t(write) * 4u,
                     CaptureRangeUsage::CommandStream, error)) {
        finish_backend();
        return false;
      }
    }
  }

  auto tracker = std::make_unique<ResourceStateTracker>();
  seed_resource_state(*tracker, frontend_capture.initial_registers);
  std::unordered_map<std::uint64_t, DecodedShader> shaders;
  add_program(shaders, frontend_capture.initial_vertex_program);
  add_program(shaders, frontend_capture.initial_pixel_program);
  std::optional<ShaderProgram> active_vertex = frontend_capture.initial_vertex_program;
  std::optional<ShaderProgram> active_pixel = frontend_capture.initial_pixel_program;

  for (const auto& command : frontend_capture.commands) {
    if (const auto* write = std::get_if<ir::RegisterWrite>(&command)) {
      tracker->apply(*write);
      continue;
    }
    if (const auto* write = std::get_if<ir::PhysicalMemoryWrite>(&command)) {
      if (!add_range(ranges, write->physical_address, 4u,
                     CaptureRangeUsage::FrontendSideEffect, error)) {
        finish_backend();
        return false;
      }
      continue;
    }
    if (const auto* indirect = std::get_if<ir::IndirectBuffer>(&command)) {
      if (!add_range(ranges, indirect->physical_address,
                     std::uint64_t(indirect->dword_count) * 4u,
                     CaptureRangeUsage::IndirectBuffer, error)) {
        finish_backend();
        return false;
      }
      continue;
    }
    if (const auto* load = std::get_if<ir::ShaderLoad>(&command)) {
      shaders.insert_or_assign(load->program.hash(), load->decoded);
      if (load->program.stage() == ShaderStage::Vertex) {
        active_vertex = load->program;
      } else {
        active_pixel = load->program;
      }
      if (!load->immediate && load->physical_address) {
        if (!add_range(ranges, load->physical_address,
                       std::uint64_t(load->program.dwords().size()) * 4u,
                       CaptureRangeUsage::ShaderSource, error)) {
          finish_backend();
          return false;
        }
      }
      continue;
    }
    if (const auto* shader_packet = std::get_if<ir::ShaderPacket>(&command)) {
      if (shader_packet->opcode == Type3Opcode::ImStore &&
          shader_packet->payload.size() == 2u) {
        const auto stage = shader_packet->payload[0] & 0x3u;
        const auto& program = stage == 0u ? active_vertex : active_pixel;
        if (stage <= 1u && program) {
          if (!add_range(ranges,
                         cpu_to_gpu_address(shader_packet->payload[0] & ~0x3u),
                         std::uint64_t(program->dwords().size()) * 4u,
                         CaptureRangeUsage::FrontendSideEffect, error) ||
              !add_range(ranges, cpu_to_gpu_address(shader_packet->payload[1]),
                         4u, CaptureRangeUsage::FrontendSideEffect, error)) {
            finish_backend();
            return false;
          }
        }
      }
      continue;
    }
    if (const auto* event = std::get_if<ir::EventPacket>(&command)) {
      if (event->opcode == Type3Opcode::EventWriteShaderDone &&
          event->payload.size() == 3u && (event->payload[1] & ~0x3u)) {
        if (!add_range(ranges,
                       cpu_to_gpu_address(event->payload[1] & ~0x3u), 4u,
                       CaptureRangeUsage::FrontendSideEffect, error)) {
          finish_backend();
          return false;
        }
      } else if (event->opcode == Type3Opcode::EventWriteExtent &&
                 event->payload.size() == 2u) {
        if (!add_range(ranges,
                       cpu_to_gpu_address(event->payload[1] & ~0x3u), 12u,
                       CaptureRangeUsage::FrontendSideEffect, error)) {
          finish_backend();
          return false;
        }
      }
      continue;
    }
    const auto* draw = std::get_if<ir::DrawPacket>(&command);
    if (!draw) continue;

    const auto state = tracker->snapshot();
    if (draw->index_buffer.valid && draw->index_buffer.length_bytes) {
      if (!add_range(ranges, draw->index_buffer.physical_address,
                     draw->index_buffer.length_bytes,
                     CaptureRangeUsage::IndexBuffer, error)) {
        finish_backend();
        return false;
      }
    }

    // If reflection is available, capture only texture slots sampled by the
    // active shaders. If a shader identity cannot be reconstructed, capture
    // every active texture descriptor and flag the artifact as conservative.
    std::array<bool, 32> used_textures{};
    bool reflection_known = true;
    const auto mark_texture_usage = [&](const ir::ShaderReference& reference) {
      if (!reference.valid) return;
      const auto found = shaders.find(reference.hash);
      if (found == shaders.end()) {
        reflection_known = false;
        return;
      }
      for (const auto slot : found->second.reflection.texture_fetch_constants) {
        if (slot < used_textures.size()) used_textures[slot] = true;
      }
    };
    mark_texture_usage(draw->vertex_shader);
    mark_texture_usage(draw->pixel_shader);
    if (!reflection_known) {
      used_textures.fill(true);
      capture->complete = false;
      capture->diagnostics.emplace_back(
          "A draw referenced a shader without captured reflection; all active texture slots were snapshotted conservatively.");
    }
    for (std::uint32_t slot = 0; slot < used_textures.size(); ++slot) {
      if (!used_textures[slot] || !state.textures[slot]) continue;
      if (!add_texture_ranges(ranges, *state.textures[slot], error)) {
        finish_backend();
        return false;
      }
    }

    // Vertex fetch ranges are small enough that retaining every active fetch
    // descriptor is safer than depending on shader reflection for mini-fetch
    // and dynamic-addressing corner cases.
    for (const auto& group : state.vertex_buffers) {
      for (const auto& vertex : group) {
        if (!vertex || !vertex->valid || !vertex->size_dwords) continue;
        if (!add_range(ranges, vertex->physical_address,
                       std::uint64_t(vertex->size_dwords) * 4u,
                       CaptureRangeUsage::VertexBuffer, error)) {
          finish_backend();
          return false;
        }
      }
    }

    const auto add_memexport = [&](const ir::ShaderReference& reference) -> bool {
      if (!reference.valid) return true;
      const auto found = shaders.find(reference.hash);
      if (found == shaders.end() || !found->second.reflection.memory_exports) {
        return true;
      }
      const auto plan = tracker->plan_memexport(found->second);
      if (!plan.valid) {
        set_error(error, "portable GPU capture could not plan shader memory export: " +
                             plan.error);
        return false;
      }
      if (plan.requires_dynamic_address_analysis) {
        capture->complete = false;
        capture->diagnostics.emplace_back(
            "A shader uses a dynamically addressed memory export; statically known export ranges were captured, but arbitrary export targets cannot be proven complete.");
      }
      for (const auto& range : plan.ranges) {
        if (!add_range(ranges, range.base_address_dwords << 2u,
                       range.size_bytes, CaptureRangeUsage::MemoryExport,
                       error)) {
          return false;
        }
      }
      return true;
    };
    if (!add_memexport(draw->vertex_shader) || !add_memexport(draw->pixel_shader)) {
      finish_backend();
      return false;
    }

    if (state.edram_mode == EdramMode::Copy) {
      const auto& vertices = state.vertex_buffers[0][0];
      if (!vertices || !vertices->valid || vertices->size_dwords < 6u) {
        capture->complete = false;
        capture->diagnostics.emplace_back(
            "A resolve draw lacks the conventional six-dword vertex stream; its destination footprint could not be proven.");
        continue;
      }
      constexpr std::uint32_t kResolveVertexBytes = 6u * sizeof(std::uint32_t);
      if (!backend.make_guest_memory_cpu_visible(vertices->physical_address,
                                                  kResolveVertexBytes)) {
        finish_backend();
        set_error(error, "portable GPU capture could not expose resolve vertices through Memory V2");
        return false;
      }
      std::array<std::byte, kResolveVertexBytes> vertex_snapshot{};
      if (!memory.copy_physical_range(vertices->physical_address,
                                      vertex_snapshot)) {
        finish_backend();
        set_error(error, "portable GPU capture could not snapshot resolve vertices");
        return false;
      }
      const auto plan = plan_resolve(state, vertex_snapshot,
                                     vertices->physical_address);
      if (!plan.valid) {
        capture->complete = false;
        capture->diagnostics.emplace_back(
            "A resolve destination could not be planned: " + plan.error);
      } else if (!add_resolve_destination_range(
                     ranges, state, plan.rectangle, plan.depth, error)) {
        finish_backend();
        return false;
      }
    }
  }

  auto merged = merge_ranges(std::move(ranges));
  capture->physical_ranges.reserve(merged.size());
  for (const auto& range : merged) {
    const auto size = range.end - range.begin;
    if (!backend.make_guest_memory_cpu_visible(range.begin, size)) {
      finish_backend();
      set_error(error,
                "portable GPU capture could not make a guest resource CPU-visible through Memory V2");
      return false;
    }
    CapturedPhysicalRange snapshot{};
    snapshot.physical_address = range.begin;
    snapshot.usage = range.usage;
    snapshot.bytes.resize(size);
    if (!memory.copy_physical_range(range.begin, snapshot.bytes)) {
      finish_backend();
      set_error(error, "portable GPU capture failed to snapshot a guest resource range");
      return false;
    }
    capture->physical_ranges.push_back(std::move(snapshot));
  }
  capture->edram.assign(edram.bytes().begin(), edram.bytes().end());
  capture->memory_epoch = memory.coherency().current_epoch();
  finish_backend();
  out = std::move(*capture);
  return true;
}

}  // namespace

GraphicsSystem::GraphicsSystem(memory::AddressSpace& memory)
    : memory_(memory), command_processor_(memory_, registers_, stream_) {}

void GraphicsSystem::reset() {
  command_processor_.reset();
  edram_.reset();
}

void GraphicsSystem::submit_buffer(std::uint32_t physical_address,
                                   std::uint32_t dword_count) {
  command_processor_.execute_buffer(physical_address, dword_count);
}

std::uint32_t GraphicsSystem::submit_ring(std::uint32_t physical_address,
                                          std::uint32_t capacity_dwords,
                                          std::uint32_t read_index,
                                          std::uint32_t write_index) {
  return command_processor_.execute_ring(physical_address, capacity_dwords,
                                         read_index, write_index);
}

FrontendSubmissionCapture GraphicsSystem::capture_buffer(
    std::uint32_t physical_address, std::uint32_t dword_count) {
  auto capture = std::make_unique<FrontendSubmissionCapture>();
  capture->source = FrontendSubmissionCapture::Source::Buffer;
  capture->physical_address = physical_address;
  capture->dword_count = dword_count;
  capture->initial_registers = registers_.snapshot();
  capture->initial_vertex_program = command_processor_.active_vertex_program();
  capture->initial_pixel_program = command_processor_.active_pixel_program();
  const auto command_begin = stream_.size();
  command_processor_.execute_buffer(physical_address, dword_count);
  capture->final_registers = registers_.snapshot();
  const auto& commands = stream_.commands();
  capture->commands.assign(commands.begin() + command_begin, commands.end());
  return std::move(*capture);
}

FrontendSubmissionCapture GraphicsSystem::capture_ring(
    std::uint32_t physical_address, std::uint32_t capacity_dwords,
    std::uint32_t read_index, std::uint32_t write_index) {
  auto capture = std::make_unique<FrontendSubmissionCapture>();
  capture->source = FrontendSubmissionCapture::Source::Ring;
  capture->physical_address = physical_address;
  capture->capacity_dwords = capacity_dwords;
  capture->read_index = read_index;
  capture->write_index = write_index;
  capture->initial_registers = registers_.snapshot();
  capture->initial_vertex_program = command_processor_.active_vertex_program();
  capture->initial_pixel_program = command_processor_.active_pixel_program();
  const auto command_begin = stream_.size();
  capture->resulting_read_index = command_processor_.execute_ring(
      physical_address, capacity_dwords, read_index, write_index);
  capture->final_registers = registers_.snapshot();
  const auto& commands = stream_.commands();
  capture->commands.assign(commands.begin() + command_begin, commands.end());
  return std::move(*capture);
}

bool GraphicsSystem::capture_portable_buffer(
    Backend& backend, std::uint32_t physical_address, std::uint32_t dword_count,
    PortableSubmissionCapture& out, std::string* error) {
  auto frontend = capture_buffer(physical_address, dword_count);
  return build_portable_capture(memory_, edram_, backend,
                                std::move(frontend),
                                out, error);
}

bool GraphicsSystem::capture_portable_ring(
    Backend& backend, std::uint32_t physical_address,
    std::uint32_t capacity_dwords, std::uint32_t read_index,
    std::uint32_t write_index, PortableSubmissionCapture& out,
    std::string* error) {
  return build_portable_capture(
      memory_, edram_, backend,
      capture_ring(physical_address, capacity_dwords, read_index, write_index),
      out, error);
}

void GraphicsSystem::replay_capture(
    Backend& backend, const FrontendSubmissionCapture& capture) {
  backend.begin_submission(memory_, edram_);
  for (std::uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
    backend.consume(ir::RegisterWrite{i, capture.initial_registers.values[i]});
  }
  replay_shader_preamble(backend, capture);
  for (const auto& command : capture.commands) backend.consume(command);
  backend.end_submission();
}

bool GraphicsSystem::replay_portable_capture(
    Backend& backend, const PortableSubmissionCapture& capture,
    std::string* error) {
  if (capture.schema_version != PortableSubmissionCapture::kSchemaVersion ||
      capture.edram.size() != Edram::kSize) {
    set_error(error, "portable GPU capture schema or EDRAM image is invalid");
    return false;
  }

  backend.begin_submission(memory_, edram_);
  if (!backend.invalidate_edram_native_state()) {
    backend.end_submission();
    set_error(error, "backend could not discard stale native EDRAM ownership");
    return false;
  }
  edram_.write(0u, capture.edram);
  for (const auto& range : capture.physical_ranges) {
    if (std::uint64_t(range.physical_address) + range.bytes.size() >
            memory::kPhysicalMemorySize ||
        !memory_.write_physical(range.physical_address, range.bytes)) {
      backend.end_submission();
      set_error(error, "portable GPU capture could not restore guest physical memory");
      return false;
    }
  }

  for (std::uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
    backend.consume(
        ir::RegisterWrite{i, capture.frontend.initial_registers.values[i]});
  }
  replay_shader_preamble(backend, capture.frontend);
  for (const auto& command : capture.frontend.commands) backend.consume(command);
  backend.end_submission();
  return true;
}

void GraphicsSystem::execute_ir(Backend& backend) {
  backend.begin_submission(memory_, edram_);
  for (const auto& command : stream_.commands()) backend.consume(command);
  backend.end_submission();
  stream_.clear();
}

bool GraphicsSystem::make_guest_memory_cpu_visible(
    Backend& backend, std::uint32_t physical_address, std::uint32_t size) {
  return backend.make_guest_memory_cpu_visible(physical_address, size);
}

bool GraphicsSystem::make_edram_canonical(Backend& backend) {
  return backend.make_edram_canonical();
}

PresentStatus GraphicsSystem::present(Backend& backend,
                                      const PresentationFrame& frame) {
  command_processor_.notify_present();
  return backend.present(frame);
}

}  // namespace xenon::gpu
