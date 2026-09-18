# Community and Discord integration

The Community feature owns public Xenon support links and optional Discord Rich Presence. QML only
renders the feature state and forwards user actions.

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
             Discord Social SDK
```

## Community links

The canonical Xenon Discord invite is:

```text
https://discord.gg/zVGw3HADgA
```

`CommunityFeature` opens support/project links through the existing `FilesystemFeature`, so QML does
not own or duplicate external-navigation policy.

## Rich Presence policy

Rich Presence is optional and disabled by default. Users can independently choose whether game titles
are included. The launcher never publishes profile names, local paths, save data or other local
metadata.

The normalized activity currently exposes launcher/session state such as:

```text
Browsing the game library
Managing Xenon modules
Managing launcher profiles
Customising Xenon
Launching <game>
Playing <game>
Stopping <game>
```

The state line is `Powered by Xenon`, and the activity supplies buttons for the Xenon Discord and
Project Xenon repository.

Session state is sourced from `SessionController`; QML does not decide which game is published.

## Discord Social SDK provider

The provider is optional at build time. Without the SDK or a Discord application ID, the Community
page still works and shows the desired Rich Presence preview, while the provider reports that setup
is required.

The official Xenon launcher Discord application ID is built in by default:

```text
1550625950570520768
```

Forks or alternate branded builds may override it with:

```text
-DXENON_LAUNCHER_DISCORD_APPLICATION_ID=<alternate-application-id>
```

To enable the live desktop provider, download the standalone **C++ Discord Social SDK** for the
Xenon application from the Discord Developer Portal. Discord distributes the SDK package through the
application's Social SDK downloads page; Xenon intentionally does not vendor that package.

The easiest Windows setup is:

```powershell
.\launcher\scripts\install-discord-sdk.ps1 -Archive C:\path\to\discord_social_sdk.zip
.\launcher\scripts\build-launcher.ps1 -Deploy -Run -RequireDiscordSdk
```

The installer copies the package to `launcher/third_party/discord_social_sdk`, which is ignored by
Git. CMake auto-detects the official package layout:

```text
include/discordpp.h
lib/release/discord_partner_sdk.lib
bin/release/discord_partner_sdk.dll
```

The DLL is copied next to `xenon_launcher.exe` automatically after a successful build. An extracted
SDK outside the repository can instead be selected with either:

```text
-DXENON_DISCORD_SOCIAL_SDK_ROOT=<sdk-root>
```

or the `DISCORD_SOCIAL_SDK_ROOT` environment variable. The lower-level include/library/runtime cache
variables remain available as escape hatches for unusual package layouts.

The provider uses Discord's Social SDK `Client::SetApplicationId`, `UpdateRichPresence`,
`ClearRichPresence` and periodic `RunCallbacks()`. On desktop, Discord explicitly supports publishing
Rich Presence before `Client::Connect`, so Xenon does not require OAuth/account linking merely to
publish activity to the signed-in local Discord client. Social/account features can be added later
behind a separate permission/authentication flow.

The Discord Developer Portal application name controls Discord's first activity line. Name the
application `Xenon Launcher` (or the final public product name) if that is what should appear there.

If a custom art asset is added later, it should be provider configuration rather than a QML concern.
