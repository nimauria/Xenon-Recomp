# Guest Timebase / SPR Audit — AC6 Runtime Readiness pass, Part 6 follow-up

While auditing Part 13 (exception/setjmp-longjmp) and building Part 17's
capability verdict, reading `src/core/session.cpp`'s `RuntimeServices`
overrides surfaced a real, load-bearing bug cluster in the guest PPC
time-base / SPR path - squarely Part 6's ("Guest timebase/vblank/timers")
territory, which this pass had not yet revisited. Documented and fixed
here rather than left as a "noticed but out of scope" footnote, since it
is a genuine correctness bug on a hot, real production path (every
`mftb`/`mftbu` in both the AOT and Gen 7 dynamic-fallback execution paths
routes through it), not a cosmetic gap.

## The bug

`XenonSession::read_time_base()` returned `time_base_counter_++` - an
arbitrary value incremented by exactly 1 per call, with no relationship to
real elapsed time at all. Any guest code computing elapsed time from two
time-base reads (frame pacing, physics timestep, animation timing - a very
common pattern in Xbox 360 titles, which read the PPC time-base register
directly for this rather than going through a kernel call) would get
completely wrong deltas: the "elapsed time" would just be "how many times
the guest happened to read the time base," never actual wall-clock time.

Separately, `TimeServices::performance_frequency()` (what the real,
already-registered `KeQueryPerformanceFrequency` xboxkrnl export reports to
guest code) returned `1000000000` (a nanosecond-scale constant), which is
not even the same unit family as the broken counter above. Real Xbox 360
guest code computes elapsed real time as `(time_base_delta) /
KeQueryPerformanceFrequency()` - the time-base register and the reported
frequency are one coherent clock domain on real hardware, not two
independently-chosen numbers. `TimeServices::performance_counter()` -
which nothing in production actually called - already existed with the
right *shape* (host-clock-derived) but the wrong *unit* (raw nanoseconds,
not scaled to any Xbox-visible tick rate).

## What the real hardware value is, and how it was verified

The Xbox 360 CPU's PPC time-base register runs at a fixed **50 MHz**,
independent of CPU clock scaling. This is not guessed: verified directly
against this project's primary Xbox 360 semantic reference,
xenia-project/xenia (`src/xenia/emulator.cc`), which calls
`Clock::set_guest_tick_frequency(50000000)` with the comment "360 uses a
50MHz clock". Per this project's own hard-learned lesson about
plausible-but-wrong constants (see the RtlImageXexHeaderField/0x12B
incident referenced elsewhere in this codebase), this was fetched and
confirmed from the actual reference source rather than relied on from
memory alone.

## The fix

- `include/xenon/kernel/time.hpp`: added
  `TimeServices::kGuestTimeBaseFrequencyHz = 50000000ULL`, documented with
  the verification above.
- `src/kernel/timing/time.cpp`: `performance_frequency()` now returns this
  constant. `performance_counter()` now derives from `steady_clock`
  (genuinely monotonic, unlike `high_resolution_clock`, which carries no
  such guarantee and may alias `system_clock` on some implementations - a
  real time-base register can never go backward) and scales host
  nanoseconds down to real 50 MHz ticks.
- `src/core/session.cpp`: `read_time_base()` now returns
  `TimeServices::performance_counter()` directly, in the same unit
  `KeQueryPerformanceFrequency` reports. The now-dead `time_base_counter_`
  member was deleted rather than left behind.

## A second, smaller gap in the same neighborhood: silent SPR access

`read_spr()`/`write_spr()` (reached for any SPR outside `mfspr`/`mtspr`'s
own hard-coded `xer`/`lr`/`ctr`/`vrsave`/`pvr`/time-base fast paths in
`dynamic_fallback.cpp`) unconditionally no-opped: `read_spr()` always
returned `0` and `write_spr()` did nothing, both completely silently. This
matches CLAUDE.md's explicit "no silent no-op" pattern this pass has
already remediated elsewhere (Part 7's GPU unknown-packet/register
counters, Part 14's fallback accounting). No attempt was made to model
per-SPR semantics here (there is no concrete evidence of which specific
SPRs AC6 or any other title actually needs modeled, and inventing
semantics for the full SPR space would be exactly the kind of speculative,
unjustified subsystem this pass avoids elsewhere) - instead, both paths are
now accounted (`XenonSession::unsupported_spr_reads_`/
`unsupported_spr_writes_`, atomics since every guest thread shares one
`RuntimeServices`) and logged at `Level::Debug`, surfaced as
`unsupportedSprReads`/`unsupportedSprWrites` in `capability_report()`'s
`"fallback"` section, and folded into Part 17's verdict as
`PASS_WITH_FALLBACK` reasons. A title that actually depends on an
unmodeled SPR now shows up as a real, diagnosable gap instead of a silent
wrong-or-lucky zero.

## Tests added

- `tests/xbox/time_export_tests.cpp`:
  `test_ke_query_performance_frequency()` now asserts the real value
  (`50000000`), not just a tautological self-comparison; new
  `test_performance_counter_is_monotonic_and_tracks_real_time()` sleeps
  50ms and asserts the counter is monotonic and its delta reflects real
  elapsed time at the real 50MHz rate.
- `tests/core/thread_creation_tests.cpp`:
  `test_read_time_base_tracks_real_elapsed_time_and_spr_access_is_accounted()`
  dispatches a real guest thread through the actual production
  `ExCreateThread` -> `RuntimeServices` path (the same interface real
  AOT-compiled `Op::ReadTimeBase`/`Op::ReadSPR`/`Op::WriteSPR` code uses),
  calls `read_time_base()` twice across a real 50ms sleep, and asserts the
  delta is real-time-consistent; separately exercises an intentionally
  unmodeled SPR (999) through both `read_spr()` and `write_spr()` and
  asserts the real counters/verdict reasons reflect it.

## Tests run

- `xenon_xboxkrnl_time_export_tests`: all pass, including the two new
  timebase tests.
- `xenon_thread_creation_tests`: all pass, including the new
  timebase/SPR-accounting test.
- `xenon_session_tests`: re-run to confirm Part 17's PASS/
  PASS_WITH_FALLBACK/FAIL verdict tests are unaffected by the new SPR
  counters (a freshly initialized session still reports zero on both).
- Full solution rebuild + full test suite: see this pass's finish report.

## Remaining gaps

Per-SPR semantics remain unmodeled (by design - see above). No vblank
timer implementation was added or audited in this follow-up; that remains
open Part 6 scope if revisited separately.
