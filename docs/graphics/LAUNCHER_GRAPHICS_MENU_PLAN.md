# Launcher Next: graphics enhancement menus and session controls

Planning specification, 11 November 2026. This document proposes product behaviour and contracts; it does not add a working menu or renderer feature. Read with [the renderer plan](UPSCALING_FRAME_GENERATION_PLAN.md) and [the every-game support programme](UNIVERSAL_RENDERING_SUPPORT_PLAN.md).

## Direction

Make advanced rendering understandable in a few choices: desired picture quality, output size, reconstruction method and frame generation. Show what is actually running and why. Provide detailed controls when requested, while retaining Xenon's themes, library organisation, profiles and controller navigation.

Use the existing Library game-settings entry and Settings hierarchy. The first product surface is **Library > game > Settings > Graphics**, shared across every game. Add **Settings > Graphics** for global defaults. A **Graphics** shortcut beside Play opens that same per-game surface rather than introducing a separate settings store.

## Verified local foundations

| Existing location | Reuse and extension |
| --- | --- |
| `launcher-next/DESIGN_DIRECTION.md` | Existing desktop shell, game details, optional carousel, Xenon branding and accessible themes |
| `web/ui/GameSettings.tsx` | Current Graphics/Audio/Controls/Game Options modal; inheritance controls and controller tab cycling |
| `web/gameSettings.ts`, `native/core/game_settings.cpp` | Runtime definitions, validation and effective global/profile/game values |
| `contracts/settings.json`, `contracts/profile-runtime.json` | Global and profile/game runtime settings catalogue |
| `contracts/launcher.ts`, `web/backend.ts` | Typed snapshots/actions and explicit browser-preview capabilities |
| `web/ui/SessionPanel.tsx`, `native/adapters/runtime_adapter.cpp` | Supervised session and authoritative runtime status |
| `runtime_host/src/launch_config.*`, `status_writer.*` | Launch configuration, atomic status, capability report and stop signal |
| `web/policies.ts` | Active-session and job locks; live editing needs an explicit supported path |
| `contracts/localization.json`, `web/localization.ts`, `web/controller.ts` | Translatable text, stable identifiers and existing input/focus behaviour |

Current production handoff is launch-time configuration plus status files and stop requests; a general live graphics command channel is not established by that. Add negotiated commands before enabling live controls. The existence of a setting definition is not proof that the running runtime applies it.

## What to adapt from COGBox

Source review at commit `cb4cb050066648134384e4d48f8ec8788459bb9f`; the COGBox application was not launched or visually tested during this planning pass.

| Observed source pattern | Xenon adaptation |
| --- | --- |
| [front.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/cogbox/src/front.cpp): focused cover carousel with Play/Settings | Keep Xenon's existing optional carousel and expose Graphics consistently in all library views |
| [page.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/cogbox/src/page.cpp): per-game tabs generated from options, summary and Save/Play actions | Share declarative setting definitions and a persistent launch summary |
| [gears_settings.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/cogbox/src/gears_settings.cpp): availability descriptions, separate AA/scene size and native-AA explanations | Explain image reconstruction versus actual scene scaling; derive support from typed runtime capabilities |
| [ingame_menu.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/ingame_menu.cpp): shared option metadata, live/restart state, controller legends and input capture | Share setting semantics across launcher and a future runtime menu, including input-release behaviour |

Xenon's proposed additions are exact-revision support status, independent upscaler/FG selection, inheritance provenance, runtime acknowledgements, local comparison/measurement and recoverable changes. Keep these as Xenon designs rather than claims about functionality missing from another project.

Avoid importing Gears-specific labels, executable string searches, hardcoded GPU-name tests or fixed scale constraints into a generic launcher. Runtime capability queries and SDK-recommended extents should be authoritative. Retain appropriate notices and credit for any eventual code reuse.

## Information architecture

Keep Audio, Controls and Game Options alongside Graphics. Within Graphics use sections or compact subnavigation rather than adding many new top-level tabs:

