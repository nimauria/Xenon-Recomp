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

## Deliberately deferred

The following still belong to later filesystem/runtime generations:

- `NtCreateFile`, `NtReadFile`, `NtWriteFile`, and other Xbox kernel exports;
- guest pointer/structure marshalling;
- kernel file objects and the global kernel handle table;
- overlapped/asynchronous I/O and completion ports;
- packing Xbox `FILE_*_INFORMATION` structures into guest memory;
- resumable `NtQueryDirectoryFile` cursors and exact DOS wildcard edge cases;
- GDFX/ISO and STFS/container devices;
- profile/content-manager policy and save-container semantics;
- title-specific DLC/update discovery.

## Research references

The architecture is cross-checked against Xenia's VFS/device and file-action
model and the ReXGlue VFS used by current static-recompilation projects.
UnleashedRecomp and AC6_recomp are treated as practical title-integration
references. Xenon keeps its own implementation and follows the project rule
that the shared runtime must not depend on any one game.
