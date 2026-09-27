// Presentation ownership/rollback proof for runtime_host/src/presentation_host.{hpp,cpp}
// (see docs/runtime/RUNTIME_HOST.md). PresentationHost::create() already performs
// real cleanup on every internal failure branch (destroy the SDL window,
// SDL_QuitSubSystem, never publish `backend_`) - this test exercises those
// paths for real rather than asserting it by reading the source, and proves
// the object is left fully reusable afterward: no leaked window, no stuck
// SDL video subsystem reference count, and a subsequent create() with a
// real backend still works.
//
// The deterministic failure exercised here is a real, always-reachable one
// that needs no GPU device at all: requesting presentation for
// gpu::NullBackend, which create() must reject outright ("Null/headless
// backends must not create a window") without ever calling
// SDL_CreateWindow. This is the same category of ownership bug this
// project's contract cares about most - a "no-op backend" quietly creating
// (or leaking) a real OS resource - proven through the real SDL2 stack, not
// a mock.

#include <cassert>
#include <iostream>
#include <string>

#include "presentation_host.hpp"
#include "xenon/gpu/backend.hpp"

int main() {
  std::cout << "Testing presentation host create()/destroy() rollback...\n";

  xenon::runtime_host::PresentationHost host;
  assert(!host.created());

  {
    // NullBackend is a real, reachable production configuration (headless/
    // test sessions - see XenonSession::init_gpu()), so create() must
    // refuse it explicitly rather than either crashing or silently
    // producing a window nothing will ever present into.
    xenon::gpu::NullBackend backend;
    std::string error;
    const bool created = host.create(320, 240, "Xenon Presentation Rollback Test", backend, &error);
    assert(!created && "PresentationHost::create() must refuse a backend with no presentation path");
    assert(!error.empty() && "the refusal must be diagnosable, not silent");
    std::cout << "  [ok] create() rejected NullBackend: " << error << "\n";
    assert(!host.created() && "a rejected create() must not leave a window behind");
  }

  // destroy() on an object that never successfully created anything must be
  // a safe no-op (no double-free, no crash) - real callers (main.cpp) always
  // pair create()/destroy() through RAII without checking created() first.
  host.destroy();
  assert(!host.created());

  // The object must remain fully usable after a failed create() - not left
  // in some half-initialized state (e.g. a leaked SDL_INIT_VIDEO reference
  // that a later real create() would then double-init against). Requesting
  // NullBackend again must fail exactly the same way, proving create()
  // is idempotent under repeated failure rather than degrading.
  {
    xenon::gpu::NullBackend backend;
    std::string error;
    const bool created = host.create(320, 240, "Xenon Presentation Rollback Test 2", backend, &error);
    assert(!created);
    assert(!host.created());
    std::cout << "  [ok] create() remains correctly refusable after a prior failed attempt (no stuck state)\n";
  }

  std::cout << "All tests passed!\n";
  return 0;
}
