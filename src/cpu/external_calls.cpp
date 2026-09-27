#include "xenon/cpu/external_calls.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace xenon::cpu {
std::string ExternalCallRegistry::key(std::string_view module, std::uint32_t ordinal) {
  std::string result; result.reserve(module.size()+16);
  for (unsigned char c : module) result.push_back(static_cast<char>(std::tolower(c)));
  char suffix[16]{}; std::snprintf(suffix,sizeof(suffix),":%08x",ordinal); result += suffix; return result;
}
bool ExternalCallRegistry::register_call(std::string module, std::uint32_t ordinal,
                                         std::string name, ExternalCallHandler handler) {
  if (module.empty() || !handler) return false;
  Entry entry{std::move(module),ordinal,std::move(name),std::move(handler)};
  const auto k=key(entry.module,ordinal); std::scoped_lock lock(mutex_);
  return entries_.emplace(k,std::move(entry)).second;
}
bool ExternalCallRegistry::unregister_call(std::string_view module,std::uint32_t ordinal) {
  std::scoped_lock lock(mutex_); return entries_.erase(key(module,ordinal)) != 0;
}
bool ExternalCallRegistry::dispatch(std::string_view module,std::uint32_t ordinal,
                                    CpuState& state,MemoryPort& memory) const {
  ExternalCallHandler handler;
  { std::scoped_lock lock(mutex_); const auto it=entries_.find(key(module,ordinal)); if(it==entries_.end()) return false; handler=it->second.handler; }
  return handler(state,memory);
}
bool ExternalCallRegistry::contains(std::string_view module,std::uint32_t ordinal) const {
  std::scoped_lock lock(mutex_); return entries_.contains(key(module,ordinal));
}
std::vector<ExternalCallDescriptor> ExternalCallRegistry::descriptors() const {
  std::scoped_lock lock(mutex_); std::vector<ExternalCallDescriptor> out; out.reserve(entries_.size());
  for(const auto& [k,e]:entries_) { static_cast<void>(k); out.push_back({e.module,e.ordinal,e.name}); }
  std::sort(out.begin(),out.end(),[](const auto&a,const auto&b){ if(a.module!=b.module)return a.module<b.module; return a.ordinal<b.ordinal; }); return out;
}
}  // namespace xenon::cpu
