# Notification center

`NotificationCenterFeature` is the durable launcher-side history behind transient QML toasts.

It stores bounded, privacy-safe notification records under the launcher's application-data directory and exposes severity, source, read state, timestamps, duplicate counts, and optional command-palette actions. The QML notification drawer renders this model but does not own persistence or action routing.

## Contract

Each entry can contain:

- `id`
- `title`
- `message`
- `severity`: `info`, `success`, `warning`, or `error`
- `source`
- `timestamp` (UTC ISO-8601)
- `read`
- `repeatCount`
- optional `commandId`, `targetId`, `sectionId`, and `actionLabel`

Identical notifications arriving within 30 seconds are collapsed into the newest entry and increment `repeatCount` instead of spamming the history. The store retains the newest 120 entries.

Notification actions deliberately reuse `CommandPaletteFeature::execute()` so navigation/update/support behavior has one backend implementation.
