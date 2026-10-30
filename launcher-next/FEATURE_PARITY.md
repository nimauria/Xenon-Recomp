# Legacy feature-flag dispositions

29 October 2026. All 23 names returned by the original `ApplicationFeature::featureEnabled` are mapped here and in `web/features.ts`. A feature being present does not advertise a connected Xenon provider. The launcher exposes its own files/HTTPS capabilities separately from runtime input, filesystem, content and network capabilities.

| Legacy feature | New surface and availability |
|---|---|
| settings.general | General preferences, startup precedence and copied-data migration |
| settings.system | Native window preferences; remaining platform acceptance stays in migration status |
| settings.appearance | Converted theme/accent/corner/background catalogues and appearance controls |
| settings.library | Collection view/density/wrap/selection preferences |
| settings.paths | Actual destinations, defaults/reset, fixed profile storage and queued managed-cache relocation |
| settings.runtime | Stored runtime launch preferences and explicit launch-configuration linkage; live runtime remains a later adapter |
| settings.graphics | Stored graphics launch preferences; does not claim a compiled renderer is available |
| settings.input | Launch input preferences and provider-gated device/player controls |
| settings.audio | Launch audio preferences; real playback requires the runtime |
| settings.network | Old default was false. Fields remain disabled unless a provider advertises `networkConfiguration`; native setting changes reject unsupported configuration. Current unavailable and status-fixture providers both advertise false. The category explains this and links to network status. |
| settings.filesystem | Links to filesystem tools, whose mount/link/path operations are disabled without the filesystem provider |
| settings.updates | Native catalogue/module/launcher HTTPS updates; manual replacement outside supported packaged Windows installations |
| settings.community | Community links, with Rich Presence explicitly unavailable and dormant opt-in rejected |
| settings.accessibility | Converted text scale, contrast, motion and input preferences; broader device/screen-reader acceptance remains |
| settings.developer | Developer preferences and diagnostics link when developer mode is enabled |
| settings.about | Native compiled version, platform and architecture |
| library.dlc | Catalogue/local add-ons, owned copies/replacement/receipt checks; content identity remains unverified without a content provider |
| library.manageFiles | Owned game moves/deletion/receipts and per-disc operations, with session/job locks |
| modules.updates | Catalogue-based check/download/install/rollback and update history; online capability and job state gate controls |
| modules.catalog | Cached/bundled catalogue and explicit refresh; offline data preserved |
| modules.settings | Manifest-normalised typed settings/defaults/reset with native validation |
| profiles.paths | Profile game/save/screenshot overrides in launch configuration; profiles themselves use the consolidated data file |
| profiles.avatars | Native image import/crop metadata/remove; browser preview does not invent local files |

Network *status* and network *configuration* are distinct. The explicit status fixture can test readiness without changing real network configuration. The launcher's own registry/download HTTPS transport also remains independent. Unknown feature names return false. `features.test.ts`, `profile_cache_tests.cpp`, provider tests and `smoke-profile-cache.mjs` exercise these boundaries; the latter checks disabled configuration and navigation to the unavailable filesystem surface.

This replaces the old compile-time booleans with explicit current availability; it does not add a production runtime implementation. General page/action, platform, accessibility and performance acceptance remains in [MIGRATION_STATUS.md](MIGRATION_STATUS.md).
