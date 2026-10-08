#pragma once

// Whether this machine has any Vulkan driver, asked of the Vulkan loader
// directly. GPU-less CI runners (GitHub's Windows images) ship a loader but no
// driver; tests skip their Vulkan checks there. The probe deliberately avoids
// Xenon's own Vulkan code, so where a device does exist (Linux CI runs Mesa
// llvmpipe) a regression in that code still fails the test instead of
// turning into a skip.

#include <cstdint>

#include <vulkan/vulkan.h>

namespace xenon::test {

inline bool vulkan_device_present() {
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &application;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) return false;
  std::uint32_t count = 0;
  const bool present =
      vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0;
  vkDestroyInstance(instance, nullptr);
  return present;
}

}  // namespace xenon::test
