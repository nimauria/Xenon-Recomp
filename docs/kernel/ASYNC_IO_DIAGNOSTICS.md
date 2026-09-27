# Filesystem / Async I/O Diagnostics — AC6 Runtime Readiness pass, Part 11

Part 11 asked to audit `game:`/`d:`/`content:`/`cache:`/save/DLC/Title
Update/profile paths, directory enumeration, file attributes, async I/O,
overlapped I/O, completion events, and file position behaviour - and to add
diagnostics for async submit, completion, guest event signal, thread, and
request sequence, without patching any specific title's own storage-job
ordering race (AC6_recomp found one in the game itself; Xenon's job is to
not introduce additional ordering violations of its own, not to work around
a title's bug).

## Audit result: the mechanism already exists and is mature

`KernelFileObject`/`KernelIoManager` (`include/xenon/kernel/file_object.hpp`,
`io_manager.hpp`) already provide a real async I/O model: per-request
tracking (`IoRequest`, monotonic sequence via `next_request_id_`),
completion-port association, cancellation on handle close
(`cancel_all_requests()`), and the full path set (`game:`/`d:`/`content:`/
`cache:`, save/profile/DLC/Title-Update mounts via `ContentManager`/
`TitleUpdateManager`/`DlcManager`) is exercised by existing tests
(`tests/kernel/kernel_io_tests.cpp`, `xbox_guest_io_tests.cpp`). No ordering
bug was found in this layer during the audit - `begin_request()`/
`complete_request()` are correctly serialized under `request_mutex_`, and
nothing in this pass changed that locking.

## What this pass added: submit/completion were not independently observable

`xenon::logging::Logger` (Phase 0) had zero call sites anywhere under
`src/kernel/**` before this pass, matching the same gap Part 7 found and
fixed in `src/graphics/**`. `begin_request()` and `complete_request()` now
each log one `"io"`-category entry (`Level::Debug`) carrying exactly what
Part 11 asked for: the request's sequence number (`IoRequest::id()`), its
operation (`Read`/`Write`/`Flush`/...), and a stable tag for the *host*
thread that submitted or completed it. This is deliberately the host
thread, not a guest thread id - `KernelFileObject` has no guest execution
context of its own - but it is exactly enough to notice a request completed
on a different host thread than the one that submitted it, which is the
general shape of ordering issue this Part is about, without encoding
anything about AC6's specific race.

`xenon_kernel` gained a link dependency on the `xenon_logging` library
introduced in Part 7 (previously only `xenon_core`/`xenon_graphics` needed
it).

## Known, tracked gap

Guest event signalling (the third item Part 11's diagnostics list asks
for - i.e. logging when a guest-visible event object tied to an async
request actually gets signalled) is not covered by this pass: that
signalling happens at the `xbox_io`/`KernelEvent` layer, one level above
`KernelFileObject`, and was not audited in this pass. `begin_request()`/
`complete_request()`'s new logging captures the I/O-manager-level submit/
completion; a caller that wants to correlate that with guest event-signal
timing would need a second log site at that higher layer.

## Tests

- `tests/kernel/kernel_io_tests.cpp`:
  `test_async_request_submit_and_completion_are_logged` installs a
  `Logger` sink and asserts both a submit and a completion entry are
  emitted, tagged with the exact request sequence number, for a real
  `KernelIoManager`-driven async request.
