#include "xenon/gpu/graphics_system.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <limits>
#include <unordered_map>

#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/logging/probe_log.hpp"
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
    backend.consume(ir::ShaderLoad{
        opcode, true, 0u, *program,
        std::make_shared<const DecodedShader>(ShaderDecoder::decode(*program)), {}});
  };
  emit_program(capture.initial_vertex_program);
  emit_program(capture.initial_pixel_program);
}

// `edram_image`, when given, is the canonical EDRAM the commands start from,
// taken earlier (a live recording's start); otherwise the backend's EDRAM is
// made canonical and taken now.
bool build_portable_capture(memory::AddressSpace& memory, Edram& edram,
                            Backend& backend,
                            FrontendSubmissionCapture&& frontend,
                            PortableSubmissionCapture& out,
                            std::string* error,
                            std::vector<std::byte>* edram_image = nullptr) {
  auto capture = std::make_unique<PortableSubmissionCapture>();
  capture->frontend = std::move(frontend);

  // Bind the backend to the canonical objects before asking it to expose any
  // native-authored memory. No IR is consumed by this maintenance submission.
  backend.begin_submission(memory, edram);
  const auto finish_backend = [&]() { backend.end_submission(); };
  if (!edram_image && !backend.make_edram_canonical()) {
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
      if (load->decoded) shaders.insert_or_assign(load->program.hash(), *load->decoded);
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
  if (edram_image)
    capture->edram = std::move(*edram_image);
  else
    capture->edram.assign(edram.bytes().begin(), edram.bytes().end());
  capture->memory_epoch = memory.coherency().current_epoch();
  finish_backend();
  out = std::move(*capture);
  return true;
}

}  // namespace

GraphicsSystem::GraphicsSystem(memory::AddressSpace& memory)
    : memory_(memory), command_processor_(memory_, registers_, stream_) {
  set_swap_callback({});
}

void GraphicsSystem::set_swap_callback(std::function<void(const FrontBufferSwap&)> callback) {
  command_processor_.set_swap_callback(
      [this, callback = std::move(callback)](const FrontBufferSwap& swap) {
        // The swap shows the frame whose work has just executed (the command
        // processor runs execute_ir() before it), so that frame ends here.
        if (temporal_.active()) temporal_.end_frame();
        if (callback) callback(swap);
      });
}

