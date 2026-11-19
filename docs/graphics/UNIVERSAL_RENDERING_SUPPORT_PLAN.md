# Rendering enhancement support for every Xenon game

Planning specification, 11 November 2026. No implementation is authorised by this document. Companion to [the renderer integration plan](UPSCALING_FRAME_GENERATION_PLAN.md) and [the Launcher Next menu specification](LAUNCHER_GRAPHICS_MENU_PLAN.md).

## Product objective and support commitment

Make high-quality reconstruction, smooth presentation and understandable configuration a standard Xenon service that every game module inherits. The long-term target is validated temporal upscaling and frame generation throughout the supported catalogue. The immediate architectural requirement is that every added game participates in the same analysis, capability reporting, settings, fallback and regression system.

These are separate commitments. A common API can cover every game; it cannot manufacture correct depth or previous deformation state for every renderer. A title with an unknown camera or inseparable HUD must remain playable through its established baseline while its missing enhancements become explicit engineering work. Importing a game must never imply that temporal features have been validated.

Every official module release must contain a rendering support manifest, even when its current result is baseline or spatial support only. Missing temporal support must have a named reason, evidence requirement and tracked task. Publish complete catalogue coverage, including unknown and blocked revisions, rather than counting only successful demonstrations. This makes universal support an operational programme as well as an interface.

## Support is a matrix

Store capability and validation independently for each feature and operating mode. Avoid a single numbered tier that implies frame generation requires a particular upscaler or that good camera detection proves motion quality.

| Feature | Minimum contract | What validation establishes |
| --- | --- | --- |
| Baseline presentation | Valid scanout, size, colour interpretation | Existing image remains correct |
| Spatial scaling | Final image and known output encoding | Scaling and sharpening work without temporal history |
| Temporal native AA | Stable jitter, matched colour/depth/motion, history | Reconstruction at equal input/output size |
| Temporal upscaling | Native-AA inputs plus coherent scaled scene graph | Reduced internal scene size with correct sampling/composition |
| Frame generation | Real-frame identity, valid inputs, composition and pacing | Intermediate frames remain acceptable in tested scenes |
| Native-resolution guest UI | Verified UI boundary and blending path | Supported HUD/menu classes render sharply at output size |
| Multiple views | Per-view camera, history, extents and composition | Split-screen, mirrors or alternate cameras avoid history contamination |

For each cell record `unassessed`, `candidate`, `experimental`, `validated`, `blocked` or `regressed`, plus evidence and scope. Hardware availability is another axis: a title can be validated for DLSS while the current computer cannot run it. Runtime activity is a third axis: an available feature can be temporarily suspended during a movie.

Dimensions include effective executable hash, rendering-relevant patch/mod identity, runtime/translator build, backend, OS, SDK implementation/version, device/driver family, output format and view mode. Validation need not exhaust every GPU, but its tested scope must be explicit. Shared engine recognition can produce candidates across games; it cannot silently transfer a validated badge.

## Universal fallback

