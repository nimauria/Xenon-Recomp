# Xenon Filesystem v1

Filesystem v1 establishes the host-independent VFS boundary used by future
Xbox kernel/XAM file APIs and by game modules. It is intentionally independent
of Xenon Memory, CPU execution internals, and the graphics backends.

## Design rules

- Xenon owns reusable Xbox/runtime filesystem behaviour; game-specific paths,
  content layouts, patches, and validation rules belong to the game module.
- The VFS works on host buffers and host strings. Guest pointers are not
  accepted here. The later Xbox API layer is responsible for marshalling guest
  structures through Xenon Memory.
- Guest paths use Xbox-style `\\` separators and are matched case-insensitively.
- Mount resolution chooses the longest matching device path, avoiding implicit
  registration-order dependencies for overlapping devices.
- Symbolic aliases such as `game:` and `d:` are separate from device mounts and
  may chain recursively with a bounded expansion depth.
- Relative paths resolve beneath a configurable working directory (`game:` by
  default) because real recompiled titles may use both rooted and relative I/O.
- Host directory devices reject `..` traversal and canonical paths escaping the
  configured mount root. This includes symlink/junction escapes discovered
  during resolution.
- Read-only device policy is enforced in the device, not left to the host OS.
- Directory enumeration is deterministically sorted so behaviour does not
  depend on host filesystem iteration order.
- Xbox-facing create/open/share semantics are represented in Xenon rather than
  delegated blindly to whichever sharing behaviour the host C runtime chooses.

## Generation 1 scope

Generation 1 established the baseline:

- guest path normalization and comparison;
- VFS device registration/unregistration;
- symbolic-link registration and bounded recursive expansion;
- configurable relative-path working directory;
- longest-prefix device routing;
- host-directory device with case-insensitive lookup on case-sensitive hosts;
- sandboxed host path resolution;
- read-only mounts;
- file open/create dispositions;
- sequential and positional reads/writes;
- seek, size, resize, and flush;
- file/directory stat;
- directory creation and enumeration;
- rename and removal;
- disk-space reporting;
- focused filesystem unit tests.

## Generation 2 scope

Generation 2 makes the VFS a stronger substrate for the later `xboxkrnl` file
bridge while remaining fully independent of Memory v2 and CPU v2:

- case-insensitive `*` / `?` guest wildcard matching;
- filtered directory queries with file/directory selection and result limits;
- Xbox/NT-style open outcomes (`Created`, `Opened`, `Superseded`,
  `Overwritten`);
- explicit read/write/delete share-access flags;
- live-handle share-conflict detection returning `SharingViolation`;
- delete/rename checks that respect existing handles' delete-sharing policy;
- richer file metadata with Xbox-style attribute bits and allocation size;
- VFS-level disk-space querying;
- mount and symbolic-link introspection for runtime diagnostics;
- a `NullDevice` for intentionally empty/read-only optional device paths;
- sanitizer regression coverage for the new query/share machinery.

The host backend keeps share bookkeeping in Xenon-owned state so Linux and
Windows do not silently expose different guest semantics solely because their
native file-sharing rules differ.

## Generation 3 scope

Generation 3 establishes the immutable-content/device boundary needed for disc
and container-backed titles while still remaining independent of Memory v2 and
CPU v2:

- a format-neutral `ReadOnlyContentSource` interface for stat/list/random-read
  access to immutable Xbox content;
- `ReadOnlyContentDevice`, allowing GDFX/ISO/container implementations to plug
  into the VFS without leaking format details into callers;
- read-only file handles with seek, sequential reads, and positional reads;
- resumable `DirectoryCursor` snapshots with paging, restart, position, and
  exhaustion state for future `NtQueryDirectoryFile` file objects;
- DOS/NT wildcard groundwork, including `*.*`, `name.*`, `DOS_STAR`, `DOS_QM`,
  and `DOS_DOT` semantics in the host-independent guest matcher;
- VFS/device metadata mutation APIs for attributes and last-write time;
- portable host read-only attribute updates and last-write timestamp updates;
- read-only content capacity reporting with zero writable/free guest space;
- regression tests for cursor paging/restart, DOS wildcard cases, metadata
  updates, and a synthetic disc-like content provider.

The content source API is deliberately narrow. A future GDFX implementation
only needs to map its directory table and sector-backed file extents into
`stat`, `list`, and `read_at`; existing VFS callers do not change.

### Launcher integration point

The launcher already has an `IContentProbe` seam and explicitly waits for the
Xenon filesystem/content probe. Once this generation is merged, the launcher
may link `Xenon::Filesystem` and use a thin adapter for mount/probe diagnostics
without waiting for CPU v2 or Memory v2. Actual game/disc identification should
be enabled when the next content-probe layer can recognize XEX/GDFX/STFS or a
game module's declared content signature. Runtime `Nt*` file calls remain a
separate later integration through the Xbox kernel bridge.

## Deliberately deferred

The following still belong to later filesystem/runtime generations:

