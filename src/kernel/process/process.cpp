#include "xenon/kernel/process.hpp"

#include <atomic>
#include <cstdio>

#include "xenon/logging/probe_log.hpp"

namespace xenon::kernel {
namespace {

std::atomic<std::uint32_t> g_next_process_id{1};

}  // namespace

KernelProcess::KernelProcess(std::shared_ptr<KernelMemory> memory)
    : KernelObject(ObjectType::Process),
      process_id_(g_next_process_id.fetch_add(1, std::memory_order_relaxed)),
      memory_(std::move(memory)),
      guest_heap_(*memory_),
      pool_(*memory_) {}

std::shared_ptr<KernelThread> KernelProcess::main_thread() const {
  return main_thread_;
}

void KernelProcess::set_main_thread(std::shared_ptr<KernelThread> thread) {
  main_thread_ = std::move(thread);
}

std::uint32_t KernelProcess::exit_code() const noexcept {
  return exit_code_;
}

void KernelProcess::set_exit_code(std::uint32_t exit_code) {
  exit_code_ = exit_code;
}

void KernelProcess::terminate(std::uint32_t exit_code) {
  exit_code_ = exit_code;
  thread_manager_.shutdown();
  guest_heap_.release_all();
  pool_.release_all();
}

std::string KernelProcess::get_env(const std::string& name) const {
  std::scoped_lock lock(env_mutex_);
  auto it = environment_.find(name);
  return it != environment_.end() ? it->second : std::string{};
}

void KernelProcess::set_env(std::string name, std::string value) {
  std::scoped_lock lock(env_mutex_);
  environment_[std::move(name)] = std::move(value);
}

std::uint32_t KernelProcess::allocate_tls_slot() noexcept {
  std::scoped_lock lock(tls_mutex_);
  for (std::size_t slot = 0; slot < tls_free_slots_.size(); ++slot) {
    if (tls_free_slots_.test(slot)) {
      tls_free_slots_.reset(slot);
      return static_cast<std::uint32_t>(slot);
    }
  }
  return kTlsOutOfIndexes;
}

void KernelProcess::free_tls_slot(std::uint32_t slot) noexcept {
  std::scoped_lock lock(tls_mutex_);
  if (slot < tls_free_slots_.size()) {
    tls_free_slots_.set(slot);
  }
}

KernelProcess::GpuRingBufferState KernelProcess::gpu_ring_buffer() const noexcept {
  std::scoped_lock lock(gpu_mutex_);
  return gpu_ring_buffer_;
}

void KernelProcess::configure_gpu_ring_buffer(std::uint32_t base_address,
                                              std::uint32_t capacity_dwords) noexcept {
  std::scoped_lock lock(gpu_mutex_);
  {
    static std::atomic<int> _ring_init_diag_count{0};
    const int _n = _ring_init_diag_count.fetch_add(1) + 1;
    xenon::logging::append_probe_log(
        "ring_init_diag.log",
        "configure_gpu_ring_buffer call #%d: base=0x%08X capacity_dwords=%u (prev base=0x%08X prev capacity=%u prev write=%u prev read=%u)\n",
        _n, base_address, capacity_dwords, gpu_ring_buffer_.base_address,
        gpu_ring_buffer_.capacity_dwords, gpu_ring_buffer_.write_index,
        gpu_ring_buffer_.read_index);
  }
  gpu_ring_buffer_.base_address = base_address;
  gpu_ring_buffer_.capacity_dwords = capacity_dwords;
  gpu_ring_buffer_.write_index = 0;
  gpu_ring_buffer_.read_index = 0;
}

void KernelProcess::set_gpu_ring_buffer_write_index(std::uint32_t write_index) noexcept {
  std::scoped_lock lock(gpu_mutex_);
  {
    static std::atomic<int> _wptr_diag_count{0};
    const int _n = _wptr_diag_count.fetch_add(1) + 1;
    if (_n <= 80) {
      xenon::logging::append_probe_log("wptr_diag.log",
                                       "set_write_index call #%d: new=%u (prev=%u read=%u)\n", _n,
                                       write_index, gpu_ring_buffer_.write_index,
                                       gpu_ring_buffer_.read_index);
    }
  }
  gpu_ring_buffer_.write_index = write_index;
}

void KernelProcess::set_gpu_ring_buffer_read_index(std::uint32_t read_index) noexcept {
  std::scoped_lock lock(gpu_mutex_);
  {
    static std::atomic<int> _rptr_diag_count{0};
    const int _n = _rptr_diag_count.fetch_add(1) + 1;
    if (_n <= 80) {
      xenon::logging::append_probe_log("rptr_diag.log",
                                       "set_read_index call #%d: new=%u (prev=%u write=%u)\n", _n,
                                       read_index, gpu_ring_buffer_.read_index,
                                       gpu_ring_buffer_.write_index);
    }
  }
  gpu_ring_buffer_.read_index = read_index;
}

void KernelProcess::set_gpu_ring_buffer_rptr_writeback(std::uint32_t address) noexcept {
  std::scoped_lock lock(gpu_mutex_);
  gpu_ring_buffer_.rptr_writeback_address = address;
}

KernelProcess::GpuInterruptCallbackState KernelProcess::gpu_interrupt_callback() const noexcept {
  std::scoped_lock lock(gpu_mutex_);
  return gpu_interrupt_callback_;
}

void KernelProcess::set_gpu_interrupt_callback(std::uint32_t callback_address,
                                               std::uint32_t context) noexcept {
  std::scoped_lock lock(gpu_mutex_);
  gpu_interrupt_callback_.callback_address = callback_address;
  gpu_interrupt_callback_.context = context;
}

KernelProcess::GpuFrontBufferState KernelProcess::gpu_front_buffer() const noexcept {
  std::scoped_lock lock(gpu_mutex_);
  return gpu_front_buffer_;
}

void KernelProcess::set_gpu_front_buffer(std::uint32_t base_address, std::uint32_t width,
                                         std::uint32_t height, std::uint32_t pitch,
                                         std::uint8_t format) noexcept {
  std::scoped_lock lock(gpu_mutex_);
  gpu_front_buffer_.base_address = base_address;
  gpu_front_buffer_.width = width;
  gpu_front_buffer_.height = height;
  gpu_front_buffer_.pitch = pitch;
  gpu_front_buffer_.format = format;
  ++gpu_front_buffer_.generation;
}

}  // namespace xenon::kernel
