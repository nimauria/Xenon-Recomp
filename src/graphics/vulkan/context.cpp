#include "xenon/gpu/vulkan/context.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace xenon::gpu::vulkan {
namespace {
bool supports_device_extension(VkPhysicalDevice device, const char* name) {
  std::uint32_t count{};
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) !=
      VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                            extensions.data()) != VK_SUCCESS)
    return false;
  return std::any_of(extensions.begin(), extensions.end(), [&](const auto& e) {
    return std::strcmp(e.extensionName, name) == 0;
  });
}
}  // namespace

Context::~Context() { reset(); }

bool Context::initialize(const ContextConfig& config) {
  reset();
  error_.clear();
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Xenon Recomp";
  app.apiVersion = VK_API_VERSION_1_3;
  const char* validation = "VK_LAYER_KHRONOS_validation";
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  if (config.enable_validation) {
    instance_info.enabledLayerCount = 1;
    instance_info.ppEnabledLayerNames = &validation;
  }
  if (vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
    error_ = "vkCreateInstance failed (Vulkan 1.3 and optional validation layer required)";
    return false;
  }
  std::uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
  std::uint64_t best_score = 0;
  for (auto candidate : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);
    if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
        (VK_API_VERSION_MAJOR(props.apiVersion) == 1 &&
         VK_API_VERSION_MINOR(props.apiVersion) < 3)) continue;
    VkPhysicalDeviceVulkan12Features candidate12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features candidate13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    candidate12.pNext = &candidate13;
    VkPhysicalDeviceFeatures2 candidate_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    candidate_features.pNext = &candidate12;
    vkGetPhysicalDeviceFeatures2(candidate, &candidate_features);
    if (!candidate12.timelineSemaphore || !candidate13.synchronization2 ||
        !candidate13.dynamicRendering ||
        !candidate_features.features.sampleRateShading) {
      continue;
    }
    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
    for (std::uint32_t family = 0; family < queue_count; ++family) {
      if (!(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      const std::uint64_t score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : props.limits.maxImageDimension2D;
      if (score > best_score) {
        best_score = score;
        physical_device_ = candidate;
        graphics_queue_family_ = family;
        properties_.device_name = props.deviceName;
        properties_.api_version = props.apiVersion;
        properties_.device_type = props.deviceType;
      }
      break;
    }
  }
  if (!physical_device_) {
    reset();
    error_ = "no Vulkan 1.3 graphics device found";
    return false;
  }
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory);
  for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
    if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      properties_.device_local_bytes += memory.memoryHeaps[i].size;
  }
  VkPhysicalDeviceVulkan12Features features12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features features13{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features12.pNext = &features13;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &features12;
  vkGetPhysicalDeviceFeatures2(physical_device_, &features);
  if (!features12.timelineSemaphore || !features13.synchronization2 ||
      !features13.dynamicRendering) {
    reset();
    error_ = "Vulkan device lacks timeline semaphore, synchronization2 or dynamic rendering";
    return false;
  }
  features12.timelineSemaphore = VK_TRUE;
  features13.synchronization2 = VK_TRUE;
  features13.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceFeatures enabled_features{};
  enabled_features.samplerAnisotropy = features.features.samplerAnisotropy;
  // Needed for Xenos dual polygon point/line modes. If unsupported, ordinary
  // filled rendering remains available and non-solid pipeline creation will
  // fail explicitly rather than being silently rendered as fill.
  enabled_features.fillModeNonSolid = features.features.fillModeNonSolid;
  // Exact Xenos EDRAM ownership transfers address individual MSAA samples.
  // Sample-rate fragment execution is required so writes can preserve every
  // unrelated host sample instead of using a native averaging resolve.
  enabled_features.sampleRateShading = VK_TRUE;
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = graphics_queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  const char* device_extensions[]{VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME};
  if (supports_device_extension(physical_device_, device_extensions[0])) {
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
  }
  device_info.pNext = &features12;
  device_info.pEnabledFeatures = &enabled_features;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
    reset();
    error_ = "vkCreateDevice failed";
    return false;
  }
  vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
  return true;
}

void Context::reset() noexcept {
  if (device_) vkDestroyDevice(device_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  device_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
  physical_device_ = VK_NULL_HANDLE;
  graphics_queue_ = VK_NULL_HANDLE;
  graphics_queue_family_ = 0;
  properties_ = {};
}

}  // namespace xenon::gpu::vulkan
