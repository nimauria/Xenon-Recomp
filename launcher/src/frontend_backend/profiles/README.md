# Profiles frontend backend

Profiles are a launcher-owned vertical slice. QML renders and edits profile data, but profile identity, validation, persistence, derived state, actions, runtime overrides, import/export and managed storage are owned by Launcher Core / FrontendBackend.

## Ownership

`ProfilesFeature` is the UI-facing profile coordinator. It decorates persisted profiles with derived launcher state such as effective content paths, managed profile storage, library/module/save counts and resolved runtime settings.

`ProfileService` owns durable profile records and the versioned `.xenonprofile` import/export format.

`actions/ProfileActionCatalog` is the authoritative action policy used by both right-click menus and the visible More menu. QML must not duplicate rules such as whether an active profile can be deleted.

`runtime/ProfileRuntimeCatalog` defines the subset of launcher runtime settings that a profile may override. Values reuse the Settings schema for normalization and are additionally checked against runtime capabilities before persistence.

## Profile runtime inheritance

When `isolatedSettings` is false, the profile inherits launcher-wide runtime, graphics, input and audio defaults.

When `isolatedSettings` is true, `runtimeOverrides` may contain the supported profile keys. `LaunchService` resolves those values into the same typed `LaunchConfiguration` fields used by normal launcher settings, so the Xenon runtime receives one contract regardless of where a value originated.

The current override set covers renderer, shader-cache policy, preferred input family, deadzone, rumble, master volume, mute-on-unfocus and audio latency policy.

## Startup page

New profiles use `Launcher default`, which defers to Settings > General. A profile may explicitly override this with Home, Library, Modules, Profiles or Settings. Existing profiles that already store an explicit page retain that choice.

## Paths and storage

A profile may override Games, Saves and Screenshots. Empty overrides inherit Settings > Paths. The managed profile-storage directory is `<Profiles>/<profile-id>/`; avatar files and future profile-owned launcher metadata belong there.

Opening/deleting managed profile data must go through backend features rather than direct QML filesystem operations.

## Context actions

Profile list rows and the selected-profile header expose the same backend-generated action list on right click. The visible More button consumes that same list. Actions currently include activation, profile editing, runtime overrides, content locations, duplication, export, opening managed storage, removing a profile image and deletion where permitted.

`XActionMenu` supports disabled actions and backend-provided disabled reasons. This is intended to be reused by Library, Modules and other launcher pages as their context-menu passes are implemented.

## UI boundary

QML may own selection, search text, dialog dirty state, layout and visual presentation. It must not generate profile IDs, validate runtime capability support, decide deletion policy, resolve inherited paths/settings, manipulate profile storage, or implement import/export itself.
