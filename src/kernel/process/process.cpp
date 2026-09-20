#include "xenon/kernel/process.hpp"

#include <atomic>

namespace xenon::kernel {
namespace {

std::atomic<std::uint32_t> g_next_process_id{1};

}  // namespace

KernelProcess::KernelProcess(std::shared_ptr<KernelMemory> memory)
    : KernelObject(ObjectType::Process),
      process_id_(g_next_process_id.fetch_add(1, std::memory_order_relaxed)),
      memory_(std::move(memory)) {}

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

}  // namespace xenon::kernel
