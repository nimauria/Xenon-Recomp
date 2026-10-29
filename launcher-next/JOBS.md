# Local installation jobs

Module ZIP installation is a native background operation. `modules.installPackage` acknowledges a persisted queued record; completion is a separate state. The CEF command worker remains available while the installation worker copies, validates and extracts the package.

## Lifecycle

`queued → running → committing → completed`

Queued work can be cancelled immediately. Running work acknowledges `cancelling`, checks cancellation during file copying, ZIP-directory validation, extraction callbacks and manifest scanning, and then removes its private staging files. The short committing phase is uncancellable. Failed, cancelled and interrupted records offer an explicit retry with a fresh ID and a `retryOf` reference.

Closing the launcher cancels unfinished work and records it as interrupted. Startup also converts persisted active records to interrupted; it does not silently run them again. The original package and the previously registered module are retained. Retry reads the source package's current size and modification time, allowing a repaired or replaced ZIP to be used.

## Commit and recovery

Each job records its destination module root at enqueue time. A fresh `.xenon-staging/<job-id>` directory under that root holds an ownership marker, a private package copy and the extracted payload. ZIP checks retain the archive layer's entry, size, CRC, traversal, link and collision restrictions. Staging on the destination filesystem permits a directory rename at commit, including when the module root is on a different drive from launcher data.

Before renaming, the worker saves `committing`. The old module remains registered until the rename and final state write succeed. The final state is built from the latest authoritative state so concurrent profile/settings changes are retained. Windows sharing/access errors receive a bounded two-second retry outside the state mutex. If registration or persistence fails after the rename, the worker attempts to move its new directory back; recovery can clean unregistered output left by a process interruption.

Cleanup checks the generated directory location and exact job ownership marker. It preserves registered module paths, retained previous versions and directories whose markers differ. Cleanup failures remain visible on the operation record. Clearing finished records does not uninstall modules or delete user packages.

These mechanisms cover process interruption. Power-loss durability and clean-machine filesystem fault testing remain part of hardening.

## Interface and limits

Snapshots expose phase, progress, per-step byte counts and available actions. Private source/root/fingerprint fields stay in native state. The renderer polls while sessions, jobs or cleanup remain active, rejects snapshots older than its current revision, and keeps the operation count reachable in the shell. Renderer reloads do not recreate or stop native jobs.

The queue admits up to 32 active operations and normally retains the latest 100 records. Archive limits remain 512 MiB compressed, 2 GiB expanded and 20,000 entries. Progress persistence within each phase is throttled to five updates per second. Catalogue refreshes, module/launcher release checks and verified downloads share this queue. Downloads stream over native HTTPS with cancellation, bounded metadata/package sizes, SHA-256 verification and an explicit install step. A verified module ZIP is checked again before installation. Packaged Windows launcher replacement uses a separate verified helper, waits for parent process identity to exit and requires the replacement interface-ready receipt; failed startup restores the retained previous installation. Linux/macOS currently use manual replacement.

Legacy inspection/import jobs also use the queue. A review is based on source and target metadata fingerprints; application rechecks both, saves a backup, copies avatars to private native storage and commits the new state atomically. New launcher edits abort a stale import. Source files remain read-only. Cancellation and startup recovery clean only marked unreferenced avatar copies; backups are retained.

## Validation

`launcher_next_install_jobs` uses a controlled native progress observer to test queued/running cancellation, retry, duplicate requests, concurrent edits, failure preservation and reconstructed interrupted commit/staging records. Archive tests cover cancellation through miniz's C callback without throwing across that boundary.

`npm run test:jobs` connects to an isolated, explicitly debug-enabled desktop instance. It checks failed ZIP repair/retry, UI cancellation, unchanged user packages, previous-version preservation, a renderer reload during a 15,000-file installation, completion and history clearing. See the README for debug-instance setup.

## Managed game and add-on files

Game moves stage and hash-check a copy before committing its library reference. Only unchanged verified original files are removed; new or changed originals remain with a visible warning. DLC always copies. Multiple selected sources can belong to one declared catalogue add-on; replacement keeps its stable local ID and previous managed copy. Receipt checks validate the current copy, not retained previous copies or runtime compatibility. Deleting an add-on deletes all its owned managed copies and keeps original sources.

Ownership markers and canonical boundaries protect cleanup/deletion. Recursive work rejects links and junctions. Transfers allow up to 50,000 entries and 512 GiB; multi-source add-on imports allow 128 sources. Queue cancellation after registration keeps the usable copy, and a retry after restart resumes source cleanup rather than importing a second game. `library_jobs_tests.cpp`, `smoke-library.mjs` and `smoke-dlc-catalog.mjs` exercise these cases.

Module manifest rescan uses the same durable operation queue without requiring a host provider. It preserves settings and unreadable registrations, reports warnings and rejects results when the configured module root changes during scanning.
