#pragma once

namespace xenon::launcher {

// -----------------------------------------------------------------------------
// Launcher front-end test mode
// -----------------------------------------------------------------------------
//
// Keep this OFF for normal builds. With test mode disabled the launcher starts
// with an empty library/module registry and does not pretend that any game,
// module, DLC, update, or imported content is present.
//
// Set this to true while developing the launcher UI. Test mode injects a fully
// fictional demo module/game so every card, menu, status row, dropdown and
// button can be exercised before the real module/content backends exist.
//
// IMPORTANT: Test data is never written to the user's real launcher state.
// -----------------------------------------------------------------------------
inline constexpr bool kTestMode = true;

}  // namespace xenon::launcher