void GraphicsSystem::reset() {
  // The backend may still be playing IR that reads the EDRAM store.
  executor_.wait_idle();
  command_processor_.reset();
  edram_.reset();
  temporal_.reset();
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

std::uint32_t GraphicsSystem::submit_ring(Backend& backend, std::uint32_t physical_address,
                                          std::uint32_t capacity_dwords,
                                          std::uint32_t read_index,
                                          std::uint32_t write_index) {
  // Only a decoded draw (a resolve is one) can change what the guest
  // observes; register writes alone wait for the next draw or the end. The
  // barrier executes that work but leaves it in the open batch: anything
  // that needs its results (a resolve's readback, a wait) submits and waits
  // itself, and the drain submits at its end. Submitting at every barrier
  // made each of AC6's draws its own GPU batch - with three in flight, every
  // draw waited a GPU round trip.
  //
  // Resolve readbacks are queued with the work and not waited for (see
  // Backend::publish_gpu_writes()). Before the guest can observe that the
  // GPU got past a point - a fence or memory write, an interrupt, a memory
  // poll - every readback queued before it is waited for and written to
  // guest memory, so a title that sees the fence also sees the resolve.
  //
  // GPU V3 (start_backend_thread()): a drain starts out executing its IR on
  // this thread, as a short drain - AC6 runs hundreds a second, a few draws
  // each, every one ending in a CPU handshake - is done before a hand-off
  // would pay for itself. Once it has decoded kStreamAfterDraws draws it
  // streams: draws go to the backend thread as they are decoded, and the
  // guest memory writes the stream makes (fences, MEM_WRITE, scratch
  // write-back, extents) become backend tasks behind the work before them,
  // so decoding goes on. Interrupts, flips, waits and the frontend's own
  // reads of guest memory still wait for everything before them; whichever
  // thread executes it, the guest sees each effect only once the work before
  // it is complete, as when one thread did both.
  //
  // Pipelined drains (pipelined_drains()): a drain's last work is handed over
  // and not waited for, so the next drain decodes while it executes - as the
  // Xenos command processor fetches ahead of the shader pipes. What the guest
  // could observe of it is ordered behind it: its deferred writes, the read
  // pointer (write_guest_after_work()), and the next drain's barriers.
  drain_streaming_ = false;
  drain_first_draw_ = command_processor_.draw_count();
  command_processor_.set_guest_visible_barrier([this, &backend] {
    // Which kind of effect held the frontend, and for how long
    // (perf_frame.log "barriers").
    const bool held = executor_.running() && !executor_.idle();
    const auto started = std::chrono::steady_clock::now();
    note_hold(perf::Counter::HoldsBarrier);
    run_ir_then(backend, /*all=*/false, /*submit=*/false, [&backend] {
      if (!backend.publish_gpu_writes(/*wait=*/true)) {
        logging::Logger::instance().log(logging::Level::Error, "gpu",
                                        "resolve readback failed: " + backend.last_error());
      }
    });
    if (!held) return;
    using Kind = CommandProcessor::BarrierKind;
    const auto kind = command_processor_.last_barrier_kind();
    const auto [count, time] =
        kind == Kind::Interrupt ? std::pair{perf::Counter::BarrierInterrupts,
                                            perf::Counter::BarrierInterruptNs}
        : kind == Kind::Swap    ? std::pair{perf::Counter::BarrierSwaps, perf::Counter::BarrierSwapNs}
        : kind == Kind::Wait    ? std::pair{perf::Counter::BarrierWaits, perf::Counter::BarrierWaitNs}
                                : std::pair{perf::Counter::BarrierWrites, perf::Counter::BarrierWriteNs};
    perf::add(count, 1u);
    perf::add(time, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - started)
                                                   .count()));
  });
  // A sync point captures guest memory as it stands now, for the commands
  // after it (Backend::sync_guest_reads()): the CPU code it releases may
  // rewrite what they read. So it runs now, on the frontend's timeline, once
  // the commands before it - which read the previous point's memory - have
  // executed.
  command_processor_.set_sync_point_callback([this, &backend] {
    note_hold(perf::Counter::HoldsSync);
    run_ir_then(backend, /*all=*/false, /*submit=*/false,
                [&backend] { backend.sync_guest_reads(); });
  });
  if (executor_.running()) {
    // Streaming: hand draws over as they are decoded - at once when the
    // backend has run dry, in small groups while it is busy - so its queue
    // stays short (a barrier waits for all of it).
    command_processor_.set_draw_emitted_callback([this, &backend] {
      if (!streaming()) return;
      constexpr std::uint32_t kDrawsPerHandOff = 8;
      if (++draws_since_hand_off_ >= kDrawsPerHandOff || executor_.idle())
        execute_ir(backend, /*submit=*/false);
    });
    command_processor_.set_guest_write_sink(
        [this, &backend](std::uint32_t address, std::vector<std::byte> bytes,
                         bool skip_if_unchanged) {
          auto write = [this, &backend, address, bytes, skip_if_unchanged] {
            if (!backend.publish_gpu_writes(/*wait=*/true)) {
              logging::Logger::instance().log(logging::Level::Error, "gpu",
                                              "resolve readback failed: " + backend.last_error());
            }
            make_guest_write(address, bytes, skip_if_unchanged);
          };
          if (!streaming() && executor_.idle()) {
            // Nothing is pending (an earlier pipelined drain's writes and
            // resolves included), so guest memory is current: a write that
            // changes nothing is no barrier (AC6 issues one after nearly
            // every draw; making each a barrier executed the draws one at a
            // time).
            if (skip_if_unchanged) {
              std::vector<std::byte> current(bytes.size());
              if (memory_.copy_physical_range(address, current) && current == bytes) {
                command_processor_.note_deferred_extent_write(true);
                return;
              }
            }
            run_ir_then(backend, /*all=*/false, /*submit=*/false, write);
            return;
          }
          if (command_processor_.draw_count() != draws_at_last_execute_)
            execute_ir(backend, /*submit=*/false);
          const auto size = static_cast<std::uint32_t>(bytes.size());
          defer_guest_write(address, std::move(write), size);
        });
    // A read waits only for the latest pending deferred write to the bytes
    // it reads: the deferred writes are fences and extents at a few small
    // locations, and the reads mostly constants loaded from elsewhere -
    // waiting on every pending write held AC6's cutscenes up once every two
    // or three draws.
    command_processor_.set_guest_read_fence([this](std::uint32_t address, std::uint32_t size) {
      while (!deferred_writes_.empty() && executor_.done(deferred_writes_.front().sequence))
        deferred_writes_.pop_front();
      const std::uint64_t end = std::uint64_t{address} + size;
      std::uint64_t wait = 0;
      for (const auto& write : deferred_writes_)
        if (write.begin < end && address < write.end) wait = write.sequence;
      if (!wait || executor_.done(wait)) return;
      perf::add(perf::Counter::HoldsRead, 1u);
      executor_.wait_until_done(wait);
    });
  }
  struct ClearBarrier {
    GraphicsSystem& system;
    Backend& backend;
    ~ClearBarrier() {
      system.command_processor_.set_guest_visible_barrier({});
      system.command_processor_.set_sync_point_callback({});
      system.command_processor_.set_draw_emitted_callback({});
      system.command_processor_.set_guest_write_sink({});
      system.command_processor_.set_guest_read_fence({});
      if (system.pipelined_drains() && !system.executor_.idle()) {
        // Behind the drain's work, which is still executing; its pending
        // writes stay tracked for the next drain's reads.
        system.executor_.post([&backend = backend] { backend.end_guest_read_sync(); });
        return;
      }
      system.deferred_writes_.clear();
      system.executor_.run([&backend = backend] { backend.end_guest_read_sync(); });
    }
  } clear{*this, backend};
  // Work the guest just published starts from guest memory as it stands now;
  // a drain resuming a parked wait continues from the sync point that wait
  // reaches when it is satisfied.
  //
  // While an earlier pipelined drain still executes, the sync point takes its
  // place behind that work, as a busy GPU reaches newly published work only
  // once it is done with what came before.
  if (!command_processor_.parked_on_wait()) {
    if (pipelined_drains() && !executor_.idle()) {
      executor_.post([&backend] { backend.sync_guest_reads(); });
    } else {
      note_hold(perf::Counter::HoldsSync);
      executor_.run([&backend] { backend.sync_guest_reads(); });
    }
  }
  if (frame_capture_ && !frame_capture_->started)
    begin_frame_capture(backend, physical_address, capacity_dwords, read_index);
  const auto next_read_index =
      command_processor_.execute_ring(physical_address, capacity_dwords, read_index, write_index);
  // A short drain with the backend idle finishes here, without a hand-off;
  // readbacks the GPU has already finished land now, without waiting.
  //
  // Pipelined, the last work of a drain that streamed (or that finds the
  // backend still busy) is handed over and left running. The caller writes
  // the read pointer back at once - the decoded commands no longer need the
  // ring words - unless a draw of the drain reads data out of command memory
  // (D3D's resolve and clear rectangles, DrawPrimitiveUP vertices): then
  // with write_guest_after_work(), so the guest reuses that memory only once
  // the draw has read it (CommandProcessor::drain_reads_command_memory()).
  const auto publish = [&backend] {
    if (!backend.publish_gpu_writes(/*wait=*/false)) {
      logging::Logger::instance().log(logging::Level::Error, "gpu",
                                      "resolve readback failed: " + backend.last_error());
    }
  };
  if (pipelined_drains() && (drain_streaming_ || !executor_.idle())) {
    execute_ir(backend, /*submit=*/true);
    executor_.post(publish);
  } else {
    note_hold(perf::Counter::HoldsDrain);
    run_ir_then(backend, /*all=*/true, /*submit=*/true, publish);
  }
  report_backend_errors();
  if (frame_capture_ && frame_capture_->started) end_frame_capture_if_done(backend, next_read_index);
  return next_read_index;
}

