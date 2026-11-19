# DLSS, FSR, XeSS and frame generation: implementation plan

Research date: 11 November 2026. Planning only; no renderer implementation or runtime validation was performed for this document.

## Expanded scope and reading order

The programme now covers the whole game catalogue and Launcher Next. Every game added to Xenon must enter the rendering support workflow and receive consistent settings, capability reporting and a working fallback. Validated temporal upscaling and frame generation for every supported title is the long-term coverage target; identical SDK availability on every GPU, OS and game rendering mode cannot be assumed.

| Specification | Purpose |
| --- | --- |
| This document | Shared renderer architecture, SDK integration, staged delivery and engineering backlog |
| [Universal rendering support](UNIVERSAL_RENDERING_SUPPORT_PLAN.md) | Mandatory game onboarding, analysis/recipes, dynamic motion, coverage matrix, fallback and quality lab |
| [Launcher graphics menus](LAUNCHER_GRAPHICS_MENU_PLAN.md) | COGBox menu audit, global/per-game settings, wireframes, inheritance, live commands and session UX |

All three are planning documents, now included in the published documentation index. Proposed keys, schema names, controls and targets do not describe implemented functionality.

The ambition is to make Xenon an unusually complete platform for enhancing recompiled games: reusable rendering intelligence, sharp supported HUDs, independently selectable reconstruction/interpolation, predictable configuration and reproducible quality evidence. Leadership must be earned through catalogue coverage, image quality, latency, reliability and measured comparisons. A successful SDK demonstration alone does not establish it.

## Recommended approach

Extend Xenon's existing temporal groundwork into a shared GPU input pipeline, prove FSR at native resolution on D3D12, then add real upscaling, XeSS and DLSS. Add frame generation after motion quality and HUD composition are validated. Bring the same contracts to Vulkan in a separate milestone. In advance of temporal readiness, provide a common spatial presentation path and rendering support manifest for every game intake path.

Keep upscaling and frame generation independently selectable. For example, DLSS Super Resolution plus FSR frame generation should be a supported combination when both providers pass capability checks. Exactly one provider owns frame-generation presentation at a time.

The main engineering risk is reconstructing the inputs from Xbox 360 rendering. Adding SDK calls alone will not solve this. Camera reprojection does not describe independently moving aircraft, characters, skinned meshes, particles or scrolling surfaces. A generic implementation needs explicit quality levels and per-revision validation.

## Verified starting point

Local snapshot: HEAD `bb640be`, with substantial pre-existing uncommitted graphics and other work. Implementation must recheck that state and preserve it.

| Area | Current local evidence | Remaining work |
| --- | --- | --- |
| Temporal arithmetic | `include/xenon/gpu/temporal.hpp`, `src/graphics/xenos/temporal.cpp` | GPU execution and convention validation |
| Revision profiles | `temporal_profile.hpp`, `temporal_profile.cpp`; session loads the effective executable hash's `rendering.json` | Measure real title profiles; extend only for demonstrated needs |
| Pass discovery and classification | `temporal_frame.hpp`, `temporal_frame.cpp`; scene, resolve, post-process, HUD and camera cuts | Feed classifications and current camera state into backend execution |
| Live frontend | `GraphicsSystem::execute_ir()` calls `temporal_.observe(command)` before `backend.consume(command)` | The returned pass classification is currently discarded; this is an integration point |
| Presentation | `PresentationFrame` describes a guest texture and visible extent; both native backends have scanout and swapchain code | Introduce host temporal resources, immutable frame identity and provider-controlled presentation |
| Configuration | `temporalProbeFrames` exists in runtime-host launch configuration | User settings, capability reporting and actual active feature state |
| Existing tests | Temporal maths/profile/classifier and frontend frame-boundary tests | GPU inputs, provider lifecycle, presentation and real-title image validation |

The [temporal rendering document](TEMPORAL_RENDERING.md) describes implemented observation and maths; it does not change rendered output. Classification alone does not establish correct object motion, multiple views or HUD composition. This plan supplies the implementation and validation gates.

