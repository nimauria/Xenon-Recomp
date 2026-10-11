#include "xenon/kernel/mutant.hpp"

#include <unordered_set>
#include <vector>

#include "kernel/synchronization/wait_internal.hpp"
#include "xenon/kernel/wait_util.hpp"

namespace xenon::kernel {
namespace {

// Every live mutant, so an exiting thread can find the ones it still owns.
// Lock order: this list, then a mutant's own mutex. Only the constructor and
// destructor take the list otherwise, and neither holds a mutant's mutex, so
// a mutant found here stays alive until the list is unlocked.
struct LiveMutants {
  std::mutex mutex;
  std::unordered_set<KernelMutant*> mutants;
};

// Never destroyed: guest threads may still exit during process teardown.
LiveMutants& live_mutants() {
  static auto* list = new LiveMutants();
  return *list;
}

}  // namespace

KernelMutant::KernelMutant(bool initial_owner, std::uint32_t owner_thread_id)
    : KernelObject(ObjectType::Mutant) {
  if (initial_owner) {
    owner_thread_id_ = owner_thread_id;
    owner_host_thread_ = std::this_thread::get_id();
    recursion_count_ = 1;
  }
  auto& live = live_mutants();
  std::scoped_lock lock(live.mutex);
  live.mutants.insert(this);
}

KernelMutant::~KernelMutant() {
  auto& live = live_mutants();
  std::scoped_lock lock(live.mutex);
  live.mutants.erase(this);
}

bool KernelMutant::acquire(std::uint32_t owner_thread_id, std::chrono::milliseconds timeout,
                           bool* abandoned) {
  std::unique_lock lock(mutex_);

  // Already owned by this thread: satisfied at once, deepening the recursion.
  // Otherwise wait for the mutant to become available.
  if (!wait_until_signaled(condition_, lock, timeout,
                           [&] { return can_satisfy_locked(owner_thread_id); })) {
    return false;
  }
  const bool took_abandoned = satisfy_locked(owner_thread_id);
  if (abandoned) *abandoned = took_abandoned;
  return true;
}

bool KernelMutant::release(std::uint32_t owner_thread_id) {
  std::scoped_lock lock(mutex_);

  if (owner_thread_id_ != owner_thread_id || recursion_count_ == 0) {
    return false;  // Not owned by this thread
  }

  --recursion_count_;
  if (recursion_count_ == 0) {
    owner_thread_id_ = 0;
    owner_host_thread_ = {};
    condition_.notify_one();
    detail::notify_multi_object_waiters();
  }

  return true;
}

bool KernelMutant::is_owned() const {
  std::scoped_lock lock(mutex_);
  return recursion_count_ > 0;
}

std::uint32_t KernelMutant::owner_thread_id() const {
  std::scoped_lock lock(mutex_);
  return owner_thread_id_;
}

std::int32_t KernelMutant::recursion_count() const {
  std::scoped_lock lock(mutex_);
  return recursion_count_;
}

void KernelMutant::abandon() {
  std::scoped_lock lock(mutex_);
  abandon_locked();
}

void KernelMutant::abandon_locked() noexcept {
  abandoned_ = true;
  owner_thread_id_ = 0;
  owner_host_thread_ = {};
  recursion_count_ = 0;
  condition_.notify_all();
  detail::notify_multi_object_waiters();
}

void abandon_mutants_owned_by_this_thread() noexcept {
  const auto self = std::this_thread::get_id();
  auto& live = live_mutants();
  std::scoped_lock list_lock(live.mutex);
  for (auto* mutant : live.mutants) {
    std::scoped_lock lock(mutant->mutex_);
    if (mutant->recursion_count_ > 0 && mutant->owner_host_thread_ == self) {
      mutant->abandon_locked();
    }
  }
}

}  // namespace xenon::kernel
