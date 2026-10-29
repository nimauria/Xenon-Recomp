#include "xenon/kernel/object_description.hpp"

#include <cstdio>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/timer.hpp"
#include "xenon/kernel/wait.hpp"

namespace xenon::kernel {

std::string describe_object_state(const KernelObject& object, std::int64_t now_ns) {
  std::string text;
  switch (object.type()) {
    case ObjectType::Event: {
      const auto& event = static_cast<const KernelEvent&>(object);
      text = event.manual_reset() ? "event (manual-reset) " : "event (auto-reset) ";
      text += event.signaled() ? "signalled" : "not signalled";
      break;
    }
    case ObjectType::Semaphore: {
      const auto& semaphore = static_cast<const KernelSemaphore&>(object);
      text = "semaphore count " + std::to_string(semaphore.count()) + " of " +
             std::to_string(semaphore.maximum_count());
      break;
    }
    case ObjectType::Mutant: {
      const auto& mutant = static_cast<const KernelMutant&>(object);
      text = mutant.is_owned() ? "mutant owned by t" + std::to_string(mutant.owner_thread_id()) +
                                     " (recursion " + std::to_string(mutant.recursion_count()) + ")"
                               : std::string("mutant free");
      break;
    }
    case ObjectType::Timer:
      text = static_cast<const KernelTimer&>(object).is_signaled() ? "timer signalled"
                                                                    : "timer not signalled";
      break;
    case ObjectType::Thread: {
      const auto& thread = static_cast<const KernelThread&>(object);
      text = "thread t" + std::to_string(thread.thread_id()) +
             (thread.is_terminated() ? " (terminated)" : " (running)");
      break;
    }
    default:
      text = "object type " + std::to_string(static_cast<unsigned>(object.type()));
      break;
  }
  // Registered waits include a thread's own registration while it waits
  // for anything; only objects' lists are meaningful here.
  if (object.type() != ObjectType::Thread) {
    if (const auto waiters = waiter_count(const_cast<KernelObject&>(object)))
      text += ", " + std::to_string(waiters) + (waiters == 1 ? " waiter" : " waiters");
  }
  const auto note = object.last_signal();
  if (!note.count) return text + "; never signalled through a recorded export";
  char line[160];
  std::snprintf(line, sizeof(line), "; last signalled by t%u (%s from 0x%08X) %lld ms ago, %llu signals",
                note.guest_thread_id, note.source ? note.source : "?", note.guest_caller,
                static_cast<long long>((now_ns - note.host_time_ns) / 1'000'000),
                static_cast<unsigned long long>(note.count));
  return text + line;
}

}  // namespace xenon::kernel
