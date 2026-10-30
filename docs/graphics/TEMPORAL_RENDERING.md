# Temporal rendering: upscaling, frame generation and HUD separation

Temporal upscalers (AMD FSR, Intel XeSS, NVIDIA DLSS) and frame generation
need things a 2005-era console renderer never produced:

- a sub-pixel jitter on the 3D scene;
- per-pixel motion vectors;
- the lit scene colour and depth before post-processing;
- a clear boundary between the 3D scene and the HUD and menus.

The HUD stays sharp at output resolution and is never interpolated.

Xenon provides all of this generically, from the guest's GPU command
stream. A game module supplies only a small per-revision data file that says
where its game keeps the camera, its lit scene and its HUD.

## The split

| Xenon (every game) | Game module (per executable revision) |
| --- | --- |
| Halton jitter sequence and phase count (`temporal.hpp`) | which vertex constants hold the view-projection matrix, and their layout |
| perspective detection, camera parameters, camera cuts | camera-cut thresholds, camera-position constant for camera-relative games |
| per-draw classification: scene, screen-space scene, scene-colour resolve, post-process, HUD (`temporal_frame.hpp`) | lit-scene colour formats, optional scene depth tile |
| reprojection matrix and camera motion from depth | which shaders begin the HUD |
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
   They are the draws that get jitter.
   - Other draws into the scene depth are **screen-space scene** draws, such
     as lighting and fog quads. They are not jittered, because their depth
     lookups would shift.
   - Everything else before the scene-colour resolve is **offscreen**.
3. The **scene-colour resolve** copies out the scene's colour target, the
   colour target bound with the camera draws. This is where a temporal
   upscaler runs.
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

**Deferred:** the steps below wait until Ace Combat 6 runs correctly, the
first game whose profile will be measured. Until then this groundwork only
observes.

**Next, in order:**

1. **Jitter.** Offset the viewport of scene draws by the frame's jitter,
   applied as an IR rewrite before the backend, so it is backend-independent.
   It is enabled only while a temporal consumer is active, since jitter
   without one only shakes the image.
2. **Motion vectors.** A compute pass per backend that applies the
   reprojection matrix to the scene depth (`temporal::camera_motion` is the
   reference arithmetic).
3. **Upscaler providers** behind one interface:
   - FSR, from AMD's FidelityFX SDK, MIT;
   - XeSS, from Intel's SDK, under its own licence;
   - DLSS, from NVIDIA's SDK, under the RTX SDK licence.

   SDK binaries are fetched by the dependency resolver and never committed.
   Native-resolution anti-aliasing (render size = output size) comes first;
   it needs no scaled rendering.
4. **Scaled 3D passes.** Render scene targets below output resolution while
   post-processing and HUD stay at output resolution, for real upscaling.
5. **Frame generation and HUD composition.** Draw the HUD into its own target
   so that interpolated frames get the HUD composited on top rather than
   interpolated.

The design follows the published approach of COGBox (Gears of War static
recompilations, MIT). Those projects show this classification is enough for
FSR, DLSS and XeSS on real Xbox 360 renderers. No COGBox code is copied.
