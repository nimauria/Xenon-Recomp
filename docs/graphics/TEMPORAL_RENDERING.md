# Temporal rendering: upscaling, frame generation and HUD separation

Temporal upscalers (AMD FSR, Intel XeSS, NVIDIA DLSS) and frame generation
need things a 2005-era console renderer never produced:

- a sub-pixel jitter on the 3D scene;
- per-pixel motion vectors;
- the lit scene colour and depth before post-processing;
- a clear boundary between the 3D scene and the HUD and menus.

The design goal is to keep the HUD sharp at output resolution and out of
interpolation; composition still needs implementation and validation.

Xenon currently provides temporal maths, revision profiles, pass
classification and probes over the guest GPU command stream. It does not yet
produce GPU motion buffers, apply jitter, run an upscaler or generate frames.
A game module can supply a per-revision data file identifying camera, scene
and HUD candidates; classification alone does not prove usable motion or
correct composition for every title.

The delivery sequence and quality gates are in the
[upscaling/frame-generation plan](UPSCALING_FRAME_GENERATION_PLAN.md), with
[catalogue-wide support](UNIVERSAL_RENDERING_SUPPORT_PLAN.md) and
[launcher settings](LAUNCHER_GRAPHICS_MENU_PLAN.md) specified separately.

## The split

| Xenon (every game) | Game module (per executable revision) |
| --- | --- |
| Halton jitter sequence and phase count (`temporal.hpp`) | which vertex constants hold the view-projection matrix, and their layout |
| perspective detection, camera parameters, camera cuts | camera-cut thresholds, camera-position constant for camera-relative games |
| per-draw classification: scene, screen-space scene, scene-colour resolve, post-process, HUD (`temporal_frame.hpp`) | lit-scene colour formats, optional scene depth tile |
| reprojection matrix and reference camera-motion arithmetic from depth (no GPU motion pass yet) | which shaders begin the HUD |
| the temporal probe that discovers the module's values | |

The module data is `rendering.json` in the module's hint package, at
`<module>/xenon-analysis/revisions/<effective image hash>/rendering.json`.
The full schema is in `include/xenon/gpu/temporal_profile.hpp`.

```json
{
  "schema": "xenon.rendering.v1",
  "temporal": {
    "camera": { "vertexConstant": 0, "layout": "rows" },
    "scene": { "colorFormats": ["R16G16B16A16Float"] },
    "hud": { "pixelShaders": ["0x0123456789abcdef"] },
    "cameraCut": { "degrees": 45 }
  }
}
```

The file is read at launch, not compiled in. Editing it takes effect on the
next launch without re-preparing the game. A malformed file fails the
launch with the field named. Project Gracemeria's exporter validates it with
the same parser before shipping.

## How a frame is classified

`TemporalFrameAnalyzer` observes the graphics IR in `GraphicsSystem::execute_ir`
before each command reaches the backend, so it works the same on D3D12 and
Vulkan. A frame ends at the front-buffer swap that shows it.

1. The frame's **camera** is the first depth-writing draw whose vertex shader
   reads the profile's four constants, and whose matrix there is a
   perspective projection.
   - Until the scene depth target is known, that draw must also write colour.
     This keeps a shadow map drawn through the same constants from being
     taken for the camera.
   - A frame that reaches its scene-colour resolve fixes the depth target for
     later frames. A profile can also pin it with `scene.depthTile`.
2. **Scene** draws use that exact matrix and test against the scene depth.
   They are candidates for jitter once the GPU integration is implemented.
   - Other draws into the scene depth are **screen-space scene** draws, such
     as lighting and fog quads. They are not jittered, because their depth
     lookups would shift.
   - Everything else before the scene-colour resolve is **offscreen**.
3. The **scene-colour resolve** copies out the scene's colour target, the
   colour target bound with the camera draws. This is a candidate boundary
   for a future temporal upscaler; no provider dispatch runs here yet.
4. After it, draws are **post-process** until the first draw using a
   profile HUD shader. From that draw on, everything is **HUD**.
5. A frame has no usable history (`reset`) in any of these cases:
   - it is the first frame;
   - it had no camera, as in a menu;
   - the view turned past `cameraCut.degrees`;
   - the camera moved past `cameraCut.distance`.

## Writing a profile with the temporal probe

Set `"temporalProbeFrames": 60` in the launch configuration. The session log
then gets a `[TemporalProbe]` report every 60 frames.

1. With no profile, the report lists **camera candidates**. These are vertex
   constant ranges holding a perspective matrix on depth-writing draws. Each
   shows its layout, depth tile, draw count, field of view and aspect. The
   game camera is normally the one with the most draws, an aspect matching
   the screen, and a plausible field of view.
2. Put that constant and layout in `rendering.json` and run again. The report
   now shows:
   - the scene depth and colour targets;
   - the scene-colour resolve;
   - each shader pair drawn after the scene, in order of first use.

   The first HUD element's pixel shader goes in `hud.pixelShaders`.
3. Run once more to confirm `hud: from draw N` and that `RESET` appears only
   at real cuts.

## Status and next steps

**Implemented and tested:**

- the temporal maths;
- the profile schema, its loading for the running revision, and launch
  failure on a bad file;
- frame classification in the live GPU path;
- the temporal probe and its discovery mode.

Nothing yet changes what is rendered.

**Real-title validation gate:** AC6 now loads through the main menu into the
first cutscene with fully working audio (owner report, 11 November 2026).
Cutscene frame rate/pacing is being fixed; gameplay remains unconfirmed.
Enhancement qualification requires correct baseline images and timing for the
tested scenes. Shared contracts and synthetic GPU work can proceed independently.

**Planned GPU work:** the linked implementation plan defines the delivery
order, dependencies and acceptance gates. The main missing pieces are:

1. **Jitter.** Offset the viewport of scene draws by the frame's jitter,
   applied as an IR rewrite before the backend, so it is backend-independent.
   It is enabled only while a temporal consumer is active, since jitter
   without one only shakes the image.
2. **Motion vectors.** A compute pass per backend that applies the
   reprojection matrix to the scene depth (`temporal::camera_motion` is the
   reference arithmetic).
3. **Upscaler providers** behind one interface:
   - FSR, from AMD's SDK;
   - XeSS, from Intel's SDK;
   - DLSS, from NVIDIA's SDK.

   Dependency-resolver integration is planned; SDK revisions, licences and
   redistribution terms must be recorded per selected package. Vendor binaries
   are to stay out of source control.
   Native-resolution anti-aliasing (render size = output size) comes first;
   it needs no scaled rendering.
4. **Scaled 3D passes.** Render scene targets below output resolution while
   post-processing and HUD stay at output resolution, for real upscaling.
5. **Frame generation and HUD composition.** Draw the HUD into its own target
   so that interpolated frames get the HUD composited on top rather than
   interpolated.

COGBox's Gears rendering integrations are research references, as recorded in
the implementation plan. They do not establish that Xenon's classifier alone
is sufficient for dynamic-object motion, multiple views or interleaved HUDs.
Those inputs and composition paths require independent implementation and
validation. No COGBox code is copied in this groundwork.
