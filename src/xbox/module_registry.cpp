#include "xenon/xbox/module_registry.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/memory/types.hpp"
#include "xenon/xbox/xex_loader.hpp"

namespace xenon::xbox {
namespace {

// System modules a title can look up by name. The first name is the canonical
// export-registry library; the rest are accepted spellings.
struct SystemModuleSpec {
  const char* library;
  const char* alias;
};
constexpr SystemModuleSpec kSystemModules[] = {
    {"xboxkrnl.exe", "xboxkrnl"},
    {"xam.xex", "xam"},
    {"xbdm.xex", "xbdm"},
};

constexpr std::uint32_t kBlr = 0x4E800020u;

// LDR_DATA_TABLE_ENTRY-compatible field offsets Xenon populates for a system
// module (the executable's record is owned by the session, which fills the
// header pointer at +0x58 itself).
constexpr std::uint32_t kRecordFullNameOffset = 0x24u;
constexpr std::uint32_t kRecordBaseNameOffset = 0x2Cu;
constexpr std::uint32_t kRecordLoadCountOffset = 0x40u;
constexpr std::uint32_t kRecordNameBufferOffset = 0x80u;
constexpr std::uint32_t kRecordSize = 0x1000u;

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// A title may look a module up by bare name or by device path
// ("\Device\Harddisk0\...\xam.xex"): only the final component identifies it.
std::string basename_lower(std::string_view name) {
  const auto cut = name.find_last_of("\\/:");
  if (cut != std::string_view::npos) name.remove_prefix(cut + 1);
  return lower(name);
}

std::string export_key(const std::string& library, std::uint32_t ordinal) {
  return library + "#" + std::to_string(ordinal);
}

}  // namespace

GuestModuleRegistry::GuestModuleRegistry(memory::AddressSpace& memory,
                                         const core::ExportRegistry& exports)
    : memory_(memory), exports_(exports) {}

void GuestModuleRegistry::set_executable(cpu::GuestAddress record, const XexImage& image,
                                         std::vector<std::string> names) {
  std::scoped_lock lock(mutex_);
  if (executable_record_ != 0u) modules_.erase(executable_record_);
  Module module{};
  module.record = record;
  module.executable = true;
  for (const auto& item : image.exports) {
    module.exports.push_back({item.name, item.ordinal, item.address});
  }
  modules_[record] = std::move(module);
  executable_record_ = record;
  executable_names_.clear();
  for (auto& name : names) executable_names_.push_back(basename_lower(name));
  if (!image.original_pe_name.empty()) {
    executable_names_.push_back(basename_lower(image.original_pe_name));
  }
}

void GuestModuleRegistry::clear_executable() {
  std::scoped_lock lock(mutex_);
  if (executable_record_ != 0u) modules_.erase(executable_record_);
  executable_record_ = 0u;
  executable_names_.clear();
}

std::optional<cpu::GuestAddress> GuestModuleRegistry::system_module_locked(
    const std::string& library, std::string_view display) {
  for (const auto& [record, module] : modules_) {
    if (!module.executable && module.library == library) return record;
  }
  memory::GuestAddress record{};
  if (!memory_.allocate(kRecordSize, memory::kBasePageSize, memory::kReadWrite,
                        /*top_down=*/true, record)) {
    return std::nullopt;
  }
  // Name string (UTF-16BE, NUL-terminated) after the fixed fields.
  const auto name_bytes = static_cast<std::uint16_t>(display.size() * 2u);
  for (std::size_t i = 0; i < display.size(); ++i) {
    memory_.write16_be(record + kRecordNameBufferOffset + static_cast<std::uint32_t>(i) * 2u,
                       static_cast<std::uint16_t>(static_cast<unsigned char>(display[i])));
  }
  memory_.write16_be(record + kRecordNameBufferOffset + name_bytes, 0u);
  for (const auto offset : {kRecordFullNameOffset, kRecordBaseNameOffset}) {
    memory_.write16_be(record + offset + 0u, name_bytes);
    memory_.write16_be(record + offset + 2u, static_cast<std::uint16_t>(name_bytes + 2u));
    memory_.write32_be(record + offset + 4u, record + kRecordNameBufferOffset);
  }
  memory_.write16_be(record + kRecordLoadCountOffset, 1u);

  Module module{};
  module.library = library;
  module.record = record;
  modules_[record] = std::move(module);
  return record;
}

std::optional<cpu::GuestAddress> GuestModuleRegistry::module_handle(std::string_view name) {
  std::scoped_lock lock(mutex_);
  if (name.empty()) {
    if (executable_record_ == 0u) return std::nullopt;
    return executable_record_;
  }
  const auto wanted = basename_lower(name);
  if (executable_record_ != 0u &&
      std::find(executable_names_.begin(), executable_names_.end(), wanted) !=
          executable_names_.end()) {
    return executable_record_;
  }
  for (const auto& spec : kSystemModules) {
    if (wanted == spec.library || wanted == spec.alias) {
      return system_module_locked(spec.library, spec.library);
    }
  }
  return std::nullopt;
}

GuestModuleRegistry::Module* GuestModuleRegistry::module_for_handle_locked(
    cpu::GuestAddress handle) {
  const auto it = modules_.find(handle);
  return it == modules_.end() ? nullptr : &it->second;
}

bool GuestModuleRegistry::mint_thunk_locked(const std::string& library, std::uint32_t ordinal,
                                            cpu::GuestAddress& out) {
  const auto key = export_key(library, ordinal);
  if (const auto found = thunk_by_export_.find(key); found != thunk_by_export_.end()) {
    out = found->second;
    return true;
  }
  if (thunk_page_ == 0u || thunk_page_used_ + kThunkSize > kThunkPageBytes) {
    memory::GuestAddress page{};
    // Thunks are guest code addresses (a caller branches to them), so the page
    // is executable; it is written once per thunk at mint time.
    if (!memory_.allocate(kThunkPageBytes, memory::kLargePageSize,
                          memory::Protect::Read | memory::Protect::Write |
                              memory::Protect::Execute,
                          /*top_down=*/true, page)) {
      return false;
    }
    thunk_page_ = page;
    thunk_page_used_ = 0u;
  }
  const auto address = thunk_page_ + thunk_page_used_;
  thunk_page_used_ += kThunkSize;
  for (std::uint32_t word = 0; word < kThunkSize; word += 4u) {
    memory_.write32_be(address + word, kBlr);
  }
  thunk_by_address_[address] = ExportTarget{library, ordinal};
  thunk_by_export_[key] = address;
  out = address;
  return true;
}

GuestModuleRegistry::ProcedureResult GuestModuleRegistry::procedure_address(
    cpu::GuestAddress handle, std::uint32_t ordinal, cpu::GuestAddress& out) {
  std::scoped_lock lock(mutex_);
  // Handle 0 means "the calling executable", as for XexGetModuleHandle(NULL).
  Module* module = module_for_handle_locked(handle != 0u ? handle : executable_record_);
  if (module == nullptr) return ProcedureResult::InvalidHandle;
  if (module->executable) {
    for (const auto& item : module->exports) {
      if (item.ordinal == ordinal && item.address != 0u) {
        out = item.address;
        return ProcedureResult::Found;
      }
    }
    return ProcedureResult::OrdinalNotFound;
  }
  if (const auto variable = exports_.resolve_variable(module->library, ordinal)) {
    out = *variable;
    return ProcedureResult::Found;
  }
  if (exports_.resolve(module->library, ordinal) == nullptr) {
    return ProcedureResult::OrdinalNotFound;
  }
  return mint_thunk_locked(module->library, ordinal, out) ? ProcedureResult::Found
                                                           : ProcedureResult::OrdinalNotFound;
}

GuestModuleRegistry::ProcedureResult GuestModuleRegistry::procedure_address_by_name(
    cpu::GuestAddress handle, std::string_view name, cpu::GuestAddress& out) {
  std::scoped_lock lock(mutex_);
  Module* module = module_for_handle_locked(handle != 0u ? handle : executable_record_);
  if (module == nullptr) return ProcedureResult::InvalidHandle;
  if (module->executable) {
    for (const auto& item : module->exports) {
      if (item.name == name && item.address != 0u) {
        out = item.address;
        return ProcedureResult::Found;
      }
    }
    return ProcedureResult::EntryPointNotFound;
  }
  if (const auto variable = exports_.resolve_variable(module->library, name)) {
    out = *variable;
    return ProcedureResult::Found;
  }
  const auto* descriptor = exports_.resolve(module->library, name);
  if (descriptor == nullptr) return ProcedureResult::EntryPointNotFound;
  return mint_thunk_locked(module->library, descriptor->ordinal, out)
             ? ProcedureResult::Found
             : ProcedureResult::EntryPointNotFound;
}

std::optional<GuestModuleRegistry::ExportTarget> GuestModuleRegistry::thunk_target(
    cpu::GuestAddress address) const {
  std::scoped_lock lock(mutex_);
  const auto it = thunk_by_address_.find(address);
  if (it == thunk_by_address_.end()) return std::nullopt;
  return it->second;
}

bool GuestModuleRegistry::is_thunk(cpu::GuestAddress address) const {
  std::scoped_lock lock(mutex_);
  return thunk_by_address_.contains(address);
}

std::size_t GuestModuleRegistry::thunk_count() const {
  std::scoped_lock lock(mutex_);
  return thunk_by_address_.size();
}

bool GuestModuleRegistry::is_module_handle(cpu::GuestAddress handle) const {
  std::scoped_lock lock(mutex_);
  return modules_.contains(handle);
}

}  // namespace xenon::xbox
