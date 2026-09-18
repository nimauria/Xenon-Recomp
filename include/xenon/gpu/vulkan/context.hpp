#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace xenon::gpu::vulkan {

struct ContextConfig {
  bool enable_validation{};
};

struct DeviceProperties {
  std::string device_name{};
  std::uint32_t api_version{};
  VkPhysicalDeviceType device_type{VK_PHYSICAL_DEVICE_TYPE_OTHER};
  std::uint64_t device_local_bytes{};
};

class Context {
 public:
  Context() = default;
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  [[nodiscard]] bool initialize(const ContextConfig& config = {});
  void reset() noexcept;
  [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
  [[nodiscard]] VkPhysicalDevice physical_device() const noexcept {
    return physical_device_;
  }
  [[nodiscard]] VkDevice device() const noexcept { return device_; }
  [[nodiscard]] VkQueue graphics_queue() const noexcept { return graphics_queue_; }
  [[nodiscard]] std::uint32_t graphics_queue_family() const noexcept {
    return graphics_queue_family_;
  }
  [[nodiscard]] const DeviceProperties& properties() const noexcept {
    return properties_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphics_queue_{VK_NULL_HANDLE};
  std::uint32_t graphics_queue_family_{};
  DeviceProperties properties_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
