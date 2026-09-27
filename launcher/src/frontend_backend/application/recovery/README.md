# Launcher recovery

The recovery slice protects the launcher from startup crash loops without teaching QML how launcher state is stored.

`RecoveryService` owns the process run marker and persistent recovery metadata. `main.cpp` calls its static bootstrap hook before startup logging is truncated. If the previous run marker still exists, Xenon treats that run as unclean, preserves the previous startup log under the local `recovery/` directory, increments the consecutive-failure counter, and exposes the incident through `RecoveryFeature`.

Safe Mode can be requested with `--safe-mode`. Xenon enters it automatically after an unclean shutdown that occurred before the launcher reached its interactive phase, or after two consecutive unclean interactive starts. This prevents startup crash loops without forcing every ordinary post-startup termination into Safe Mode. A deliberate restart is marked clean before the replacement process is launched, so it is not mistaken for another crash.

Safe Mode deliberately avoids production profile/library/module state loading, runtime service connection, automatic update checks, theme backdrops, and game launching. Test mode keeps fixture data available so recovery UI can be exercised without production content.

The current run moves through bootstrap phases (`bootstrap`, `logging-ready`, `backend-constructing`, `backend-ready`, `qml-loading`, `qml-root-ready`, `interactive`). The phase is stored in the run marker so an unclean launch reports the last known startup boundary.

The UI only consumes recovery state and actions through `LauncherBridge`. It must not read or mutate marker/state files directly.

For launcher UI testing the PowerShell build helper accepts `-SafeMode` together with `-Run`, which passes `--safe-mode` to the executable.
