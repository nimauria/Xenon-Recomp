#include "xenon/gpu/resource_barrier.hpp"

#include <algorithm>
#include <utility>

namespace xenon::gpu {

std::size_t ResourceBarrierPlanner::KeyHash::operator()(
    const Key& key) const noexcept {
  auto value = key.resource;
  value ^= std::uint64_t(key.subresources.first) << 17u;
  value ^= std::uint64_t(key.subresources.count) << 41u;
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  return static_cast<std::size_t>(value);
}

void ResourceBarrierPlanner::reset() noexcept {
  states_.clear();
  pending_.clear();
}

void ResourceBarrierPlanner::set_initial_state(
    NativeResourceId resource, ResourceUsage usage,
    ResourceSubresourceRange subresources) noexcept {
  if (!resource) return;
  states_[Key{resource, subresources}] = usage;
}

bool ResourceBarrierPlanner::request(
    NativeResourceId resource, ResourceUsage usage,
    ResourceSubresourceRange subresources) {
  if (!resource) return false;
  const Key key{resource, subresources};
  const auto found = states_.find(key);
  const auto before = found == states_.end() ? ResourceUsage::Undefined
                                              : found->second;
  if (before == usage) return false;
  states_[key] = usage;

  // Several state changes may be requested while the backend is assembling a
  // native dependency batch. Preserve the first source and final destination,
  // dropping a round trip that returns to its original state.
  const auto pending = std::find_if(
      pending_.rbegin(), pending_.rend(), [&](const PlannedResourceBarrier& b) {
        return b.type == ResourceBarrierType::Transition &&
               b.resource == resource && b.subresources == subresources;
      });
  if (pending != pending_.rend()) {
    pending->after = usage;
    if (pending->before == pending->after) {
      pending_.erase(std::next(pending).base());
    }
    return true;
  }
  pending_.push_back({ResourceBarrierType::Transition, resource, 0, 0, before,
                      usage, subresources});
  return true;
}

void ResourceBarrierPlanner::forget(NativeResourceId resource) noexcept {
  for (auto it = states_.begin(); it != states_.end();) {
    if (it->first.resource == resource)
      it = states_.erase(it);
    else
      ++it;
  }
}

bool ResourceBarrierPlanner::alias(
    NativeResourceId before, NativeResourceId after, ResourceUsage after_usage,
    ResourceSubresourceRange subresources) {
  if (!after || before == after) return false;
  if (before) forget(before);
  forget(after);
  states_[Key{after, subresources}] = after_usage;
  pending_.push_back({ResourceBarrierType::Aliasing, after, before, after,
                      ResourceUsage::Undefined, after_usage, subresources});
  return true;
}

bool ResourceBarrierPlanner::memory_dependency(NativeResourceId resource) {
  if (!resource) return false;
  if (!pending_.empty()) {
    const auto& last = pending_.back();
    if (last.type == ResourceBarrierType::MemoryDependency &&
        last.resource == resource) {
      return false;
    }
  }
  pending_.push_back({ResourceBarrierType::MemoryDependency, resource});
  return true;
}

std::optional<ResourceUsage> ResourceBarrierPlanner::state(
    NativeResourceId resource,
    ResourceSubresourceRange subresources) const noexcept {
  const auto found = states_.find(Key{resource, subresources});
  if (found == states_.end()) return std::nullopt;
  return found->second;
}

std::vector<PlannedResourceBarrier> ResourceBarrierPlanner::flush() {
  auto result = std::move(pending_);
  pending_.clear();
  return result;
}

}  // namespace xenon::gpu
