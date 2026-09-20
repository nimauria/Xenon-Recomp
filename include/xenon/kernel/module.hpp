#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

struct ModuleExport {
  std::string name;
  std::uint32_t ordinal{0};
  std::uint32_t address{0};
};

class KernelModule final : public KernelObject {
 public:
  explicit KernelModule(std::string name, std::uint32_t base_address,
                       std::uint32_t size);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] std::uint32_t base_address() const noexcept { return base_address_; }
  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
  [[nodiscard]] std::uint32_t entry_point() const noexcept { return entry_point_; }

  void set_entry_point(std::uint32_t entry_point) noexcept {
    entry_point_ = entry_point;
  }

  void add_export(ModuleExport export_entry);
  [[nodiscard]] std::optional<ModuleExport> find_export_by_name(
      std::string_view name) const;
  [[nodiscard]] std::optional<ModuleExport> find_export_by_ordinal(
      std::uint32_t ordinal) const;

  [[nodiscard]] std::vector<ModuleExport> exports() const;

  // TLS support
  void set_tls_info(std::uint32_t tls_address, std::uint32_t tls_size,
                    std::uint32_t tls_slot) {
    tls_address_ = tls_address;
    tls_size_ = tls_size;
    tls_slot_ = tls_slot;
  }

  [[nodiscard]] std::uint32_t tls_address() const noexcept { return tls_address_; }
  [[nodiscard]] std::uint32_t tls_size() const noexcept { return tls_size_; }
  [[nodiscard]] std::uint32_t tls_slot() const noexcept { return tls_slot_; }

 private:
  std::string name_;
  std::uint32_t base_address_;
  std::uint32_t size_;
  std::uint32_t entry_point_{0};
  std::uint32_t tls_address_{0};
  std::uint32_t tls_size_{0};
  std::uint32_t tls_slot_{0};

  mutable std::mutex mutex_;
  std::unordered_map<std::string, ModuleExport> exports_by_name_;
  std::unordered_map<std::uint32_t, ModuleExport> exports_by_ordinal_;
};

// Module manager for the kernel
class ModuleManager {
 public:
  ModuleManager() = default;

  [[nodiscard]] std::shared_ptr<KernelModule> load_module(
      std::string name, std::uint32_t base_address, std::uint32_t size);

  [[nodiscard]] std::shared_ptr<KernelModule> get_module(
      std::string_view name) const;

  [[nodiscard]] std::shared_ptr<KernelModule> get_module_at_address(
      std::uint32_t address) const;

  void unload_module(std::string_view name);

  [[nodiscard]] std::vector<std::shared_ptr<KernelModule>> enumerate_modules() const;

  [[nodiscard]] std::optional<std::uint32_t> resolve_export(
      std::string_view module_name, std::string_view export_name) const;

  [[nodiscard]] std::optional<std::uint32_t> resolve_export(
      std::string_view module_name, std::uint32_t ordinal) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<KernelModule>> modules_;
};

}  // namespace xenon::kernel
