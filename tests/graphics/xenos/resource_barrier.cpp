#include <cassert>
#include <iostream>

#include "xenon/gpu/resource_barrier.hpp"

using namespace xenon::gpu;

int main() {
  ResourceBarrierPlanner planner;
  constexpr NativeResourceId color = 1;
  constexpr NativeResourceId texture_alias = 2;

  assert(planner.request(color, ResourceUsage::ColorAttachment));
  assert(!planner.request(color, ResourceUsage::ColorAttachment));
  auto barriers = planner.flush();
  assert(barriers.size() == 1);
  assert(barriers[0].type == ResourceBarrierType::Transition);
  assert(barriers[0].before == ResourceUsage::Undefined);
  assert(barriers[0].after == ResourceUsage::ColorAttachment);

  assert(planner.request(color, ResourceUsage::CopySource));
  assert(planner.request(color, ResourceUsage::ColorAttachment));
  assert(planner.pending_count() == 0);
  assert(planner.state(color) == ResourceUsage::ColorAttachment);

  const ResourceSubresourceRange mip{3, 1};
  planner.set_initial_state(texture_alias, ResourceUsage::CopyDestination, mip);
  assert(planner.request(texture_alias, ResourceUsage::ShaderRead, mip));
  barriers = planner.flush();
  assert(barriers.size() == 1 && barriers[0].subresources == mip);

  assert(planner.alias(color, texture_alias, ResourceUsage::ShaderRead));
  assert(!planner.state(color));
  assert(planner.state(texture_alias) == ResourceUsage::ShaderRead);
  barriers = planner.flush();
  assert(barriers.size() == 1);
  assert(barriers[0].type == ResourceBarrierType::Aliasing);
  assert(barriers[0].alias_before == color);
  assert(barriers[0].alias_after == texture_alias);

  assert(planner.memory_dependency(texture_alias));
  assert(!planner.memory_dependency(texture_alias));
  barriers = planner.flush();
  assert(barriers.size() == 1);
  assert(barriers[0].type == ResourceBarrierType::MemoryDependency);

  planner.reset();
  assert(!planner.state(texture_alias));
  assert(planner.pending_count() == 0);
  std::cout << "xenon_resource_barrier_tests: ok\n";
}
