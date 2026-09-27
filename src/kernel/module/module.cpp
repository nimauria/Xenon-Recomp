#include "xenon/kernel/module.hpp"

#include <algorithm>

namespace xenon::kernel {

KernelModule::KernelModule(std::string name, std::uint32_t base_address,
                          std::uint32_t size)
    : KernelObject(ObjectType::Module),
      name_(std::move(name)),
      base_address_(base_address),
      size_(size) {}

void KernelModule::add_export(ModuleExport export_entry) {
  std::scoped_lock lock(mutex_);
  if (!export_entry.name.empty()) {
    exports_by_name_[export_entry.name] = export_entry;
  }
  if (export_entry.ordinal != 0) {
    exports_by_ordinal_[export_entry.ordinal] = export_entry;
  }
}

std::optional<ModuleExport> KernelModule::find_export_by_name(
    std::string_view name) const {
  std::scoped_lock lock(mutex_);
  auto it = exports_by_name_.find(std::string(name));
  if (it != exports_by_name_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<ModuleExport> KernelModule::find_export_by_ordinal(
    std::uint32_t ordinal) const {
  std::scoped_lock lock(mutex_);
  auto it = exports_by_ordinal_.find(ordinal);
  if (it != exports_by_ordinal_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::vector<ModuleExport> KernelModule::exports() const {
  std::scoped_lock lock(mutex_);
  std::vector<ModuleExport> result;
  result.reserve(exports_by_name_.size());
  for (const auto& [name, export_entry] : exports_by_name_) {
    result.push_back(export_entry);
  }
  return result;
}

// ModuleManager implementation

std::shared_ptr<KernelModule> ModuleManager::load_module(
    std::string name, std::uint32_t base_address, std::uint32_t size) {
  std::scoped_lock lock(mutex_);
  
  auto module = std::make_shared<KernelModule>(name, base_address, size);
  modules_[name] = module;
  
  return module;
}

std::shared_ptr<KernelModule> ModuleManager::get_module(
    std::string_view name) const {
  std::scoped_lock lock(mutex_);
  auto it = modules_.find(std::string(name));
  return it != modules_.end() ? it->second : nullptr;
}

std::shared_ptr<KernelModule> ModuleManager::get_module_at_address(
    std::uint32_t address) const {
  std::scoped_lock lock(mutex_);
  
  for (const auto& [name, module] : modules_) {
    if (address >= module->base_address() &&
        address < module->base_address() + module->size()) {
      return module;
    }
  }
  
  return nullptr;
}

void ModuleManager::unload_module(std::string_view name) {
  std::scoped_lock lock(mutex_);
  modules_.erase(std::string(name));
}

std::vector<std::shared_ptr<KernelModule>> ModuleManager::enumerate_modules() const {
  std::scoped_lock lock(mutex_);
  std::vector<std::shared_ptr<KernelModule>> result;
  result.reserve(modules_.size());
  for (const auto& [name, module] : modules_) {
    result.push_back(module);
  }
  return result;
}

std::optional<std::uint32_t> ModuleManager::resolve_export(
    std::string_view module_name, std::string_view export_name) const {
  auto module = get_module(module_name);
  if (!module) {
    return std::nullopt;
  }
  
  auto export_entry = module->find_export_by_name(export_name);
  if (!export_entry) {
    return std::nullopt;
  }
  
  return export_entry->address;
}

std::optional<std::uint32_t> ModuleManager::resolve_export(
    std::string_view module_name, std::uint32_t ordinal) const {
  auto module = get_module(module_name);
  if (!module) {
    return std::nullopt;
  }
  
  auto export_entry = module->find_export_by_ordinal(ordinal);
  if (!export_entry) {
    return std::nullopt;
  }
  
  return export_entry->address;
}

}  // namespace xenon::kernel
