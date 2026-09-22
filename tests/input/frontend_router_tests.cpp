// Regression coverage for xenon::input::FrontendInputRouter (used by the
// launcher's InputFeature to turn raw gamepad button/analog state into
// semantic navigation actions - see launcher/src/frontend_backend/input/
// input_feature.cpp's configureFrontendRouter()/frontendActions()). This is
// the part of the launcher's controller-navigation pipeline that is real
// dependency-free C++ and testable in isolation, independent of a live
// controller or the Qt/QML layer above it.

#include <cassert>
#include <cstdint>
#include <iostream>

#include "xenon/input/action_router.hpp"

using namespace xenon::input;

namespace {

// Mirrors GamepadButton bit values used as "codes" for FrontendInputSource::Gamepad
// (see include/xenon/input/types.hpp) - kept local so this test does not need
// to pull in the whole gamepad state header for four constants.
constexpr std::int32_t kDpadUp = 0x0001;
constexpr std::int32_t kA = 0x1000;
constexpr std::int32_t kY = 0x8000;
constexpr std::int32_t kX = 0x4000;

}  // namespace

int main() {
  // Test 1: an unbound (source, code) pair produces no event - proves
  // dispatch() never fabricates an action for a code nothing was ever
  // bound to, matching the router's own "shared action router... does not
  // know about QML or window controls" contract.
  {
    FrontendInputRouter router;
    router.dispatch(FrontendInputSource::Gamepad, kDpadUp, true, false);
    FrontendInputEvent event{};
    assert(!router.poll(event) && "dispatch() must not synthesize an event for an unbound code");
  }
  std::cout << "  [PASS] dispatch() on an unbound code produces no event\n";

  // Test 2: a bound code round-trips through dispatch()/poll() with the
  // right action, source, pressed and repeated flags preserved - this is
  // the exact mechanism the launcher's D-pad/stick repeat state machine
  // depends on (a real repeat is dispatched with repeated=true).
  {
    FrontendInputRouter router;
    router.bind({FrontendInputSource::Gamepad, kDpadUp, FrontendInputAction::Up});
    router.dispatch(FrontendInputSource::Gamepad, kDpadUp, true, false);
    router.dispatch(FrontendInputSource::Gamepad, kDpadUp, true, true);  // held-repeat tick

    FrontendInputEvent first{};
    assert(router.poll(first));
    assert(first.action == FrontendInputAction::Up);
    assert(first.source == FrontendInputSource::Gamepad);
    assert(first.pressed && !first.repeated);

    FrontendInputEvent second{};
    assert(router.poll(second));
    assert(second.action == FrontendInputAction::Up);
    assert(second.pressed && second.repeated);

    FrontendInputEvent none{};
    assert(!router.poll(none) && "queue must be empty after both events are polled");
  }
  std::cout << "  [PASS] a bound code round-trips through dispatch()/poll(), repeated flag preserved\n";

  // Test 3: the launcher's actual button map (Part 5/17 of the Gracemeria
  // launcher navigation pass) - Y opens a focused item's context menu
  // (Context), X is the reserved per-page secondary action (Secondary) -
  // resolves to distinct actions, not the stale Search/Menu mapping this
  // pass replaced.
  {
    FrontendInputRouter router;
    router.bind({FrontendInputSource::Gamepad, kY, FrontendInputAction::Context});
    router.bind({FrontendInputSource::Gamepad, kX, FrontendInputAction::Secondary});
    router.dispatch(FrontendInputSource::Gamepad, kY, true, false);
    router.dispatch(FrontendInputSource::Gamepad, kX, true, false);

    FrontendInputEvent context_event{};
    assert(router.poll(context_event));
    assert(context_event.action == FrontendInputAction::Context);

    FrontendInputEvent secondary_event{};
    assert(router.poll(secondary_event));
    assert(secondary_event.action == FrontendInputAction::Secondary);
  }
  std::cout << "  [PASS] Y binds to Context and X binds to Secondary, not the old Search/Menu mapping\n";

  // Test 4: push() delivers a fully-formed event directly, bypassing the
  // bind()/dispatch() code-lookup path entirely - this is how the launcher
  // reports analog trigger presses (ScrollUp/ScrollDown), which have no
  // discrete GamepadButton code to bind.
  {
    FrontendInputRouter router;
    router.push({FrontendInputSource::Gamepad, FrontendInputAction::ScrollUp, 200, 12345, true, false});
    FrontendInputEvent event{};
    assert(router.poll(event));
    assert(event.action == FrontendInputAction::ScrollUp);
    assert(event.value == 200);
    assert(event.timestamp == 12345);
    assert(event.pressed && !event.repeated);
  }
  std::cout << "  [PASS] push() delivers analog-sourced events (e.g. trigger ScrollUp/ScrollDown) directly\n";

  // Test 5: clear() removes all bindings - the launcher calls this before
  // re-registering its map (configureFrontendRouter()), and a stale
  // binding surviving reconfiguration would silently resurrect a mapping
  // that should no longer exist.
  {
    FrontendInputRouter router;
    router.bind({FrontendInputSource::Gamepad, kDpadUp, FrontendInputAction::Up});
    router.clear();
    router.dispatch(FrontendInputSource::Gamepad, kDpadUp, true, false);
    FrontendInputEvent event{};
    assert(!router.poll(event) && "clear() must remove previously-registered bindings");
  }
  std::cout << "  [PASS] clear() removes all bindings\n";

  std::cout << "All FrontendInputRouter tests passed!\n";
  return 0;
}
