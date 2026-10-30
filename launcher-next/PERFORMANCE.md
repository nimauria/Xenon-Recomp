# Initial library performance baseline

29 October 2026. Windows Release desktop build, CEF 156, AMD Ryzen 9 7950X3D (32 logical processors), approximately 63 GiB reported RAM, Windows build 26300. These are indicative automation samples from this development machine, not full release acceptance or a promise for other hardware.

The isolated fixtures contain 0, 100, 1,000 and 10,000 synthetic entries with a shared fictional source and no artwork. Grid view and reduced motion are enabled. Normal native snapshot/persistence paths are used; there is no production benchmark mode or invented installed library.

| Entries | Search feedback p95, ms | Filter feedback p95, ms | Sort feedback p95, ms | Full native snapshot p95, ms | Renderer JS heap, MiB | Game buttons in DOM |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 32.2 | n/a | 32.3 | 1.6 | 5.5 | 0 |
| 100 | 32.3 | 32.2 | 32.2 | 2.7 | 8.8 | 35 |
| 1,000 | 32.2 | 32.2 | 32.3 | 15.4 | 13.6 | 35 |
| 10,000 | 54.9 | 46.0 | 32.3 | 141.4 | 31.2 | 35 |

Feedback is measured from an automation-dispatched DOM input/change event to two animation frames. This introduces a roughly 32 ms floor on this run; it is not physical keyboard/controller-to-display latency. Assertions verify that search results and sort order actually changed. There are 15 search, 10 filter/sort and 5 snapshot samples per fixture. The reported p95 is an order statistic from those small samples. Heap size is a single renderer JS-heap observation and excludes native/CEF process memory; garbage collection and concurrent machine load were not controlled.

The 10,000-entry fixture is about 7.8 MB of persisted JSON. Its full snapshot round-trip p95 was 270.2 ms before removing an unnecessary deep copy when wrapping native responses; the repeat was 141.4 ms. This measures the snapshot path, not overall application speed. Launch configuration also now borrows the authoritative game/profile collections instead of copying the entire library. Catalogue presentation refresh indexes module/catalogue identities once rather than repeatedly searching the catalogue for every game.

Renderer reload-to-observed-native-Library readiness was 33.5 / 38.9 / 69.2 / 470.8 ms for the four sizes. It excludes native process/CEF cold startup and includes automation observation delay. It must not be presented as cold startup time.

The reproducible runner is `scripts/benchmark-library.mjs`. `node scripts/benchmark-library.mjs prepare` creates a new root below `build/` and prints its location. For each directory in that root's `fixtures.json`, start the desktop host with `--offline --remote-debugging-port=9223 --data-dir="<fixture directory>"`, then run `node scripts/benchmark-library.mjs measure "<fixture directory>"`. The runner verifies its fixture marker, writes `result.json` and closes that test window. Run each size separately; do not point it at real launcher data. The measurements above are retained locally under `build/library-benchmark-1793290536521/`, with `baseline-result.json` preserving the pre-change samples.

Remaining acceptance: full native cold/warm startup, total process memory, lower-end hardware, large artwork libraries, long-session behaviour, sustained scroll/frame distributions, physical input/controller latency, 200% scaling, Linux/macOS and broader filesystem/network fault scenarios. Full 10,000-entry snapshots remain above 100 ms; investigate incremental updates or other measured improvements before setting a snapshot budget. The initial local-feedback target is not a claim that all performance gates have passed.
