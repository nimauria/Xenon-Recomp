# Launcher Frontend Backend

This directory owns **launcher behaviour used by QML**. It is the application layer between the
presentation-only QML files and the shared launcher services/Xenon runtime boundary.

```text
QML presentation
    |
    v
LauncherBridge                Qt/QML transport facade only
    |
    v
FrontendBackend               feature composition / workflow layer
    |
    +-- application/          system appearance hints and page state
    +-- branding/             runtime-coloured launcher brand assets
    +-- community/            support links + optional Discord Rich Presence
    +-- diagnostics/          user/developer environment summaries
    +-- filesystem/           local folders, external URLs and clipboard actions
    +-- import_export/        import/export workflow coordinator
    +-- launch/               UI launch workflow and launch-contract handoff
    +-- library/              game/library workflows, properties and DLC projection
    +-- modules/              module workflows, manifests, settings and catalog view
    +-- paths/                configured launcher path workflows
    +-- profiles/             profile workflows, fixtures and avatar handling
    +-- runtime/              runtime state/capability projection
    +-- settings/             validated settings schema + appearance/theme engine
    +-- updates/              launcher/module update workflow and provider seam
    |
    v
LauncherCore services         persistence/domain primitives
    |
    v
IRuntimeBridge                only runtime-facing application seam
    |
    v
Xenon framework/runtime
```

## Boundary rule

QML is presentation. It may own ephemeral view state such as selection, search text, dialog dirty
state, animation state and layout calculations. It must not own authoritative behaviour such as:

- persistence or serialisation;
- filesystem mutation;
- package staging/install workflows;
- profile, library or module business validation;
- launcher update state or update package handling;
- runtime capability interpretation;
- game launch-contract construction;
- module manifest parsing or module setting persistence;
- test/preview fixture generation;
- import/export file formats;
- platform diagnostics or OS integration.

If a page needs new behaviour, add or extend the corresponding feature slice here and expose the
smallest required operation through `LauncherBridge`. Do not implement the behaviour directly in
QML.

## Layer responsibilities

`FrontendBackend` composes the feature slices and coordinates cross-feature workflows, for example
moving profile storage when the configured Profiles path changes.

The feature slices are allowed to use the generic services in `launcher/src/services`. Those
services remain reusable domain/persistence primitives and should not gain QML concepts.

`IRuntimeBridge` is the launch/session seam into live Xenon runtime functionality. Xbox game/DLC
identification uses the separate `IContentProbe` seam composed by Launcher Core. GPU, memory, CPU,
filesystem/content, input, audio and network implementation details must stay below those explicit
adapters.

The Library slice is further divided into `library/`, `library/properties/` and `library/dlc/`. QML
consumes their projections and actions only; manifest parsing, DLC folder ownership and verification
remain in Core services. See `library/README.md`.

## Adding a feature

For a new launcher feature, create a dedicated directory:

```text
frontend_backend/<feature>/
  <feature>_feature.hpp
  <feature>_feature.cpp
```

The feature owns the workflow, validation/state projection and service/runtime coordination. Add it
to `FrontendBackend`, then expose only the UI-facing operation/result through `LauncherBridge`.

This keeps future Xenon framework work independent from the QML implementation and lets the UI be
replaced without rewriting launcher behaviour.