void GraphicsSystem::arm_frame_capture(std::filesystem::path path, std::uint32_t frames) {
  frame_capture_ = std::make_unique<FrameCaptureRecording>();
  frame_capture_->path = std::move(path);
  frame_capture_->frames = (std::max)(frames, 1u);
}

void GraphicsSystem::begin_frame_capture(Backend& backend, std::uint32_t physical_address,
                                         std::uint32_t capacity_dwords,
                                         std::uint32_t read_index) {
  auto& recording = *frame_capture_;
  // Registers and shaders are the frontend's as decoded so far; they are
  // the commands' starting state only when everything decoded has been
  // handed over (a drain that failed may leave IR for the next one).
  if (stream_.size() != 0u) return;
  bool canonical = false;
  executor_.run([&] {
    backend.begin_submission(memory_, edram_);
    canonical = backend.make_edram_canonical();
    backend.end_submission();
    if (canonical) recording.edram.assign(edram_.bytes().begin(), edram_.bytes().end());
  });
  if (!canonical) {
    frame_capture_result_ = FrameCaptureResult{
        false, recording.path, "the backend could not make EDRAM canonical: " + backend.last_error()};
    frame_capture_.reset();
    return;
  }
  auto& frontend = recording.frontend;
  frontend.source = FrontendSubmissionCapture::Source::Ring;
  frontend.physical_address = physical_address;
  frontend.capacity_dwords = capacity_dwords;
  frontend.read_index = read_index;
  frontend.initial_registers = registers_.snapshot();
  frontend.initial_vertex_program = command_processor_.active_vertex_program();
  frontend.initial_pixel_program = command_processor_.active_pixel_program();
  recording.swaps_at_start = command_processor_.statistics().front_buffer_swaps;
  recording.started = true;
}