| Section | Main controls |
| --- | --- |
| Overview | Preset, support state, effective settings summary, reset to inherited values |
| Display | Adapter/backend, output size, window mode, aspect handling, VSync/VRR status where measurable |
| Image quality | Reconstruction provider, native AA/quality mode, actual scene size, sharpening, dynamic-resolution bounds when supported |
| Smoothness | FG provider/multiplier, presentation target, latency mode and real-versus-displayed rate explanation |
| Compatibility | Current revision's feature coverage and known limitations, recipe update status, recovery action |
| Advanced | Provider implementation/model options, memory budget, debug overlays and developer-only input inspection |

Small windows can collapse sections vertically. Do not force large artwork beside a long settings form. Use an optional compact game header and existing semantic CSS tokens. The same controls must work in Dark, Carbon, Industrial, Light and other supported themes.

## Per-game menu wireframe

Illustrative layout and hypothetical values only; this is not a claim that any title is already validated.

```text
Example Game                                      Graphics settings
Profile: Player 1                  [Use global/profile settings]

Overview | Display | Image quality | Smoothness | Compatibility

Preset                      [ Balanced                      v ]
Image reconstruction        [ Automatic                     v ]
Quality                     [ Quality                       v ]
Frame generation            [ Off                           v ]

Your selection
  Output                    2560 x 1440
  3D scene                  1707 x 960 (runtime recommendation)
  HUD                       Output resolution
  Changes                   Apply on next launch

Support for this version
  Temporal upscaling        Validated on Direct3D 12
  Frame generation          Experimental: moving particles
  [View details]

[Restore inherited]                  [Cancel] [Save] [Save and Play]
```

When stopped, call the summary a selection or prediction, not the active renderer. Output and recommended internal dimensions are informational until the runtime confirms them. If exact extents cannot be known before launch, show "Determined at launch" rather than inventing a ratio. Show the literal rendering resolution when the requested mode falls back to native AA.

When a game is running, replace the prediction with requested and active state, applied revision and live/restart labels. The menu opens read-only if the runtime offers no supported live controls; save next-launch settings through a distinct action if permitted by the session policy.

## Presets and automatic selection

Proposed user-facing presets:

| Preset | Intent |
| --- | --- |
| Original presentation | Baseline renderer and output behaviour with temporal enhancement disabled |
| Image quality | Prefer native AA or a validated high-quality reconstruction setting within the user's output choice |
| Balanced | Prefer a validated Quality setting with conservative sharpening; FG remains an explicit choice |
| Performance | Prefer lower scene cost only within validated bounds and show the resulting size |
| Custom | Shown when individual settings diverge from a preset |

Preset application shows the concrete changed values before Save. Automatic provider selection honours the title's validation, hardware/backend capabilities, pinned package versions, quality target and optional local measurement. It must explain its selected provider. Do not equate "NVIDIA card" with "DLSS FG available" or assume Arc is required for every XeSS feature.

An optional **Optimise for this PC** action runs a bounded, cancellable local check when a suitable scene is available. Distinguish an actual game benchmark from a synthetic hardware estimate. Keep the current settings if measurements are unstable or incomplete. Never generate a universal "optimal" badge from device name alone.

FG initially defaults off. After explicit user enablement, an Automatic FG provider may choose among compatible validated options. A CPU-bound game may benefit in displayed motion, while input/simulation responsiveness remains tied to real frames. Show a concise explanation when base performance is too low for the selected provider's intended quality. Use provider-specific guidance rather than one universal hardcoded FPS threshold.

## Controls and semantics

| Proposed stable key | Meaning | Normal surface |
| --- | --- | --- |
| `graphics/preset` | User intent expanded into concrete selections | Overview |
| `graphics/upscaler` | Off, spatial, Automatic, FSR, DLSS or XeSS; actual options capability-derived | Image quality |
| `graphics/upscaleQuality` | Native AA, Quality, Balanced, Performance and other supported modes | Image quality |
| `graphics/outputSize` | Auto/display match or validated explicit extent | Display |
| `graphics/sharpening` | Provider-adapted optional strength with a neutral/default position | Image quality |
| `graphics/frameGeneration` | Off, Automatic or a named FG provider independent of SR | Smoothness |
| `graphics/frameMultiplier` | Supported 2x or additional queried modes | Smoothness |
| `graphics/presentationTarget` | Requested displayed cadence with a clear FG explanation | Smoothness |
| `graphics/latencyMode` | SDK-compatible latency policy | Smoothness |
| `graphics/dynamicResolution` | Opt-in control with bounds and target real render time | Advanced initially |
| `graphics/overlay` | Off, compact performance or detailed session metrics | Smoothness |

