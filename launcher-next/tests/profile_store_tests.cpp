#include "core/profile_store.hpp"
#include <iostream>
#include <stdexcept>

using xenon::launcher_next::ProfileStore;
void require(bool condition) {
  if (!condition)
    throw std::runtime_error("Assertion failed");
}
template <class F> void rejects(F action) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error("Expected invalid_argument");
}
int main() {
  try {
    rejects([] { ProfileStore empty({}, "missing"); });
    ProfileStore store({{"a", "Player", ""}}, "a");
    rejects([&] { store.remove("a"); });
    rejects([&] { store.activate("missing"); });
    rejects([&] { store.create({"a", "Duplicate ID", ""}); });
    rejects([&] { store.create({"b", "   ", ""}); });
    require(store.profiles().size() == 1 && store.active_id() == "a");
    store.create({"b", "Second player", ""});
    store.activate("b");
    store.remove("a");
    require(store.profiles().size() == 1 && store.active_id() == "b");
    rejects([&] { store.remove("missing"); });
    std::cout << "Profile identity and active-profile invariants passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