void GraphicsSystem::end_frame_capture_if_done(Backend& backend, std::uint32_t next_read_index) {
  auto& recording = *frame_capture_;
  ++recording.drains;
  if (command_processor_.statistics().front_buffer_swaps - recording.swaps_at_start <
      recording.frames)
    return;
  auto frontend = std::move(recording.frontend);
  frontend.write_index = next_read_index;
  frontend.resulting_read_index = next_read_index;
  frontend.final_registers = registers_.snapshot();
  FrameCaptureResult result{};
  result.path = recording.path;
  result.drains = recording.drains;
  result.commands = frontend.commands.size();
  result.draws = static_cast<std::uint64_t>(
      std::count_if(frontend.commands.begin(), frontend.commands.end(), [](const ir::Command& c) {
        return std::holds_alternative<ir::DrawPacket>(c);
      }));
  auto capture = std::make_unique<PortableSubmissionCapture>();
  bool built = false;
  // Behind the recorded work: the memory it reaches is copied once it has
  // executed (and its GPU writes are visible).
  executor_.run([&] {
    built = build_portable_capture(memory_, edram_, backend, std::move(frontend), *capture,
                                   &result.error, &recording.edram);
  });
  if (built) {
    capture->complete = false;
    capture->diagnostics.emplace_back(
        "Recorded live across " + std::to_string(result.drains) +
        " ring drains: guest memory was snapshotted when the recording ended, so bytes the CPU "
        "rewrote during it hold their final values.");
    result.physical_bytes = capture->physical_byte_count();
    std::error_code ignored;
    if (recording.path.has_parent_path())
      std::filesystem::create_directories(recording.path.parent_path(), ignored);
    result.ok = save_portable_capture(*capture, recording.path, &result.error);
  }
  frame_capture_result_ = std::move(result);
  frame_capture_.reset();
}

