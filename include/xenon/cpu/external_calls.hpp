#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/cpu/memory_port.hpp"
#include "xenon/cpu/state.hpp"

namespace xenon::cpu {

struct ExternalCallDescriptor {
  std::string module{};
  std::uint32_t ordinal{};
  std::string name{};
};

using ExternalCallHandler = std::function<bool(CpuState&, MemoryPort&)>;

// Temporary CPU-v1/runtime registration surface. It deliberately models only
// module + ordinal + PPC state/memory, so CPU v2 can reuse or replace the
// dispatch implementation without changing subsystems such as Input.
class ExternalCallRegistry {
 public:
  [[nodiscard]] bool register_call(std::string module, std::uint32_t ordinal,
                                   std::string name, ExternalCallHandler handler);
  [[nodiscard]] bool unregister_call(std::string_view module,
                                     std::uint32_t ordinal);
  [[nodiscard]] bool dispatch(std::string_view module, std::uint32_t ordinal,
                              CpuState& state, MemoryPort& memory) const;
  [[nodiscard]] bool contains(std::string_view module, std::uint32_t ordinal) const;
  [[nodiscard]] std::vector<ExternalCallDescriptor> descriptors() const;

 private:
  struct Entry { std::string module; std::uint32_t ordinal{}; std::string name; ExternalCallHandler handler; };
  [[nodiscard]] static std::string key(std::string_view module, std::uint32_t ordinal);
  mutable std::mutex mutex_{};
  std::unordered_map<std::string, Entry> entries_{};
};

}  // namespace xenon::cpu
