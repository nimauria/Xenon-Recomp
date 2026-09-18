#include "xenon/gpu/vulkan/command_queue.hpp"

namespace xenon::gpu::vulkan {

CommandQueue::~CommandQueue() {
  (void)wait_idle();
  reset();
}

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
  for (auto& frame : frames_) {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool(device_, &pool, nullptr, &frame.pool) != VK_SUCCESS) {
      error_ = "vkCreateCommandPool failed";
      reset();
      return false;
    }
    VkCommandBufferAllocateInfo allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = frame.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate, &frame.command) != VK_SUCCESS) {
      error_ = "vkAllocateCommandBuffers failed";
      reset();
      return false;
    }
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
  timeline_ = VK_NULL_HANDLE;
  if (device_) {
    for (auto& frame : frames_) {
      if (frame.pool) vkDestroyCommandPool(device_, frame.pool, nullptr);
      frame = {};
    }
  } else {
    for (auto& frame : frames_) frame = {};
  }
  queue_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  next_frame_ = 0;
  current_frame_ = UINT32_MAX;
  next_value_ = 1;
  last_submitted_value_ = 0;
}

bool CommandQueue::wait_for_value(std::uint64_t value) {
  if (!value) return true;
  if (!device_ || !timeline_) {
    error_ = "Vulkan timeline wait requested on an uninitialized queue";
    return false;
  }
  VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wait.semaphoreCount = 1;
  wait.pSemaphores = &timeline_;
  wait.pValues = &value;
  if (vkWaitSemaphores(device_, &wait, UINT64_MAX) != VK_SUCCESS) {
    error_ = "vkWaitSemaphores failed";
    return false;
  }
  return true;
}

bool CommandQueue::begin_batch() {
  if (current_frame_ != UINT32_MAX) return true;
  if (!device_ || !queue_ || !timeline_) {
    error_ = "Vulkan command queue is not initialized";
    return false;
  }
  auto& frame = frames_[next_frame_];
  if (!wait_for_value(frame.timeline_value)) return false;
  if (vkResetCommandPool(device_, frame.pool, 0) != VK_SUCCESS) {
    error_ = "vkResetCommandPool failed";
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(frame.command, &begin) != VK_SUCCESS) {
    error_ = "vkBeginCommandBuffer failed";
    return false;
  }
  frame.command_count = 0;
  frame.recording = true;
  current_frame_ = next_frame_;
  return true;
}

bool CommandQueue::execute_async(const std::function<void(
    VkCommandBuffer, std::uint32_t, std::uint32_t)>& record) {
  if (!begin_batch()) return false;
  auto& frame = frames_[current_frame_];
  const auto draw_slot = frame.command_count;
  try {
    record(frame.command, current_frame_, draw_slot);
  } catch (...) {
    throw;
  }
  ++frame.command_count;
  if (frame.command_count >= kBatchCommandCount) return flush();
  return true;
}

bool CommandQueue::submit_current(VkSemaphore wait_semaphore,
                                  VkPipelineStageFlags2 wait_stage,
                                  VkSemaphore signal_semaphore) {
  if (current_frame_ == UINT32_MAX) return true;
  auto& frame = frames_[current_frame_];
  if (!frame.recording || !frame.command_count) {
    frame.recording = false;
    frame.command_count = 0;
    current_frame_ = UINT32_MAX;
    return true;
  }
  if (vkEndCommandBuffer(frame.command) != VK_SUCCESS) {
    error_ = "vkEndCommandBuffer failed";
    return false;
  }
  const auto value = next_value_++;
  VkCommandBufferSubmitInfo command_info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  command_info.commandBuffer = frame.command;

  VkSemaphoreSubmitInfo waits[1]{};
  std::uint32_t wait_count = 0;
  if (wait_semaphore) {
    waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[0].semaphore = wait_semaphore;
    waits[0].value = 0;
    waits[0].stageMask = wait_stage ? wait_stage
                                    : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    wait_count = 1;
  }
  VkSemaphoreSubmitInfo signals[2]{};
  signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signals[0].semaphore = timeline_;
  signals[0].value = value;
  signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  std::uint32_t signal_count = 1;
  if (signal_semaphore) {
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = signal_semaphore;
    signals[1].value = 0;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signal_count = 2;
  }
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = wait_count;
  submit.pWaitSemaphoreInfos = wait_count ? waits : nullptr;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  submit.signalSemaphoreInfoCount = signal_count;
  submit.pSignalSemaphoreInfos = signals;
  if (vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
    error_ = "vkQueueSubmit2 failed";
    return false;
  }
  frame.timeline_value = value;
  frame.recording = false;
  frame.command_count = 0;
  last_submitted_value_ = value;
  next_frame_ = (current_frame_ + 1u) % kFrameCount;
  current_frame_ = UINT32_MAX;
  return true;
}

bool CommandQueue::flush() {
  return submit_current(VK_NULL_HANDLE, VK_PIPELINE_STAGE_2_NONE,
                        VK_NULL_HANDLE);
}

bool CommandQueue::execute_present(
    const std::function<void(VkCommandBuffer)>& record,
    VkSemaphore wait_semaphore, VkPipelineStageFlags2 wait_stage,
    VkSemaphore signal_semaphore) {
  if (!flush() || !begin_batch()) return false;
  auto& frame = frames_[current_frame_];
  record(frame.command);
  frame.command_count = 1;
  return submit_current(wait_semaphore, wait_stage, signal_semaphore);
}

bool CommandQueue::execute(const std::function<void(VkCommandBuffer)>& record) {
  if (!flush() || !begin_batch()) return false;
  auto& frame = frames_[current_frame_];
  record(frame.command);
  frame.command_count = 1;
  if (!flush()) return false;
  return wait_for_value(last_submitted_value_);
}

bool CommandQueue::wait_idle() {
  if (!flush()) return false;
  if (!device_ || !timeline_) return true;
  return wait_for_value(last_submitted_value_);
}

std::uint64_t CommandQueue::completed_value() const noexcept {
  std::uint64_t value = 0;
  if (device_ && timeline_)
    vkGetSemaphoreCounterValue(device_, timeline_, &value);
  return value;
}

}  // namespace xenon::gpu::vulkan