bool GraphicsSystem::streaming() {
  if (drain_streaming_) return true;
  if (!executor_.running() ||
      command_processor_.draw_count() - drain_first_draw_ < kStreamAfterDraws)
    return false;
  drain_streaming_ = true;
  return true;
}

void GraphicsSystem::run_ir_then(Backend& backend, bool all, bool submit,
                                 const std::function<void()>& then) {
  std::vector<ir::Command> commands;
  if (all || command_processor_.draw_count() != draws_at_last_execute_) {
    draws_at_last_execute_ = command_processor_.draw_count();
    draws_since_hand_off_ = 0;
    commands = stream_.take();
    record_for_capture(commands);
    if (executor_.running()) backend.prefetch_shaders(commands);
    if (temporal_.active())
      for (const auto& command : commands) temporal_.observe(command);
  }
  // Behind everything handed over, and waited for: on this thread when the
  // backend is idle, so a drain that never streamed costs no hand-off.
  executor_.run([&] {
    if (!commands.empty() || submit)
      consume_commands(backend, commands, submit, /*observe_temporal=*/false);
    if (then) then();
  });
}

void GraphicsSystem::defer_guest_write(std::uint32_t address, std::function<void()> write,
                                       std::uint32_t size) {
  const auto sequence = executor_.post_sequenced(std::move(write));
  deferred_writes_.push_back({address, std::uint64_t{address} + size, sequence});
}

void GraphicsSystem::write_guest_after_work(std::uint32_t address, std::vector<std::byte> bytes) {
  // Finished writes need no tracking.
  while (!deferred_writes_.empty() && executor_.done(deferred_writes_.front().sequence))
    deferred_writes_.pop_front();
  if (!executor_.running() || executor_.idle()) {
    make_guest_write(address, bytes, /*skip_if_unchanged=*/false);
    return;
  }
  const auto size = static_cast<std::uint32_t>(bytes.size());
  defer_guest_write(
      address,
      [this, address, bytes = std::move(bytes)] {
        make_guest_write(address, bytes, /*skip_if_unchanged=*/false);
      },
      size);
}

bool GraphicsSystem::write_guest_ahead_of_work(std::uint32_t address,
                                               std::span<const std::byte> bytes) {
  while (!deferred_writes_.empty() && executor_.done(deferred_writes_.front().sequence))
    deferred_writes_.pop_front();
  const std::uint64_t end = std::uint64_t{address} + bytes.size();
  const bool behind_deferred =
      std::any_of(deferred_writes_.begin(), deferred_writes_.end(),
                  [&](const DeferredWrite& write) {
                    return write.begin < end && address < write.end;
                  });
  if (behind_deferred) {
    write_guest_after_work(address, std::vector<std::byte>(bytes.begin(), bytes.end()));
    return false;
  }
  make_guest_write(address, bytes, /*skip_if_unchanged=*/false);
  return true;
}

void GraphicsSystem::make_guest_write(std::uint32_t address, std::span<const std::byte> bytes,
                                      bool skip_if_unchanged) {
  if (skip_if_unchanged) {
    std::vector<std::byte> current(bytes.size());
    const bool unchanged = memory_.copy_physical_range(address, current) &&
                           std::equal(current.begin(), current.end(), bytes.begin());
    command_processor_.note_deferred_extent_write(unchanged);
    if (unchanged) return;
  }
  if (!memory_.write_physical(address, bytes)) {
    char text[96];
    std::snprintf(text, sizeof(text), "GPU write to physical 0x%08X has no backing",
                  static_cast<unsigned>(address));
    logging::Logger::instance().log(logging::Level::Error, "gpu", text);
  }
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
  bool result = false;
  executor_.run([&] {
    result = build_portable_capture(memory_, edram_, backend, std::move(frontend), out, error);
  });
  return result;
}

