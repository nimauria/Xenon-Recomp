# Community and Discord integration

The Community feature owns Xenon's public support links and a dormant optional Discord Rich Presence
integration. QML renders feature state and forwards user actions; it does not own Discord URLs,
presence policy, session-to-activity mapping or provider setup.

```text
QML Help / Settings Community
           |
           v
     LauncherBridge
           |
           v
     CommunityFeature
       /          \
external links   DiscordPresenceFeature
                      |
                      v
             IDiscordPresenceProvider
                      |
             optional Discord provider
```

## Community links

The canonical Xenon Discord invite is:

```text
https://discord.gg/zVGw3HADgA
```

Community/support links are fully functional regardless of Rich Presence support. They are opened
through `FilesystemFeature`, so external navigation policy stays outside QML.

## Rich Presence status

Rich Presence is intentionally **disabled at build time by default**. The Community page keeps the
feature visible as planned functionality, but its switch is disabled and no Discord callbacks,
SDK loading or presence publication occurs in normal builds.

This is deliberate: Xenon does not currently require developers or users to obtain/install Discord's
Social SDK merely to build the launcher.

The presence model remains implemented for later use and maps launcher/session state to activities
such as:

```text
Browsing the game library
Managing Xenon modules
Managing launcher profiles
Customising Xenon
Launching <game>
Playing <game>
Stopping <game>
```

The state line is `Powered by Xenon`. No profile names, local paths, save data or other local metadata
are included. User opt-in defaults to off even in builds that later enable the provider.

## Future enablement

The official Xenon Discord application ID remains reserved in the build configuration:

```text
1550625950570520768
```

Rich Presence can be enabled explicitly in a future/developer build with:

```text
-DXENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE=ON
```

or through the launcher build script:

```powershell
.\launcher\scripts\build-launcher.ps1 -EnableDiscordRichPresence
```

At that point the existing Social SDK discovery/link/deployment support becomes active. If the SDK
is required for a release/CI build, use `-RequireDiscordSdk` (which implies Rich Presence enablement).
The provider seam remains isolated so Discord's SDK can also be replaced later without changing the
Community UI or session controller.

Forks can override the application ID with:

```text
-DXENON_LAUNCHER_DISCORD_APPLICATION_ID=<alternate-application-id>
```