- `NtCreateFile`, `NtReadFile`, `NtWriteFile`, and other Xbox kernel exports;
- guest pointer/structure marshalling;
- kernel file objects and the global kernel handle table;
- overlapped/asynchronous I/O and completion ports;
- packing Xbox `FILE_*_INFORMATION` structures into guest memory;
- exact remaining `NtQueryDirectoryFile` packing/edge cases beyond the host-side cursor;
- SVOD/multi-fragment package providers beyond single-file STFS;
- profile/content-manager policy and save-container semantics;
- title-specific DLC/update discovery.

## Research references

The architecture is cross-checked against Xenia's VFS/device and file-action
model and the ReXGlue VFS used by current static-recompilation projects.
UnleashedRecomp and AC6_recomp are treated as practical title-integration
references. Xenon keeps its own implementation and follows the project rule
that the shared runtime must not depend on any one game.

## Generation 4 scope

Generation 4 adds the first content-identification layer and connects that layer
to the launcher's existing `IContentProbe` seam without introducing a CPU or
Memory dependency:

- a bounded, metadata-only XEX2 header parser;
- XEX execution-info extraction for title ID, media ID, version/base version,
  platform, executable table, disc number/count, and savegame ID;
- optional original PE-name extraction from the XEX header;
- a host-neutral `ContentProbe` that identifies extracted/root game directories
  through `default.xex` and directly selected XEX2 files;
- explicit candidate detection for STFS signatures and disc-image paths without
  claiming those formats are parsed yet;
- rejection of symbolic-link content roots/default executables at the probe
  boundary;
- launcher `XenonContentProbe` integration when `Xenon::Filesystem` is built;
- strict module matching against module-declared Xbox title/media/version/disc
  identities;
- persistence of title ID, media ID, XEX version, disc metadata, source type and
  executable path in launcher library metadata.

The XEX parser deliberately reads only the clear XEX header required for content
identification. It does not decrypt, decompress, relocate, map, or execute the
image. Those operations remain runtime/module responsibilities and must not be
pulled into the launcher.

Generation 6 promotes single-file STFS packages from candidate-only handling to a
concrete read-only package provider. GDFX disc images moved to a concrete provider
in Generation 5.

## Generation 5 scope

Generation 5 adds a concrete read-only GDFX/Xbox 360 disc source and makes disc
images identifiable through the same content probe used for extracted games:

- 2 KiB GDFX sector addressing and Xbox media-volume signature validation;
- discovery of the known game-partition offsets used by raw/XGD-style images;
- bounded root-volume parsing and directory binary-tree traversal;
- case-insensitive stat/list/random-read access through `ReadOnlyContentSource`;
- nested directory and sector-backed file extents;
- corrupt-tree cycle detection, depth/node limits, integer-overflow guards and
  image-bound checks before any file read;
- direct mounting through the existing `ReadOnlyContentDevice`;
- `.iso` and `.xgd` game probing plus `.dvd` descriptor resolution;
- discovery and metadata parsing of root `default.xex` directly inside GDFX;
- launcher identification of GDFX content using the same module identity rules
  introduced in Generation 4.

The GDFX reader is streaming/random-access rather than mapping an entire Xbox
360 disc image into memory, so multi-gigabyte images do not require an equally
large host allocation. Disc parsing still performs no guest execution and has no
dependency on Memory v2, CPU v2, or the graphics subsystem.

## Generation 6 scope

Generation 6 adds the first concrete Xbox content-package provider and connects
the launcher's DLC import seam to validated single-file STFS packages:

- CON, LIVE and PIRS signature/header recognition;
- bounded XContent metadata parsing for content type, content ID, title/media ID,
  XEX package version, display/title names, publisher and volume descriptor;
- single-file STFS volume parsing with 4 KiB data blocks and hash-table-aware
  logical-to-physical block translation;
- chained file-table and payload-block traversal with end-of-chain, cycle,
  package-bound and allocation-count validation;
- STFS directory hierarchy reconstruction with case-insensitive stat/list/read
  through the existing `ReadOnlyContentSource` interface;
- cross-block random reads, so files do not need to be materialized before the
  runtime can consume them;
- a generic safe `materialize_content_source` helper for launcher-managed
  extraction of immutable package providers;
- launcher DLC identification by the package's exact 20-byte XContent Content
  ID, with optional title/media/version refinements supplied by the game module;
- revalidation immediately before materialization so a package changed between
  identification and install is refused;
- regression fixtures for metadata, nested directories, chained file reads,
  materialization and deliberately cyclic block chains.

STFS parsing is intentionally read-only. Generation 6 does not modify packages,
re-sign them, emulate Xbox license policy, or claim cryptographic authenticity
of user content. Header and filesystem structure are validated for safe parsing;
cryptographic signature/hash verification can be layered separately where the
runtime actually requires it.

SVOD remains separate because its fragment and Enhanced-GDF/XSF layouts differ
materially from single-file STFS. The common content-source interface means an
SVOD provider can be added later without changing the VFS, launcher library, or
DLC-service contracts.