bool GraphicsSystem::capture_portable_ring(
    Backend& backend, std::uint32_t physical_address,
    std::uint32_t capacity_dwords, std::uint32_t read_index,
    std::uint32_t write_index, PortableSubmissionCapture& out,
    std::string* error) {
  auto frontend = capture_ring(physical_address, capacity_dwords, read_index, write_index);
  bool result = false;
  executor_.run([&] {
    result = build_portable_capture(memory_, edram_, backend, std::move(frontend), out, error);
  });
  return result;
}

void GraphicsSystem::replay_capture(
    Backend& backend, const FrontendSubmissionCapture& capture) {
  executor_.run([&] {
    backend.begin_submission(memory_, edram_);
    for (std::uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
      backend.consume(ir::RegisterWrite{i, capture.initial_registers.values[i]});
    }
    replay_shader_preamble(backend, capture);
    for (const auto& command : capture.commands) backend.consume(command);
    backend.end_submission();
  });
}

bool GraphicsSystem::replay_portable_capture(
    Backend& backend, const PortableSubmissionCapture& capture,
    std::string* error, ReplayTiming* timing, bool restore_edram) {
  bool result = false;
  executor_.run([&] {
    result = replay_portable_capture_now(backend, capture, error, timing, restore_edram);
  });
  return result;
}

bool GraphicsSystem::replay_portable_capture_now(
    Backend& backend, const PortableSubmissionCapture& capture,
    std::string* error, ReplayTiming* timing, bool restore_edram) {
  using Clock = std::chrono::steady_clock;
  const auto restore_start = Clock::now();
  if (capture.schema_version != PortableSubmissionCapture::kSchemaVersion ||
      capture.edram.size() != Edram::kSize) {
    set_error(error, "portable GPU capture schema or EDRAM image is invalid");
    return false;
  }

  backend.begin_submission(memory_, edram_);
  if (restore_edram) {
    if (!backend.invalidate_edram_native_state()) {
      backend.end_submission();
      set_error(error, "backend could not discard stale native EDRAM ownership");
      return false;
    }
    edram_.write(0u, capture.edram);
  }
  for (const auto& range : capture.physical_ranges) {
    if (std::uint64_t(range.physical_address) + range.bytes.size() >
            memory::kPhysicalMemorySize ||
        !memory_.write_physical(range.physical_address, range.bytes)) {
      backend.end_submission();
      set_error(error, "portable GPU capture could not restore guest physical memory");
      return false;
    }
  }

  const auto consume_start = Clock::now();
  for (std::uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
    backend.consume(
        ir::RegisterWrite{i, capture.frontend.initial_registers.values[i]});
  }
  replay_shader_preamble(backend, capture.frontend);
  for (const auto& command : capture.frontend.commands) backend.consume(command);
  backend.end_submission();
  if (timing) {
    const auto end = Clock::now();
    timing->restore_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(consume_start - restore_start).count());
    timing->consume_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - consume_start).count());
  }
  return true;
}

void GraphicsSystem::execute_ir(Backend& backend, bool submit) {
  draws_at_last_execute_ = command_processor_.draw_count();
  draws_since_hand_off_ = 0;
  record_for_capture(stream_.commands());
  if (!executor_.running() || executor_.inside_task()) {
    consume_commands(backend, stream_.commands(), submit, /*observe_temporal=*/true);
    stream_.clear();
    return;
  }
  auto commands = stream_.take();
  // Shaders loaded in this stretch start compiling now, on workers, ahead of
  // the backend reaching them.
  backend.prefetch_shaders(commands);
  // The temporal analyzer belongs to the frontend: it sees the commands as
  // they are handed over, in order with the swaps that end its frames.
  if (temporal_.active())
    for (const auto& command : commands) temporal_.observe(command);
  // Nothing to play: at most the open batch to submit, in order - here when
  // the backend is idle, behind its work when a pipelined drain still runs.
  if (commands.empty()) {
    if (!submit) return;
    if (executor_.idle())
      executor_.run([&] { consume_commands(backend, {}, true, /*observe_temporal=*/false); });
    else
      executor_.post([this, &backend] {
        consume_commands(backend, {}, true, /*observe_temporal=*/false);
      });
    return;
  }
  executor_.post([this, &backend, commands = std::move(commands), submit] {
    consume_commands(backend, commands, submit, /*observe_temporal=*/false);
  });
}

