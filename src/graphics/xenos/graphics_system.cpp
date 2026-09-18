#include "xenon/gpu/graphics_system.hpp"

namespace xenon::gpu {

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
  return backend.present(frame);
}

}  // namespace xenon::gpu
