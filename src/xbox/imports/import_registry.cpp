#include "xenon/xbox/imports.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace xenon::xbox {

std::string ImportRegistry::normalize_module(std::string_view module) {
  std::string normalized;
  normalized.reserve(module.size() + 4);
  for (const char c : module) {
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (normalized.size() >= 4 && normalized.ends_with(".dll")) {
    normalized.resize(normalized.size() - 4);
  } else if (normalized.size() >= 4 && normalized.ends_with(".exe")) {
    normalized.resize(normalized.size() - 4);
  }
  return normalized;
}

bool ImportRegistry::same_name(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto ac = static_cast<unsigned char>(a[i]);
    const auto bc = static_cast<unsigned char>(b[i]);
    if (std::tolower(ac) != std::tolower(bc)) return false;
  }
  return true;
}

bool ImportRegistry::register_import(ImportDescriptor descriptor) {
  if (descriptor.module.empty() || descriptor.name.empty() || !descriptor.thunk) return false;
  descriptor.module = normalize_module(descriptor.module);
  if (descriptor.module.empty()) return false;

  std::unique_lock lock(mutex_);
  const auto collision = std::find_if(imports_.begin(), imports_.end(), [&](const auto& existing) {
    return existing.module == descriptor.module &&
           (existing.ordinal == descriptor.ordinal || same_name(existing.name, descriptor.name));
  });
  if (collision != imports_.end()) {
    return collision->ordinal == descriptor.ordinal &&
           same_name(collision->name, descriptor.name) && collision->thunk == descriptor.thunk;
  }
  imports_.push_back(std::move(descriptor));
  return true;
}

const ImportDescriptor* ImportRegistry::resolve(std::string_view module,
                                                std::string_view name) const {
  const auto normalized = normalize_module(module);
  std::shared_lock lock(mutex_);
  const auto it = std::find_if(imports_.begin(), imports_.end(), [&](const auto& entry) {
    return entry.module == normalized && same_name(entry.name, name);
  });
  return it == imports_.end() ? nullptr : &*it;
}

const ImportDescriptor* ImportRegistry::resolve(std::string_view module,
                                                std::uint16_t ordinal) const {
  const auto normalized = normalize_module(module);
  std::shared_lock lock(mutex_);
  const auto it = std::find_if(imports_.begin(), imports_.end(), [&](const auto& entry) {
    return entry.module == normalized && entry.ordinal == ordinal;
  });
  return it == imports_.end() ? nullptr : &*it;
}

bool ImportRegistry::invoke(std::string_view module, std::string_view name,
                            ImportCallContext& context) const {
  ImportThunk thunk{};
  {
    const auto normalized = normalize_module(module);
    std::shared_lock lock(mutex_);
    const auto it = std::find_if(imports_.begin(), imports_.end(), [&](const auto& entry) {
      return entry.module == normalized && same_name(entry.name, name);
    });
    if (it == imports_.end()) return false;
    thunk = it->thunk;
  }
  thunk(context);
  return true;
}

bool ImportRegistry::invoke(std::string_view module, std::uint16_t ordinal,
                            ImportCallContext& context) const {
  ImportThunk thunk{};
  {
    const auto normalized = normalize_module(module);
    std::shared_lock lock(mutex_);
    const auto it = std::find_if(imports_.begin(), imports_.end(), [&](const auto& entry) {
      return entry.module == normalized && entry.ordinal == ordinal;
    });
    if (it == imports_.end()) return false;
    thunk = it->thunk;
  }
  thunk(context);
  return true;
}

std::vector<ImportDescriptor> ImportRegistry::enumerate(std::string_view module) const {
  const auto normalized = module.empty() ? std::string{} : normalize_module(module);
  std::shared_lock lock(mutex_);
  std::vector<ImportDescriptor> result;
  result.reserve(imports_.size());
  for (const auto& entry : imports_) {
    if (normalized.empty() || entry.module == normalized) result.push_back(entry);
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    if (a.module != b.module) return a.module < b.module;
    return a.ordinal < b.ordinal;
  });
  return result;
}

std::optional<std::uint32_t> PpcArgumentReader::u32(std::size_t index) const noexcept {
  if (index < 8) return static_cast<std::uint32_t>(cpu_.gpr[3 + index]);
  const auto sp = static_cast<std::uint32_t>(cpu_.gpr[1]);
  const std::uint64_t address64 = static_cast<std::uint64_t>(sp) + 0x54ull +
                                  static_cast<std::uint64_t>(index - 8) * 8ull;
  if (address64 > 0xFFFFFFFFull) return std::nullopt;
  const auto address = static_cast<memory::GuestAddress>(address64);
  const auto first = memory_.query(address);
  const auto last = memory_.query(static_cast<memory::GuestAddress>(address + 3u));
  if (!first || !last || first->state != memory::PageState::Committed ||
      last->state != memory::PageState::Committed ||
      !memory::has(first->current_protect, memory::Protect::Read) ||
      !memory::has(last->current_protect, memory::Protect::Read)) {
    return std::nullopt;
  }
  try {
    return memory_.read32_be(address);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace xenon::xbox