Current title progress (owner report, 11 November 2026): AC6 loads through the main menu into the first cutscene with fully working audio. Cutscene frame rate/pacing is actively being fixed; gameplay remains unconfirmed. That progress does not qualify temporal inputs or enhancement quality.

## COGBox reference audit

The current source is [Shagg88/COGBox on GitLab](https://gitlab.com/shagg88/cogbox). Source was inspected at commit `cb4cb050066648134384e4d48f8ec8788459bb9f`. Links below pin that snapshot; recheck changes before porting.

| Source under `Gears2-Native/src/layer0_xenos/renderer/` | Useful reference |
| --- | --- |
| [upscaler.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/upscaler.cpp) | SDK-independent factory, jitter and camera maths |
| [fsr_upscaler.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/fsr_upscaler.cpp) | FidelityFX adapter and version enumeration; separate Vulkan adapter |
| [xess_upscaler.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/xess_upscaler.cpp) | Optional runtime loading and explicit input conventions |
| [dlss_upscaler.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/dlss_upscaler.cpp) | Direct NGX Super Resolution/DLAA integration |
| [frame_generation.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/frame_generation.cpp) | FSR swapchain-only test mode, prepare-before-present and teardown order |
| [dlss_framegen.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/dlss_framegen.cpp) | Streamline manual hooks, resource tagging, Reflex and feature queries |
| [xess_framegen.cpp](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/Gears2-Native/src/layer0_xenos/renderer/xess_framegen.cpp) | XeFG proxy swapchain, XeLL markers and matching present IDs |

Use these for adapter and lifecycle patterns. Their rexglue hooks and Gears-specific integration must be translated to Xenon's interfaces. Merely finding these files does not establish equivalent visual quality or compatibility in Xenon. Some introductory comments describe intermediate development states, so check call sites and current behaviour during implementation.

COGBox's own code is MIT; its [README and licence notes](https://gitlab.com/shagg88/cogbox/-/blob/cb4cb050066648134384e4d48f8ec8788459bb9f/README.md) distinguish vendor components. If code is reused, retain notices and add the requested “COGBox by Shagg” credit and project link. Vendor SDK and binary terms must be recorded separately.

## Shared architecture

Proposed interfaces, with names subject to repository conventions:

- `TemporalFrameInputs`: GPU handles for scene colour, depth, motion, optional exposure/reactive/transparency masks, and output; render/output extents, colour space, depth convention, jitter, unjittered camera matrices, elapsed render time, reset reason and view/frame IDs.
- `UpscalerProvider`: query capabilities and preferred input size, create/reconfigure, dispatch and release. Native AA, Quality, Balanced and Performance are semantic modes; obtain sizes from each SDK where supported.
- `FrameGenerationProvider`: query support, create presentation integration, submit/tag a real frame, configure interpolation, drain/recreate and shut down. Keep swapchain ownership separate from upscaler dispatch.
- `TemporalFeatureState`: requested and active providers, selected SDK implementation/version, fallback reason and input quality. Report unsupported profiles separately from unsupported GPUs.

Use one canonical motion convention internally: current-to-previous displacement in render pixels, positive X right/Y down, with jitter excluded. Providers adapt units, sign and coordinate conventions. Record depth inversion, infinite far plane, exposure and colour encoding explicitly. Preserve depth precision and handle MSAA deliberately.

Maintain host-only enhanced render targets. Guest addresses, pitches, EDRAM ownership, resolve semantics and CPU-visible bytes retain their guest meaning. Scaled host resources need explicit mappings through texture aliases and resolves; a larger allocation cannot silently change guest memory interpretation.

Pass annotations must reach the backend while the relevant draw or resolve executes. The completed `end_frame()` report is useful for history and diagnostics, but arrives too late to schedule that frame's earlier upscaler dispatch. Avoid mutating guest registers to apply host jitter.

Resource states and ownership remain explicit. Retain the exact colour/depth/motion set until its SDK work and presents complete, including work on SDK-owned queues. Restore graphics bindings after SDK execution. Never read temporal resources back to the CPU in the normal feature path.

## Delivery milestones

### 0. Capture a trustworthy baseline

Confirm current rendering correctness with temporal features off. Use AC6 as the first integration title once the relevant scenes have correct baseline images and timing. Begin with the reached menu/cutscene sequence; add gameplay, camera motion, independently moving geometry, transparencies and a camera cut as those become available and validated. Establish draw/resolve/depth correspondence and GPU/CPU timings. The current cutscene pacing issue remains a baseline blocker for affected scenes.

Generic plumbing and synthetic scenes can proceed while title rendering is being fixed. Real-title support stays gated on valid images. Title profiles and AC6-specific data belong in Project-Gracemeria; reusable rendering and SDK adapters belong in Xenon.

Exit: repeatable baseline captures and a measured profile for an exact executable revision.

### 1. Add frame contracts, capabilities and optional dependencies

Add the shared interfaces and per-command annotation path. Separate guest frame ID, real rendered frame ID, SDK frame token and displayed frame counters. Add independent upscaler/FG configuration with both defaulting off.

Extend `tools/deps/manifest.json`, `tools/deps/bootstrap.py`, `cmake/Dependencies.cmake` and graphics CMake targets as appropriate. Pin SDK revisions and checksums, package required notices, and keep vendor binaries out of source control. Preserve builds without any vendor SDK. Record redistribution and application/project identity requirements for the selected packages.

Recommended NVIDIA route: use Streamline for SR and FG so device registration, tokens and latency integration share one owner. COGBox's direct NGX SR path is an alternative to evaluate in a small SDK sample before locking this decision. Streamline bootstrap must precede graphics API creation when required; loading it for the first time at `present()` is too late. See the [official Streamline guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md).

Exit: features-off rendering is unchanged; missing SDKs and unavailable devices produce clear inactive states without launch failure.

### 2. Produce and inspect temporal GPU inputs

Implement D3D12 scene colour/depth capture at verified boundaries, scene jitter and camera-motion compute. Audit screen-space passes that sample jittered depth; leaving their geometry unjittered does not automatically align their inputs. Add debug views for colour, depth, motion, masks, jitter and reset reasons.

Start with camera motion as an explicitly experimental input tier. Next establish reliable dynamic-object correspondence: consume real guest motion data where present, or instrument translated draws with previous transforms/vertex inputs for known draw classes. Do not match objects only by shader hash or draw order. Skinned geometry needs previous deformation state. Unknown motion, particles and changing topology require disocclusion/reactive handling or conservative history rejection.

Exit: static pixels under camera movement reproject correctly; moving-object errors are visible and classified. Declare which motion classes are supported before enabling FG for a title.

### 3. Integrate FSR native AA on D3D12

Dispatch at equal render/output size first. Establish reset handling, exposure and HDR/LDR conventions without also changing target dimensions. Introduce provider failure recovery and context recreation.

Use FSR 3.1-class analytical upscaling as the compatibility baseline. Evaluate newer ML implementations through the pinned AMD package's actual capability/version query; expose the implementation actually selected. Do not assume every AMD feature or binary shares the older open-source licence. AMD documents the [separation of upscaling and frame generation](https://gpuopen.com/learn/amd_fsr_3_1_release/) and its [current SDK](https://gpuopen.com/amd-fsr-sdk/).

Exit: stable stationary detail, correct motion direction and clean history resets, with no jitter left enabled following fallback.

### 4. Add real scene scaling and native HUD composition

Separate guest dimensions, internal scene dimensions and output dimensions. Scale the scene's host targets, viewport/scissor and dependent sampling coherently. Track scaled aliases and resolve consumers without changing guest bytes. Preserve offscreen/shadow targets unless their scaling is explicitly supported.

Treat scene resolve and first HUD draw as candidate boundaries, not universal insertion points. Some games have later scene draws, multiple resolves or post-processing that assumes guest dimensions. Validate or describe the required ordering per revision. Recompose at output resolution where compatible; retain a conservative output path when native-resolution guest HUD rendering is unsupported.

Build HUD-less final colour and an explicit overlay/composition strategy. Arbitrary guest blending cannot always be replayed into a transparent layer unchanged; destination-dependent blending, masks and interleaved world/UI draws need validation. Fullscreen menus, video and split-screen need distinct handling. Initially disable temporal features for unsupported multiple-view frames, then extend to per-view histories.

Exit: Quality/Balanced/Performance produce the intended internal dimensions, stable post-processing and sharp supported HUD paths. Measure a GPU-time improvement in a GPU-bound scene; CPU-bound results are reported separately.

### 5. Add XeSS SR and DLSS SR/DLAA

Reuse the validated inputs and lifecycle. Implement XeSS capability/init/execute paths and Streamline SR integration. Validate native AA and at least Quality mode independently for each provider, then the remaining modes. Keep provider-specific exposure and motion adaptation inside adapters.

Intel's [SDK overview](https://github.com/intel/xess) describes SR and FG as distinct features. Query runtime support rather than using a GPU-brand-only check. Permit explicit fallback to FSR or ordinary rendering, and report it visibly.

Exit: the same captured scenes pass on applicable NVIDIA, AMD and Intel hardware, with supported feature-off builds and missing-runtime tests.

### 6. Add FSR frame generation on D3D12

First replace/wrap presentation with generation disabled and verify resize, minimisation, borderless switching and exit. Then enable one generated frame between real frames. Supply validated depth, motion, frame timing, resets and HUD-less/composition inputs according to the pinned SDK. Integrate frame pacing and latency control before assessing responsiveness.

Only submit a new real frame once. Repainting an old buffer must not advance history. Pause or bypass generation for menus, loading, video, missing camera/input resources and unsupported frame layouts. Camera cuts reset interpolation. SDK failures must drain safely and restore ordinary presentation.

Generated presents must not trigger extra guest vblank callbacks, advance simulation/audio clocks, consume extra guest input or create guest swap events. They improve displayed motion; they do not raise game simulation speed. Record real FPS, displayed FPS, GPU cost, pacing and latency separately.

Exit: moving objects and HUD pass capture review, frame times remain paced, and repeated toggles/resizes/exits complete without stale resources or deadlocks.

### 7. Add DLSS FG and XeSS FG

DLSS FG uses the Streamline presentation integration and required Reflex markers. Keep tokens and marker IDs aligned with the real frame. Detect hardware, driver, OS and presentation support through the SDK. NVIDIA's [FG guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md) specifies resource tagging, lifetime and Reflex integration. Start with 2x output; expose multi-frame or dynamic modes later only when queried support and pacing tests pass.

XeSS FG uses its own proxy swapchain and mandatory XeLL integration. Intel's [FG guide](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md) describes these requirements. Start with 2x, then validate optional modes. Switch providers by draining and recreating the required presentation state; never stack their swapchain proxies.

Exit: each provider and its fallback pass the same lifecycle and latency checks as FSR FG. Validate independent combinations such as DLSS SR + FSR FG and FSR SR + XeSS FG where supported.

### 8. Add Vulkan parity and ship measured title support

Reuse the frontend annotations and input contract; implement Vulkan layouts, descriptor handling, barriers, queue ownership and lifetime rules explicitly. Port FSR SR first, then FSR FG. Evaluate DLSS and XeSS against the selected SDK's API/OS capabilities; their support matrix is not identical. NVIDIA documents Vulkan FG as well as feature-specific limitations, so D3D12-first is a staging decision, not a claim that Vulkan FG is impossible.

Validate Windows Vulkan and native Linux separately; Proton is another test target, not evidence of native support. Publish capabilities by backend/provider/OS. Keep unsupported combinations disabled with a concrete explanation.

Exit: focused backend checks, real-title captures, package verification and sustained gameplay/resize/exit validation on supported configurations.

## Validation and release criteria

Use existing temporal/frontend tests for maths, profile selection and frame boundaries. Add meaningful tests for missing-resource fallback, reset propagation, provider recreation, distinct frame IDs and generated-present isolation from guest timing. Use a small animated GPU scene to validate camera and object motion independently of game correctness.

Run Release backend integration tests with D3D12 debug layer or Vulkan validation as appropriate. Capture input buffers and output sequences for stationary detail, pans, moving actors/aircraft, disocclusions, foliage/particles, transparent HUD, pause/loading, cuts and resolution changes. Test missing/wrong-version runtime files and unsupported hardware. Check VRAM growth and shutdown after repeated provider switches.

The launcher/runtime settings should show requested and active upscaler, quality mode, requested and active FG, actual render/output size and why a selection is unavailable. Use SDK capability-derived modes. Ship temporal features as opt-in per validated revision, with camera-only motion clearly marked experimental. Avoid promising support for every Xbox 360 title from a single successful profile.

The first renderer implementation slice is milestones 0–2: a measured title profile, backend annotations and inspectable colour/depth/motion. Universal manifests, spatial fallback and launcher design can start alongside it. Completion of the renderer slice determines the remaining title-specific difficulty far more reliably than an SDK-only prototype or a calendar estimate.

## Programme backlog and dependency gates

The rows below are reviewable implementation epics, not instructions to start implementing now. Each epic must preserve the existing dirty tree and distinguish generic Xenon work from title/module data. UI fixtures can be designed early, but product availability must wait for real host capabilities.

| ID | Deliverable and likely owner area | Depends on | Acceptance evidence |
| --- | --- | --- | --- |
| R01 | Shared capability/feature-state contracts; graphics + runtime host | Baseline audit | Typed requested/available/active state, per-feature reasons, version negotiation and old-host behaviour |
| R02 | Spatial scaling and safe baseline for every native backend | R01 | Real scanout checks for every intake path; correct colour/aspect/UI expectations |
| R03 | Support manifests and preparation/catalogue integration | R01 | Every official revision has explicit feature dispositions; local modules get unassessed status |
| R04 | Resource graph and backend pass annotations | R01 | Draw/resolve classification reaches execution in the right frame; alias/MSAA/view fixtures |
| R05 | D3D12 jitter, sampled depth and camera-motion inputs | R04 | Inspectable GPU inputs, motion convention checks, history/reset and feature-off equivalence |
| R06 | Stable object identity and rigid/deformed motion classes | R05 | Animated and independent object tests, previous-state lifetime and bounded memory |
| R07 | FSR native AA and failure recovery | R05 | Correct reconstruction and provider fallback; camera-only scope remains explicit until R06 |
| R08 | Host scene scaling, post-process and UI composition | R04, R07 | Non-integer supported extents, unchanged guest memory semantics, output-resolution UI cases |
| R09 | XeSS/DLSS SR adapters and managed packages | R07, dependency/licence decisions | Native AA + scaled scene captures, capability checks, missing-package and recreation checks |
| R10 | FSR FG, presentation ownership and pacing | R06, R08 | Swapchain-only pass, real-frame identity, UI review and generated-present timing isolation |
| R11 | Streamline FG/Reflex and XeSS FG/XeLL | R09, R10 | Single FG owner, supported mixed SR/FG combinations, marker correctness and transitions |
| R12 | Launcher global/per-game Graphics menus | R01; backend capabilities for activation | Inheritance, localisation, controller navigation, real availability and saved request handoff |
| R13 | Negotiated live settings and session graphics status | R01, R12; relevant renderer lifecycle | Acknowledged applied state, stale request rejection, rollback/restart and reattachment |
| R14 | Vulkan temporal and FG adapters | R04; corresponding D3D12 contracts | Vulkan validation, queue/lifetime tests, API/OS-specific capability matrix |
| R15 | Analysis authoring, recipe updates and regression lab | R03, R04, R05 | Reviewed candidates for unrelated engines, versioned evidence and recipe rollback |
| R16 | Multiple views, hybrid renderers and difficult composition | R06, R08, R15 | Split-screen/scopes/mirrors and orthographic/hybrid cases without shared-history leakage |
| R17 | Local advisor, comparisons and adaptive resolution | Stable R08–R15 | Repeatable real-game measurements, honest confidence, SDK-compatible DRS and opt-in policy |
| R18 | Catalogue-wide enhancement completion | R03, R15, R16; per-feature adapters | Every supported revision/mode assessed; remaining blocked cells have concrete work and tested fallbacks |

R18 has two distinct gates: universal framework coverage requires assessment, a common menu and fallback for every added game; full applicable enhancement coverage additionally requires validation of the applicable temporal/FG cells. Known blocked cells keep the latter gate open. Techniques that are fundamentally inapplicable require a documented rationale and validated alternative, not a generic exemption.

R12 can first be delivered against clearly labelled mock capabilities; that does not complete its real-runtime gate. R15's basic evidence format and fixtures should start with R01, even though its full authoring automation depends on later work. R18 is an ongoing release requirement, not a final one-time task after all other work ends.

### Release slices

1. **Universal foundation:** R01–R04, spatial coverage and manifest intake, first menu design and synthetic input fixtures. Every game is visible in the support system.
2. **End-to-end temporal slice:** R05–R09 plus real R12. One validated title/revision and a second unrelated renderer expose generalisation failures early.
3. **Smooth presentation:** R06/R08 quality gates, R10–R11 and R13. Ship 2x FG before extending optional multipliers and dynamic modes.
4. **Backend and catalogue expansion:** R14–R16 and continuous R18. Include native Linux separately from Windows/Proton and difficult view/render modes.
5. **Advanced experience:** R17 and the optional native runtime menu. Keep automatic tuning evidence-based and preserve explicit user control.

Do not attach calendar dates until the first two real render architectures establish motion and composition costs. Estimate each epic after its preceding technical gate; keep research spikes separate from committed feature delivery.

## Architecture decisions to settle before implementation

| Decision | Recommended starting position | Required experiment or review |
| --- | --- | --- |
| NVIDIA SDK ownership | Shared Streamline bootstrap/lifecycle for SR + FG | Compare direct NGX SR interoperability and deployment with the pinned SDK |
| Classification handoff | Explicit annotations available during execution | Prove streaming frame metadata is sufficient without waiting for end_frame |
| Scale representation | Host-only enhanced resources with guest alias mapping | Non-integer dimensions, resolve readback, overlapping surfaces and CPU access |
| Motion storage | Per-class trustworthy previous state with bounded lifetimes | Rigid/skinned scene correctness and representative CPU/GPU/VRAM cost |
| HUD handling | Per-revision composition strategy with native output target where valid | Destination-dependent blending and interleaved world/UI |
| Live transport | Versioned session requests/acks compatible with current supervision | Bounded file transport versus local IPC, reattachment and stale-session behaviour |
| Recipe schema | Explicit version transition and declarative bounded facts | Compatibility with current v1 parser and effective-image identity |
| Enhancement defaults | Existing baseline preserved; user-selected presets and opt-in FG | Migration and actual before/after launch state |

Each decision record should state evidence, supported scope, rejected alternatives and reversal cost. The plan deliberately leaves unknown algorithms as experiments rather than treating them as solved engineering.

## Product completion checklist

- Every import/preparation/module-install path creates or loads a revision-scoped support record.
- Every game uses the same launcher Graphics surface, with accurate availability and source-of-setting information.
- Selecting a vendor name cannot bypass input validity, device support or title-quality gates.
- Native AA, temporal scaling, spatial scaling and frame generation are explained and measured separately.
- Guest rendering correctness, saves, timing and input remain protected across fallback and generated presents.
- Managed SDK/recipe updates are versioned, recoverable and usable offline after installation.
- Support evidence includes unrelated renderers and multiple views; full catalogue reporting includes unsupported and regressed cells.
- Release claims distinguish code availability, hardware support, validated quality and current active operation.

Advanced ray reconstruction, invented scene geometry, arbitrary game-engine frame unlocks and a replacement game renderer are separate projects. This programme should first establish exceptional reconstruction quality and presentation across existing Xenon games without depending on those additions.