Provide a GPU-side spatial path over valid final scanout for every supported native backend: point/bilinear for exact or inexpensive scaling, plus an optional spatial reconstruction/sharpening implementation such as [AMD FSR 1](https://gpuopen.com/fidelityfx-superresolution/). Label this separately from temporal FSR. It needs no camera or object-motion inference and therefore supplies broad coverage while temporal support matures.

Spatial scaling of a complete frame also scales its HUD and cannot recreate a native-resolution HUD. State that plainly. Scaling a final 720p image to a larger display does not reduce guest scene rendering cost; only lowering actual scene work offers that opportunity. Do not advertise a performance improvement from output enlargement alone.

For non-3D or atypical games, assess whether a spatial path is the best-quality option rather than forcing a perspective-camera model. Pure video, orthographic rendering, mixed 2D/3D, frame-doubled video and heavily interleaved world/UI require distinct classifications. Do not feed guessed matrices to vendor temporal APIs merely to expose every toggle.

An image-only optical-flow interpolation experiment may be researched separately for remaining titles. It is not a substitute for the promised SDK input work, is not assumed to be DLSS/XeSS FG, and must pass its own latency, UI and quality gates before becoming a product option.

## Mandatory onboarding for every module

1. **Identify.** Resolve the effective executable after title updates, region and relevant patches. Record renderer and analysis schema requirements. Reuse only an exact matching validated recipe or an explicitly supported family with checked invariants.
2. **Observe.** Run bounded discovery, initially without altering output. Gather render-target lifetime and alias relationships, resolves, viewport/scissor changes, shader usage, camera candidates, depth encodings and late-frame composition.
3. **Propose.** Generate a candidate profile with provenance and confidence for each field. Distinguish directly observed data from inferred intent. Include alternative scene/HUD boundaries when ambiguous.
4. **Inspect.** A module author reviews colour, depth and motion debug views and representative sequences. Discovery must not certify its own output using the same heuristics that generated it.
5. **Validate.** Run the scene matrix and provider/backend checks below. Approve individual feature cells, not the entire game indiscriminately.
6. **Package.** Include profile, support manifest, recipe version, known limitations and references to validation evidence in the revision package. Keep generated caches out of the source recipe.
7. **Monitor.** Runtime checks input validity each real frame and records bounded suspension/fallback events. New observations can open a local candidate update without modifying a shipped profile.

Preparation should offer this analysis as a persisted, cancellable job using the existing Xenon preparation and launcher job paths. Static inspection may finish during preparation; dynamic evidence requires an actual run, compatible replay or author-provided capture. Never show a fabricated percentage for unobserved gameplay. Users can play in the baseline path while analysis is pending.

Official catalogue intake requires a manifest and explicit feature dispositions. Community/local modules remain launchable with a generated `unassessed` manifest. A rendering regression can quarantine the enhancement recipe without removing the user's game or changing saves.

## Recipe schema and provenance

Retain the existing revision-scoped `rendering.json` loading model. Extend it through an explicit versioned schema when required; do not reinterpret `xenon.rendering.v1` fields or allow old runtimes to execute a new recipe incorrectly. Separate declarative rendering facts from provider/hardware validation results.

Proposed package components:

| Component | Responsibilities |
| --- | --- |
| `rendering.json` | Cameras/views, scene graph boundaries, depth convention, jitter eligibility, motion sources and composition rules |
| `rendering-support.json` | Exact applicability, feature cells, limitations, evidence references, minimum runtime contracts |
| Validation recipe | Scene names, reproducible entry conditions, expected properties and capture hashes where appropriate |
| Local analysis cache | Device-dependent results, observations and unreviewed candidates; never overrides the shipped recipe implicitly |

Use typed bounded fields and allowlisted operations. Do not embed arbitrary C++/shader code in a downloadable JSON recipe. Engine-specific native hooks, when necessary, remain explicit module extension code under existing module loading policy. A shader hash is an observation key, not enough to identify an object, camera or draw role alone.

Each recipe needs author/source, creation tool/version, parent recipe, exact image identity, observed render modes and review status. A renderer patch that changes resource interpretation invalidates affected cells. A new driver or SDK changes the environment fingerprint and prompts bounded revalidation; it need not erase unrelated title facts. Locally installed older recipes and runtimes remain usable offline.

Illustrative support-record shape below. The schema is proposed and identity placeholders must be replaced by verified values before packaging; this is not accepted by an existing parser.

```json
{
  "schema": "xenon.rendering-support.v1",
  "effectiveImageHash": "<verified-effective-image-hash>",
  "recipeId": "<immutable-recipe-content-hash>",
  "minimumRenderingContract": 1,
  "provenance": {
    "analysisTool": "<tool-and-version>",
    "review": "candidate",
    "parentRecipeId": null
  },
  "features": [
    {
      "feature": "temporalUpscaling",
      "provider": "fsr",
      "backend": "d3d12",
      "viewMode": "singleView",
      "status": "candidate",
      "inputQuality": "cameraMotionOnly",
      "reasonCodes": ["DynamicMotionAwaitingValidation"],
      "evidenceRefs": []
    }
  ],
  "fallback": {
    "temporalFailure": "spatial",
    "frameGenerationFailure": "ordinaryPresentation"
  }
}
```

The final schema must define hash algorithms, bounded lists/strings, enum evolution, evidence resolution and unknown-field behaviour. A candidate record must never satisfy a validated-support predicate. Feature rows not explicitly covered are unassessed, not implicitly supported. Evidence references identify local or separately published reports; they do not authorise automatic network access during rendering.

Distribute recipe revisions through existing module/catalogue update mechanisms with integrity verification, atomic installation and rollback. Pin the recipe and provider package for an active session. No mid-session remote replacement. Content hashes or small metadata may be shared voluntarily; game binaries, shader dumps and captures must not be uploaded automatically.

## Generic render analysis

Grow the existing analyser into a resource-use graph spanning a real guest frame and, where needed, prior frames. Track resource subregions, samples, format reinterpretation and physical aliases. Detect reused EDRAM tiles by generation/lifetime rather than assuming a tile permanently represents the scene.

Use several independent signals: matching camera, depth continuity, target coverage, resolve consumers, draw geometry, shader resource reads and later composition. Confidence must be calibrated on held-out scenes and games. False positive scene classification is more damaging than declining to jitter an uncertain pass.

Maintain explicit scene phases and multiple views. Mirrors, scopes, cockpit displays and shadow cameras must not overwrite main-camera history. One guest swap can contain several views; one command submission need not equal a frame. A menu may still contain 3D background geometry. View IDs persist only while their identity remains valid.

Add semantic annotations at the frontend/backend boundary rather than spreading title-ID checks through D3D12 and Vulkan. Generic rules belong in Xenon; measured per-title facts belong in revision recipes; unavoidable engine adapters are reusable modules with declared compatibility. Keep expensive discovery off the release hot path once a recipe is validated.

## Motion reconstruction programme

Support complementary motion sources, selected per view or draw class:

- **Guest-produced vectors:** locate and decode them where present; verify scale, coverage, jitter and encoding before reuse.
- **Camera reprojection:** derive background motion from depth and unjittered camera transforms; explicitly identify invalid depth and disocclusions.
- **Rigid geometry:** obtain stable instance identity and previous transforms/vertex inputs. Account for instancing and buffer reuse across frames.
- **Deformed geometry:** preserve prior skinning/morph/vertex-animation inputs or instrument translated shaders to emit previous/current projected positions using trustworthy prior state.
- **Unsupported/translucent classes:** use reactive/disocclusion handling and conservative history rules; measure the remaining artefacts before declaring support.

Shader instrumentation must respect rasterisation, clipping, depth tests, colour attachment limits and backend pipeline compatibility. Extra outputs may require a dedicated velocity pass; replaying a draw must not repeat guest side effects, queries or memory writes. A previous shader invocation is not valid if its buffers have already been overwritten. Set budgets for retained data and reject a technique that requires unbounded geometry history.

Depth-only camera vectors are a useful first milestone, not catalogue-wide FG quality. Engine-family adapters can accelerate rigid/skinned motion support once independently verified across revisions. Explore optical-flow assistance as additional evidence, not a guarantee of correct object semantics.

## Runtime policy and recovery

Evaluate readiness at launch, view changes, resource transitions and provider changes. Keep inexpensive per-frame validity checks: current colour/depth/motion agree on extent and frame, history exists, camera is sane, output encoding is known, and resources remain alive. Do not run full discovery every frame in a validated release path.

On a camera cut reset history. On a temporary missing input suspend that feature and use the agreed fallback. On repeated dispatch/device faults quarantine that provider configuration for the session, safely drain its work and restore ordinary presentation if the device remains usable. Persist a crash-recovery recommendation without silently rewriting the user's requested settings. Device removal may require ending/restarting the session; recovery is not universally possible in-place.

Use hysteresis so one uncertain frame cannot cause a swapchain recreation loop. Frame generation can enter a supported passthrough state during short interruptions; larger provider changes follow a controlled drain/recreate sequence. The launcher displays the active outcome and a concise reason.

Automatic selection is deterministic for a pinned environment. Filter for validated title/backend support, runtime/device capability, user quality target, latency preference and memory budget. Then select among tested candidates using optional local measurements. Do not always rank a vendor first, silently enable FG, or lower quality during play without an opted-in adaptive policy. Preserve manual choices while reporting why they are inactive.

## Differentiators to earn

| Proposed capability | User benefit | Evidence required |
| --- | --- | --- |
| Automatic per-revision setup | Every newly added game enters one predictable workflow | All intake paths produce a manifest and understandable status |
| Native HUD and multiple views | Sharp text and correct cockpit/split-screen images | Validated blend/view cases across unrelated engines |
| Independent SR/FG selection | Best supported combination for the user's device | Cross-provider matrix and single presentation owner |
| Local quality/performance advisor | Recommendations reflect the actual machine and game | Repeatable benchmark, GPU/CPU bottleneck detection and confidence |
| Image comparison and diagnostics | Users and authors can identify ghosting or regressions | Aligned captures, real FPS and settings provenance |
| Recipe updates independent of recompilation | Rendering support improves without rebuilding game code when facts alone change | Schema/runtime compatibility, rollback and offline tests |
| Opt-in adaptive scene resolution | Maintain a render-time budget while preserving HUD size | SDK-compatible bounds, stable history and measured response |
| Open coverage reporting | Users see exactly what works in their revision | Complete denominator, tested scope and current evidence |

These are proposed differentiators, not claims of market leadership. Establish comparisons using reproducible scenarios, public methodology and equivalent hardware/settings before making competitive claims.

## Quality lab and measurable gates

Build synthetic scenes for rigid/skinned motion, transparency, particles, camera cuts, reversed/infinite depth, MSAA, multiple views, unusual aspect ratios, variable frame cadence, duplicate presents and destination-dependent HUD blending. Follow with unrelated engine families and render architectures, including a 2D/orthographic title. Do not validate universal heuristics only on AC6 and Gears-like rendering.

Use timestamp queries for GPU stages and monotonic real-frame IDs. Report real rendered FPS, displayed FPS, delivered-frame intervals, p50/p95/p99 frame times, interpolation cost, memory and reset/fallback counts. Report measured latency only when measurement supports it; an input-poll-to-submit estimate is not end-to-end display latency. Warm shader caches before steady-state comparisons and report compilation separately.

Proposed initial acceptance targets, to be ratified against baseline hardware:

| Gate | Target and method |
| --- | --- |
| Catalogue accountability | 100% of official module revisions have a support manifest and explicit feature dispositions |
| Features off | Zero visual/guest-memory differences attributable to enhancements; target <=1% median frame-time overhead in a controlled repeated benchmark |
| Recipe coverage | Every supported render mode has a reviewed scene list; unsupported modes trigger a deterministic fallback |
| Motion/composition | No accepted severe ghosting, UI corruption or history leakage in the agreed capture suite; quantitative image metrics supplement human sequence review |
| Lifecycle | 100 provider transitions/resize cycles without deadlock or resource accumulation, followed by a 30-minute supported-title session |
| Timing isolation | Synthetic checks prove generated presents do not advance guest simulation, vblank, input consumption or audio clocks |
| Overload | Bounded memory, queues and discovery work; visible degradation/recovery rather than a presentation stall loop |

A full replay system is an additional project, not an assumed existing capability. Start with synthetic GPU inputs and small owned IR/resource captures; document non-deterministic guest state and licence restrictions. Use scene instructions plus local recordings until capture replay is trustworthy. Golden outputs must be versioned by backend/SDK and reviewed when algorithms change.

## Engineering sequence and open decisions

First deliver manifest/capability contracts and a spatial fallback for all existing intake paths. In parallel with later renderer work, design the launcher against explicit fixture capabilities labelled as mock data. Then deliver one complete vertical slice, a second unrelated engine, multiple views and scalable recipe authoring before claiming broad compatibility.

Resolve these with small experiments before expensive implementation: reliable dynamic-object identity; previous deformation retention cost; interleaved HUD reconstruction; non-integer host scaling with guest resolves; per-view recipe expressiveness; Streamline SR versus direct NGX; and supported local capture/replay boundaries. Record each choice, rejected alternative, evidence and reversal cost.

Engineers may complete generic architecture while a title is blocked. Missing title data is tracked per feature/revision, with the narrow next observation required. The programme is complete only when the catalogue coverage and quality gates are met, not when three SDKs initialise successfully.

Separate two milestones when reporting progress: **universal framework coverage** means all games enter the system with accurate capability state and fallback; **full applicable enhancement coverage** means every supported revision/render mode has validated temporal/FG support wherever that technique applies. A blocked cell is unfinished work for the latter. A genuinely inapplicable feature, such as perspective-camera temporal reconstruction for a pure video path, needs an explicit technical rationale and validated alternative. Assessment alone must not be reported as full enhancement support.
