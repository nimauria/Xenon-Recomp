#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace xenon::gpu {

using NativeResourceId = std::uint64_t;

enum class ResourceUsage : std::uint8_t {
  Undefined,
  CopySource,
  CopyDestination,
  ShaderRead,
  ColorAttachment,
  DepthStencilRead,
  DepthStencilWrite,
  Present,
};

struct ResourceSubresourceRange {
  static constexpr std::uint32_t kAll = UINT32_MAX;
  std::uint32_t first{};
  std::uint32_t count{kAll};
  friend bool operator==(const ResourceSubresourceRange&,
                         const ResourceSubresourceRange&) = default;
};

enum class ResourceBarrierType : std::uint8_t {
  Transition,
  Aliasing,
  MemoryDependency,
};

struct PlannedResourceBarrier {
  ResourceBarrierType type{ResourceBarrierType::Transition};
  NativeResourceId resource{};
  NativeResourceId alias_before{};
  NativeResourceId alias_after{};
  ResourceUsage before{ResourceUsage::Undefined};
  ResourceUsage after{ResourceUsage::Undefined};
  ResourceSubresourceRange subresources{};
};

// Backend-neutral resource-state planner. It deliberately stores opaque IDs
// rather than native handles so EDRAM aliases and guest-memory mirrors can be
// planned once and translated to Vulkan/D3D12 barriers independently.
class ResourceBarrierPlanner {
 public:
  void reset() noexcept;
  void set_initial_state(
      NativeResourceId resource, ResourceUsage usage,
      ResourceSubresourceRange subresources = {}) noexcept;
  [[nodiscard]] bool request(
      NativeResourceId resource, ResourceUsage usage,
      ResourceSubresourceRange subresources = {});
  [[nodiscard]] bool alias(
      NativeResourceId before, NativeResourceId after, ResourceUsage after_usage,
      ResourceSubresourceRange subresources = {});
  [[nodiscard]] bool memory_dependency(NativeResourceId resource);
  [[nodiscard]] std::optional<ResourceUsage> state(
      NativeResourceId resource,
      ResourceSubresourceRange subresources = {}) const noexcept;
  [[nodiscard]] std::size_t pending_count() const noexcept {
    return pending_.size();
  }
  [[nodiscard]] std::vector<PlannedResourceBarrier> flush();

 private:
  struct Key {
    NativeResourceId resource{};
    ResourceSubresourceRange subresources{};
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
  };

  void forget(NativeResourceId resource) noexcept;

  std::unordered_map<Key, ResourceUsage, KeyHash> states_{};
  std::vector<PlannedResourceBarrier> pending_{};
};

}  // namespace xenon::gpu
