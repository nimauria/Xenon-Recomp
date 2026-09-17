#include "xenon/gpu/vulkan/command_queue.hpp"

namespace xenon::gpu::vulkan {

CommandQueue::~CommandQueue() { reset(); }

bool CommandQueue::initialize(VkDevice device, VkQueue queue,
                              std::uint32_t queue_family) {
  reset();
  error_.clear();
  if (!device || !queue) {
    error_ = "Vulkan command queue requires a device and queue";
    return false;
  }
  device_ = device;
  queue_ = queue;
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
               VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool.queueFamilyIndex = queue_family;
  if (vkCreateCommandPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
    error_ = "vkCreateCommandPool failed";
    reset();
    return false;
  }
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  semaphore.pNext = &type;
  if (vkCreateSemaphore(device_, &semaphore, nullptr, &timeline_) != VK_SUCCESS) {
    error_ = "vkCreateSemaphore failed";
    reset();
    return false;
  }
  return true;
}

void CommandQueue::reset() noexcept {
  if (device_ && timeline_) vkDestroySemaphore(device_, timeline_, nullptr);
  if (device_ && pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  timeline_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  next_value_ = 1;
}

bool CommandQueue::execute(const std::function<void(VkCommandBuffer)>& record) {
  if (!device_ || !queue_ || !pool_ || !timeline_) {
    error_ = "Vulkan command queue is not initialized";
    return false;
  }
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = pool_;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &allocate, &command) != VK_SUCCESS) {
    error_ = "vkAllocateCommandBuffers failed";
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    error_ = "vkBeginCommandBuffer failed";
    vkFreeCommandBuffers(device_, pool_, 1, &command);
    return false;
  }
  try {
    record(command);
  } catch (...) {
    vkFreeCommandBuffers(device_, pool_, 1, &command);
    throw;
  }
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    error_ = "vkEndCommandBuffer failed";
    vkFreeCommandBuffers(device_, pool_, 1, &command);
    return false;
  }
  const auto value = next_value_++;
  VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  command_info.commandBuffer = command;
  VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = timeline_;
  signal.value = value;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  if (vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
    error_ = "vkQueueSubmit2 failed";
    vkFreeCommandBuffers(device_, pool_, 1, &command);
    return false;
  }
  VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wait.semaphoreCount = 1;
  wait.pSemaphores = &timeline_;
  wait.pValues = &value;
  const bool ok = vkWaitSemaphores(device_, &wait, UINT64_MAX) == VK_SUCCESS;
  if (!ok) error_ = "vkWaitSemaphores failed";
  vkFreeCommandBuffers(device_, pool_, 1, &command);
  return ok;
}

std::uint64_t CommandQueue::completed_value() const noexcept {
  std::uint64_t value = 0;
  if (device_ && timeline_) vkGetSemaphoreCounterValue(device_, timeline_, &value);
  return value;
}

}  // namespace xenon::gpu::vulkan
