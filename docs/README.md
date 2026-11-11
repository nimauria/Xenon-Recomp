# Documentation guide

Current status was updated on 11 November 2026. Runtime progress below comes
from the project owner's testing; this documentation update did not rerun AC6.

## Current runtime milestone

Ace Combat 6 loads through the main menu into the first cutscene. Audio is
fully working in that tested sequence. Cutscene frame rate/pacing is actively
being fixed. Gameplay, sustained play and full-title compatibility remain
unconfirmed; no commercial title is confirmed playable.

The [root README](../README.md#current-milestone-project-gracemeria) is the
current compatibility summary. [Audio V1](audio/AUDIO_V1.md#current-validation-boundary)
records audio scope. Generic fixes belong in Xenon; title profiles, generated
code and title-specific changes belong in Project Gracemeria.

## Rendering plans and implemented groundwork

Read these in order:

1. [Temporal rendering](graphics/TEMPORAL_RENDERING.md): implemented maths,
   revision profiles, pass classification and probes. These observe rendering;
   GPU jitter, motion buffers and SDK integration are not implemented.
2. [XeSS, FSR, DLSS and frame generation](graphics/UPSCALING_FRAME_GENERATION_PLAN.md):
   shared GPU inputs, D3D12-first delivery, native AA, scaled rendering,
   provider integration, frame generation and later Vulkan work.
3. [Universal rendering support](graphics/UNIVERSAL_RENDERING_SUPPORT_PLAN.md):
   assessment for each game revision/render mode, motion and HUD quality,
   capability reporting and safe fallbacks.
4. [Launcher graphics menus](graphics/LAUNCHER_GRAPHICS_MENU_PLAN.md): proposed
   global/per-game controls, inheritance and reporting of the actual active state.

The last three documents are plans. Their proposed settings, providers and
schemas are not available features. Support depends on title inputs, backend,
hardware and validation. Generic work can proceed while the current cutscene
pacing issue is fixed; enhancement qualification needs a correct baseline.

## Current plans and retained references

Launcher implementation evidence and remaining release gates are consolidated
in [its status record](../launcher-next/MIGRATION_STATUS.md). Product follow-ups
live in [design direction](../launcher-next/DESIGN_DIRECTION.md#planned-product-follow-ups),
and active host/provider work stays in [integration](../launcher-next/INTEGRATION.md).
Superseded conversion checklists and retired Qt packaging instructions have
been removed; current packaging is in the [launcher guide](../launcher-next/README.md#distribution-and-self-updating).

The [root documentation table](../README.md#documentation) links the maintained
subsystem references, build instructions and launcher integration documents.
Version numbers such as Memory V2 or GPU V1 identify document/subsystem
generations; they do not by themselves mean a document is obsolete.

The [AC6 stall investigation](runtime/AC6_RUNTIME_INVESTIGATION.md),
[restructure findings](development/RESTRUCTURE_FINDINGS.md) and
[graphics validation log](graphics/VALIDATION.md) retain earlier symptoms,
architecture observations and measured results. Read their historical scope
before treating a failure or test count as current. Older analysis-pass
documents record the work performed in that pass, rather than today's
compatibility verdict.

The [coverage dashboard](coverage/README.md) measures audited source operations,
not game playability. Current CI results come from the workflows linked in the
root README. Keep dated evidence intact and update current summaries when new
runtime observations supersede it.
