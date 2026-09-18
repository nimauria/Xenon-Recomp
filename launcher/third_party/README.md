# Launcher third-party dependencies

This directory is reserved for optional binary SDKs used by the launcher. Dependency payloads are
not committed to Xenon.

## Discord Social SDK (future / optional)

Discord Rich Presence is currently disabled in normal Xenon builds, so **nothing needs to be placed
here** to build or use the launcher. Discord community/support links do not depend on the SDK.

The existing SDK integration is retained for future development. When Rich Presence is deliberately
enabled with `-EnableDiscordRichPresence` / `XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE=ON`, the
helper installer can stage Discord's C++ package here:

```powershell
.\launcher\scripts\install-discord-sdk.ps1 -Archive C:\path\to\discord_social_sdk.zip
```

Expected local layout:

```text
launcher/third_party/discord_social_sdk/
├── include/discordpp.h
├── lib/release/discord_partner_sdk.lib
└── bin/release/discord_partner_sdk.dll
```

The SDK directory is intentionally ignored by Git. Keep any downloaded license/notices with the
local package and follow Discord's distribution terms if Rich Presence is enabled in a published
build later.
