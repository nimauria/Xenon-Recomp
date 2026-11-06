# Launcher performance

## Process startup, 6 November 2026

Final branded Windows package: `build/packages/2f6635359cea43e5bb4e2929c4221932`. Sequential rerun after desktop checks finished, same hardware/method below, five launches per cell:

| Synthetic games | Fresh data/cache median | Reused data/cache median | Slowest sample across both |
|---:|---:|---:|---:|
| 0 | 478.8 ms | 401.3 ms | 500.9 ms |
| 1,000 | 478.0 ms | 509.9 ms | 606.2 ms |
| 10,000 | 954.4 ms | 891.1 ms | 1,320.5 ms |

Evidence: `build/startup-benchmark-1793991576371/results.json`. The 10,000-entry median meets the provisional sub-second local-interface target, but individual launches exceed it. A preceding run overlapped desktop acceptance and produced a 6,682.9 ms outlier (`build/startup-benchmark-1793972243949`); it is retained as contention evidence, not used as the sequential baseline. Background user workloads were not controlled in either run.

### Earlier package baseline

Windows Release CEF 156, Ryzen 9 7950X3D, 32 logical processors, 63 GiB RAM, Windows 10.0.26300. Five launches per cell (30 measured launches), plus untimed priming for reused data. Packaged version 0.1.0, before the latest Updates/demo/icon refinements.

| Synthetic games | Fresh launcher data/cache median | Reused data/cache median | Slowest sample across both |
|---:|---:|---:|---:|
| 0 | 370.1 ms | 326.5 ms | 650.5 ms |
| 1,000 | 415.0 ms | 433.8 ms | 734.5 ms |
| 10,000 | 723.3 ms | 696.6 ms | 870.6 ms |

`npm run benchmark:startup` measures child-process spawn to a freshly appended native interface-ready marker, polling every 5 ms. It then verifies visible navigation and the expected game count through native IPC. Fresh data means a new launcher/CEF cache, not a flushed operating-system disk cache. Fixtures have no individual artwork. This does not measure display presentation, physical input, complete artwork loading or online content. Results are indicative small samples on this machine. Evidence: `build/startup-benchmark-1793969431111/results.json`; `XENON_TEST_DESKTOP` can select a packaged payload.

For the requested public Steam reference, [PC Gamer's own launcher comparison](https://www.pcgamer.com/hardware/epic-said-its-game-launcher-was-faster-than-steam-on-the-same-day-steam-got-a-speed-boost-so-ive-put-both-to-the-test/) reported a 9.32-second average across three Steam storefront tests, including images. Its machine and workload differ from Xenon's offline local interface. This is context, not evidence of a relative speedup. No local Steam benchmark was run. Keep a provisional sub-second local-interface target while measuring artwork-heavy and lower-end systems separately.

## Initial library interaction baseline

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

Remaining acceptance: OS-cold startup, total process memory, lower-end hardware, large artwork libraries, long-session behaviour, sustained scroll/frame distributions, physical input/controller latency, 200% scaling, Linux/macOS performance and broader filesystem/network fault scenarios. Full 10,000-entry snapshots remain above 100 ms; investigate incremental updates or other measured improvements before setting a snapshot budget. The initial local-feedback target is not a claim that all performance gates have passed.
