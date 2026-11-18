#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xenon::recomp {

// Where generated function shards begin under a byte budget, for functions in
// guest-address order with their generated sizes. Returns the index of each
// shard's first function (the first is always 0; empty for no functions).
//
// The boundaries are content-defined, so a regeneration rebuilds only the
// shards around a change. A greedy fill cuts wherever the budget runs out,
// so one function that grows, appears or disappears can move the boundaries
// after it until some shard's slack absorbs the difference - each moved
// shard recompiled. Here a shard ends before a function when either
//   - the function would take it past `budget_bytes` (a hard cut; a single
//     function larger than the budget still gets a shard of its own), or
//   - it has reached half the budget and the function is an anchor: a fixed
//     hash of its guest address picks about one function in twelve
//     (shard_anchor()).
// Cuts fall on anchors, which depend on addresses alone, so the layouts
// before and after a change meet again at the first anchor both cut at, and
// every shard from there on is byte-identical. `header_bytes` is counted
// once per shard.
//
// Measured on Ace Combat 6's generated functions (12,129, 281 MB, 1 MiB
// budget) over 300 random edits (a function growing, appearing or
// disappearing): 1.26 shards rewritten per edit on average and 5 at most,
// against 4.3 and 103 for the greedy fill; 358 shards instead of 282. A
// three-quarter window, or denser anchors, cut on the budget more often than
// on an anchor and kept most of the greedy fill's tail.
[[nodiscard]] std::vector<std::size_t> plan_shard_starts(std::span<const std::uint32_t> addresses,
                                                         std::span<const std::size_t> sizes,
                                                         std::size_t budget_bytes,
                                                         std::size_t header_bytes);

// Whether a function at `address` may begin a shard once the current one is
// full enough (plan_shard_starts()).
[[nodiscard]] bool shard_anchor(std::uint32_t address) noexcept;

}  // namespace xenon::recomp
