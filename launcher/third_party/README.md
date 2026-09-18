# Launcher third-party dependencies

This directory is for optional binary SDKs used by the launcher. Dependency payloads are not
committed to Xenon.

## Discord Social SDK

Download the **C++ Discord Social SDK** from the Discord Developer Portal for the Xenon application,
then either:

```powershell
.\\launcher\\scripts\\install-discord-sdk.ps1 -Archive C:\\path\\to\\discord_social_sdk.zip
```

or point the build directly at an extracted SDK:

```powershell
.\\launcher\\scripts\\build-launcher.ps1 `
  -DiscordSdkRoot C:\\path\\to\\discord_social_sdk `
  -RequireDiscordSdk `
  -Deploy -Run
```

The installer places the package at:

```text
launcher/third_party/discord_social_sdk/
├── include/discordpp.h
├── lib/release/discord_partner_sdk.lib
└── bin/release/discord_partner_sdk.dll
```

The normal launcher build auto-detects that location. `discord_partner_sdk.dll` is copied next to
`xenon_launcher.exe` after linking, so no manual runtime copy is required.

The SDK directory is intentionally ignored by Git. Keep Discord's downloaded license/notices with
the local package and follow Discord's distribution terms when publishing launcher builds.
