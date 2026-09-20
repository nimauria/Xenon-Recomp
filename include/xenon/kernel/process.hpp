#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "xenon/kernel/memory.hpp"
#include "xenon/kernel/module.hpp"
#include "xenon/kernel/object.hpp"
#include "xenon/kernel/thread.hpp"

namespace xenon::kernel {

// Minimal process state required by retail games
class KernelProcess final : public KernelObject {
 public:
  explicit KernelProcess(std::shared_ptr<KernelMemory> memory);

  [[nodiscard]] ThreadManager& thread_manager() { return thread_manager_; }
  [[nodiscard]] const ThreadManager& thread_manager() const { return thread_manager_; }

  [[nodiscard]] ModuleManager& module_manager() { return module_manager_; }
  [[nodiscard]] const ModuleManager& module_manager() const { return module_manager_; }

  [[nodiscard]] KernelMemory& memory() { return *memory_; }
  [[nodiscard]] const KernelMemory& memory() const { return *memory_; }

  [[nodiscard]] std::shared_ptr<KernelThread> main_thread() const;
  void set_main_thread(std::shared_ptr<KernelThread> thread);

  [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }
  [[nodiscard]] std::uint32_t exit_code() const noexcept;

  void set_exit_code(std::uint32_t exit_code);
  void terminate(std::uint32_t exit_code);

  // Environment variables (simplified)
  [[nodiscard]] std::string get_env(const std::string& name) const;
  void set_env(std::string name, std::string value);

 private:
  std::uint32_t process_id_;
  std::uint32_t exit_code_{0};
  std::shared_ptr<KernelMemory> memory_;
  ThreadManager thread_manager_;
  ModuleManager module_manager_;
  std::shared_ptr<KernelThread> main_thread_;

  mutable std::mutex env_mutex_;
  std::unordered_map<std::string, std::string> environment_;
};

}  // namespace xenon::kernel
