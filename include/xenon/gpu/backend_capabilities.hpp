#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xenon::gpu {

enum class BackendKind : std::uint8_t { Vulkan, Direct3D12 };

struct BackendCapability {
  BackendKind kind{BackendKind::Vulkan};
  bool runtime_available{};
  bool development_files_available{};
  std::uint32_t api_version{};
  std::string detail{};
};

[[nodiscard]] std::vector<BackendCapability> discover_backend_capabilities();

}  // namespace xenon::gpu
