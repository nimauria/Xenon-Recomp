# Legacy property audit

29 October 2026. All 32 `Q_PROPERTY` declarations in the locally retained `launcher/src/launcher_bridge.hpp` are listed below. This records their destination separately from the 183 invokable methods. A destination is not proof that every platform or interaction case has passed.

| Legacy property | Destination and current evidence | Disposition / remaining gate |
|---|---|---|
| version | Compiled native `application.version`, About/Support and `launcherUpdate.installedVersion` | Converted; native and desktop build-info checks |
| backendConnected | Native transport initialisation, snapshot `mode`, connection/recovery UI | Converted; desktop IPC/recovery smoke checks |
| runtimeStatus | Snapshot runtime capability and session status; SessionPanel | Adapter seam; real Xenon later |
| testMode | Explicit session/provider `fixture` flags; production never enables them automatically | Converted; native and desktop lifecycle checks |
| safeMode | `recovery.safeMode`, startup argument and restart controls | Converted; restart/crash checks |
| recoveryState | `recovery.previousUncleanExit`, startup fallback and recovery UI | Partial; repeated unclean-exit counter policy remains |
| themeId | `ui/theme`, converted appearance catalogue and theme resolver | Converted; palette/asset smoke checks |
| accentId | `ui/accent`, appearance catalogue and CSS variables | Converted; appearance tests |
| cornerStyle | `ui/cornerStyle` and original corner catalogue | Converted; appearance tests |
| customAccentColor | `ui/customAccent`, native hex validation and renderer tokens | Converted; appearance/native settings checks |
| profileName | Active profile IDs, native CRUD/selector and a pinned Unicode identity shared with preview and migration | Converted; Unicode create/edit/copy/import collision and inline validation checks |
| profileNameLimit | `contracts/profiles.json` limit 48, native/editor validation | Converted; profile tests |
| profileDescriptionLimit | Same catalogue, limit 180 | Converted; profile tests |
| systemDark | CEF `prefers-color-scheme` query and change listener in App/AppearanceSettings | Converted; desktop theme emulation; live OS matrix remains |
| systemHighContrast | CEF `forced-colors` query/listener, appearance resolver and CSS | Converted; desktop emulation; real screen-reader/OS matrix remains |
| currentSession | Snapshot `session` and separate runtime adapter | Adapter seam; fixture launch/stop/reattach tested |
| sessionState | `session.state` and persistent session panel | Adapter seam; live provider later |
| communityInfo | Community description and HTTPS project/Discord actions in Settings | Partial; consolidate remaining legacy metadata/diagnostics |
| discordPresenceState | Disabled/unavailable provider UI and native rejection of dormant opt-in | Partial; optional legacy SDK provider is not ported |
| notificationUnreadCount | Native notification records, derived unread inbox count and badges | Converted; shell smoke checks |
| hostArchitecture | Native `application.architecture`, About and Support diagnostics | Converted; compile-target architecture checked in native/desktop fixtures |
| platformName | Native `application.platform`, About, Support and package metadata | Converted; Windows exercised; other platform builds remain global gates |
| qtVersion | No Qt exists in this launcher | Retired; do not fabricate a Qt version |
| availableGraphicsBackends | Converted graphics preference choices; runtime capability remains unavailable | Adapter seam; preference options are not a claim of compiled Xenon backend availability |
| appDataPath | Snapshot `storagePath`, stable-ID Open launcher data action | Converted; native folder opening policy and isolated state checks |
| configPath | `locations.fixed` shows the real atomic `state.json`; stable location opening | Converted; launcher data relocation uses startup `--data-dir`, with no automatic file move |
| defaultGameLibraryPath | `locations.destinations` defaults/configured/effective paths; common native resolver for imports and launch | Converted; defaults/reset, original preservation and managed-transfer checks |
| defaultSaveDataPath | Same resolver; global destination and absolute profile save override in launch configuration | Converted; native/profile launch-config checks |
| defaultProfilesPath | Actual `state.json` and avatar locations shown; Open profile storage opens the real data folder | Converted with deliberate consolidated storage model; old profile-folder setting retained visibly but unused |
| defaultModulesPath | Default/custom/reset destination shared by install and discovery | Converted; installation/discovery and path checks; existing owned roots stay recorded after destination changes |
| defaultScreenshotsPath | One resolver for Settings, capture scan/resources/open/delete and launch configuration | Converted; blank/custom/reset image decoding and profile/global launch precedence exercised |
| cachePath | Queued verified copy of managed artwork/downloads, atomic reference switch, cancel/retry/reset/recovery; prior copies retained; actual paths shown | Converted for managed cache; browser cache follows launcher data. Wider permission/disk/drive-failure acceptance remains global |

The path rows deliberately distinguish legacy settings retained for migration from working relocation behaviour. Native file-picker interaction, platform builds, accessibility and performance remain shared gates in [MIGRATION_STATUS.md](MIGRATION_STATUS.md). No property is counted complete merely because its old value survives in legacy metadata.