These names are proposed additions, not existing supported settings. Resolve potential duplication with current renderer/cache definitions before implementation. Provider-specific model presets and library versions belong in Advanced; defaults follow the pinned validated package rather than arbitrary DLL swapping. Initial release should use managed packages with rollback; expert custom libraries are a separate scoped design.

Keep any game-engine frame-rate unlock in module Game Options with its own validated patch. The graphics menu must not imply that raising a presentation target changes game physics safely. Distinguish game simulation limits, real rendering cadence and displayed frame rate in explanatory text.

## Inheritance and persistence

Preserve the current order: built-in defaults, global settings, enabled isolated-profile overrides, game runtime overrides. Expand a selected preset at its own layer; a lower-priority preset must never erase a higher-priority explicit field. The session may carry a temporary override above saved game values. Runtime constraints resolve an active configuration from that request without overwriting its source.

Display the source of each value: Global, Profile, This game or This session. Provide per-field "Use inherited" and section-level "Restore inherited". Do not add per-profile-per-game storage implicitly; that would change current semantics and needs a separate migration decision. Saving defaults affects future launches and does not mutate the active session automatically.

Keep support recipes distinct from user preferences. Updating a recipe can change availability/recommendations while preserving the chosen intent. Re-evaluate after module, runtime, driver, adapter or SDK changes. Mark previously measured recommendations as needing recheck. Portable settings exports should preserve intent and avoid binding another PC to a local adapter identifier.

## Availability and failure copy

Unavailable options remain discoverable with a short explanation and a next action when one exists. Keyboard/controller users must be able to reach the explanation; disabled native controls alone are insufficient. Examples:

- "DLSS is unavailable on the selected graphics adapter. Automatic can use FSR or XeSS."
- "Frame generation is awaiting validation for this game version. Upscaling is available."
- "The running Xenon host cannot change this setting live. Saved for the next launch."
- "FSR native AA is active. The 3D scene is rendering at output resolution."
- "Frame generation is temporarily off while this movie plays. Your selection is saved."

Use typed reason codes internally: missing runtime, unsupported backend/device/driver, revision mismatch, unvalidated motion, unsupported view/composition, missing resource, insufficient memory, dispatch failure or restart required. Translate the user-facing explanation; do not parse English log messages to decide behaviour. Expected menu/video suspension should not produce repeated error toasts.

## Launch and live control contract

Add a versioned rendering capability summary to the existing capability-report path, with a compact status reference. Include selected adapter/backend, matching image/recipe identity, supported options, validation scope, runtime package versions and live-change classes. Cache it by environment fingerprint. Never initialise or load render providers inside the launcher's CEF renderer just to populate a dropdown.

A preflight capability query must be an explicit host operation that does not launch guest code or claim gameplay validation. Choose and version this protocol before implementation; old hosts remain usable with existing launch-time settings and clearly limited UI.

For live changes, introduce a negotiated session-scoped request/ack protocol alongside current status reporting. A bounded atomically published command inbox/outbox is a reasonable first transport consistent with existing files; use a named pipe/local socket instead only after recording the lifecycle and portability tradeoff. Do not treat `stop.signal` or free-form log lines as a settings channel.

Each request carries session ID, request ID, expected settings revision and an allowlisted typed patch. Each result carries accepted/rejected fields, new revision, pending/applied state, actual configuration and reason codes. The host validates again and schedules changes on its graphics owner thread at a safe boundary. Duplicate requests are idempotent; stale sessions and revisions cannot overwrite newer state. Keep queue size, file size, retries and acknowledgements bounded.

Classify changes by actual implementation capability:

| Change class | Product behaviour |
| --- | --- |
| Immediate | Apply at a safe point and acknowledge the active result |
| History reset | Apply on a real-frame boundary, reset relevant histories and acknowledge |
| Presentation recreation | Drain/recreate through the single presentation owner, keep a visible pending state |
| Restart only | Save separately and show "Next launch"; offer deliberate restart without pretending progress is preserved |

Do not assume provider swaps, backend changes or SDK loads can be live. Initial implementation can support a small subset and truthfully label the rest. Keep existing active-session locks for unrelated game/module edits.

For disruptive display changes, retain the last working configuration and offer a timed Keep/Revert flow after successful application. The runtime owns timeout/revert if the launcher disconnects. A device loss can require restart rather than rollback. If the launcher exits while changes are pending, reattachment reads authoritative settings revision and results.

## Session panel and optional runtime menu

Add a compact graphics status area to the existing Session Panel:

```text
Graphics: DLSS Quality | 1707x960 -> 2560x1440
Frame generation: FSR 2x
Rendered: 58 FPS   Displayed: 112 FPS
HUD: output resolution
[Adjust graphics] [Compare] [Report a rendering problem]
```

All values above are hypothetical. Real displayed-rate counters need provider/presentation feedback; never calculate an observed rate by multiplying real FPS by a selected multiplier. Unmeasured latency is omitted or explicitly labelled estimated. Throttle UI updates so telemetry does not disturb rendering.

A later in-game quick menu can reuse the setting definitions, effective state and command validation. It should be a native runtime overlay rendered at output resolution after reconstruction and through the FG provider's supported UI composition path. Launcher's Chromium surface must remain separate from the game pipeline. Select an overlay technology through an architecture decision; no new dependency is prescribed here.

Use a remappable shortcut and controller chord that respects platform-reserved buttons. Capture input while open and release the closing action before returning control to the game. Preserve prior cursor/capture state. Do not imply that opening an overlay pauses multiplayer or guest execution. The overlay remains usable during an otherwise unresponsive guest where the host is still responsive, and exposes stop/recovery through existing session controls.

## Comparison, reporting and author tools

Offer **Compare picture quality** as a later differentiator: local aligned captures, zoom, temporal sequence playback and the full settings fingerprint. A still-image wipe alone cannot reveal ghosting or judder. Switching providers sequentially is preferable to rendering two full pipelines every frame on normal hardware. Use matching repeatable sequences where possible and disclose when scenes cannot be aligned.

**Report a rendering problem** prepares a reviewable local bundle with runtime/recipe/SDK versions, effective settings, recent typed failure events and optional screenshots. Reuse current support-bundle infrastructure. Redact local identity/paths where appropriate, include captures only by user selection, and never submit to a third party without an explicit user action.

Developer mode adds an author workflow for profile candidates, scene/depth/motion views, pass markers, validation run selection and evidence export. Keep camera constant indices and shader hashes out of ordinary game settings. Show developer fixture data as fixture data, including in screenshots and exported results.

## Accessibility and acceptance

Keep all flows usable by mouse, keyboard and controller. Provide visible focus, predictable Back/Escape, LB/RB section navigation, persistent focus after dynamic updates and no focus jumps when a capability result arrives. Use text plus icons for support states; colour alone is insufficient. Test high contrast, reduced motion, increased text size, narrow windows and screen-reader labels.

Add all product-owned strings to the existing localisation catalogue, with English fallback and the currently supported French, German and Spanish translations. Keep keys, enum values, provider IDs, filenames and user text untranslated. Device-specific button legends should follow the current input method without oscillating on incidental mouse motion.

Acceptance requires native persistence and inheritance checks, capability mismatch handling, old-host fallback, authoritative live acknowledgements, reattachment, safe-mode behaviour, controller-only setup and meaningful failed-change recovery. Browser previews may exercise explicitly labelled fixtures; they must not acquire native capabilities from stored state. Run tests appropriate to implemented slices, then verify real host behaviour before describing any option as working.

Initial UI delivery is complete when a user can open the same menu for every game, understand which features are available for their exact version, save a valid selection, launch through the existing host path and see the actual active outcome. Advanced comparison, adaptive resolution and the runtime overlay follow their separate backend gates.
