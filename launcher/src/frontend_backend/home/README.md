# Home / recent activity feature

`HomeFeature` owns the launcher-facing aggregation used by the Home dashboard.
It combines existing generic launcher data only: Library records, module state,
session history and notification history. QML renders the returned snapshot but
does not reconstruct or persist activity state itself.

The snapshot exposes:

- current/most-recent game;
- recently played games;
- recent session outcomes;
- recent launcher notifications;
- aggregate play/session/module counts.

No game-specific logic belongs here. Content parsing and runtime execution remain
behind their existing launcher/runtime boundaries.
