# Launcher diagnostics and support

Diagnostics are owned by Launcher Core. QML renders summaries and invokes support actions; it does not collect host, runtime, module, library or session state itself.

## Support bundle

`SupportBundleService` produces a single `xenon-support-YYYYMMDD-HHMMSS.zip` suitable for attaching to a GitHub issue or Discord support post. The ZIP is written with the standard store method and has no third-party archive dependency.

The default bundle contains:

- a user-facing system summary;
- developer diagnostics and runtime capability state;
- public launcher settings except configured filesystem paths;
- sanitized library/module summaries;
- up to ten recent session outcomes/errors;
- bounded excerpts from launcher/update logs when they exist;
- a privacy note describing what is omitted/redacted.

The bundle intentionally excludes profile names, avatar paths, game/save/module/profile directories, source content paths and complete launch configurations. Known home/configured paths and email-shaped strings are redacted from included logs. Modules may still emit arbitrary text into shared logs, so the UI tells the user to review a bundle before posting it publicly.

Bundles are placed in `Downloads/Xenon Support` where available, falling back to Documents or the launcher's local diagnostics directory.