void GraphicsSystem::consume_commands(Backend& backend,
                                      const std::vector<ir::Command>& commands, bool submit,
                                      bool observe_temporal) {
  using Clock = std::chrono::steady_clock;
  const auto begin_start = Clock::now();
  backend.begin_submission(memory_, edram_);
  const auto elapsed_ms = [](Clock::time_point since) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
  };
  if (const auto ms = elapsed_ms(begin_start); ms >= 50)
    logging::append_probe_log("ir_timing_diag.log", "begin_submission %lld ms\n", ms);
  // A slow command delays the drain and vsync work behind it; with probe
  // logs on, name the slow ones.
  // Regression (AC6 missions at 20 fps): the two clock reads per command ran
  // regardless - tens of thousands of register writes a frame made them 13%
  // of the pump thread.
  const bool temporal = observe_temporal && temporal_.active();
  const bool timed = logging::probe_logs_enabled();
  for (const auto& command : commands) {
    if (!timed) {
      if (temporal) temporal_.observe(command);
      backend.consume(command);
      continue;
    }
    const auto start = Clock::now();
    if (temporal) temporal_.observe(command);
    backend.consume(command);
    if (const auto ms = elapsed_ms(start); ms >= 50) {
      std::uint32_t detail = 0;
      if (const auto* draw = std::get_if<ir::DrawPacket>(&command)) detail = draw->index_count;
      if (const auto* load = std::get_if<ir::ShaderLoad>(&command)) detail = load->physical_address;
      logging::append_probe_log("ir_timing_diag.log", "command variant %zu took %lld ms (detail 0x%X)\n",
                                command.index(), ms, detail);
    }
  }
  const auto end_start = Clock::now();
  if (submit)
    backend.end_submission();
  else
    backend.end_submission_deferred();
  if (const auto ms = elapsed_ms(end_start); ms >= 50)
    logging::append_probe_log("ir_timing_diag.log", "end_submission %lld ms\n", ms);
}

void GraphicsSystem::start_backend_thread() {
  if (const char* value = std::getenv("XENON_GPU_PIPELINED_DRAINS");
      value && value[0] == '0' && value[1] == '\0')
    pipelined_drains_ = false;
  executor_.start();
}

void GraphicsSystem::note_hold(perf::Counter cause) {
  if (executor_.running() && !executor_.idle()) perf::add(cause, 1u);
}

void GraphicsSystem::stop_backend_thread() noexcept {
  executor_.stop();
  report_backend_errors();
}

void GraphicsSystem::report_backend_errors() {
  // A task that threw was contained on the backend thread; say what it was
  // (once per failure, not per drain).
  const auto error = executor_.take_error();
  if (!error.empty())
    logging::Logger::instance().log(logging::Level::Error, "gpu", "IR execution failed: " + error);
}

bool GraphicsSystem::make_guest_memory_cpu_visible(
    Backend& backend, std::uint32_t physical_address, std::uint32_t size) {
  bool result = false;
  executor_.run([&] { result = backend.make_guest_memory_cpu_visible(physical_address, size); });
  return result;
}

bool GraphicsSystem::make_edram_canonical(Backend& backend) {
  bool result = false;
  executor_.run([&] { result = backend.make_edram_canonical(); });
  return result;
}

PresentStatus GraphicsSystem::present(Backend& backend,
                                      const PresentationFrame& frame) {
  // The guest-visible swap counter advances when the command processor
  // executes a flip (CommandProcessor::set_swap_callback), not per host present.
  auto status = PresentStatus::Error;
  executor_.run([&] { status = backend.present(frame); });
  return status;
}

}  // namespace xenon::gpu
