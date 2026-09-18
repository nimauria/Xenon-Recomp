# Xenon Filesystem v1

Filesystem v1 establishes the host-independent VFS boundary used by future
Xbox kernel/XAM file APIs and by game modules. It is intentionally independent
of Xenon Memory and the graphics backends.

## Design rules

- Xenon owns reusable Xbox/runtime filesystem behaviour; game-specific paths,
  content layouts, patches, and validation rules belong to the game module.
- The VFS works on host `std::span` buffers and host strings. Guest pointers are
  not accepted here. The later Xbox API layer is responsible for marshalling
  guest structures through Xenon Memory.
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

## Generation 1 scope

Implemented in this first filesystem generation:

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

## Deliberately deferred

The following belong to later filesystem/runtime generations rather than this
Memory-independent foundation:

- `NtCreateFile`, `NtReadFile`, `NtWriteFile`, and other Xbox kernel exports;
- guest pointer/structure marshalling;
- kernel file objects and handles;
- overlapped/asynchronous I/O and completion ports;
- Xbox wildcard/query-directory structure packing;
- STFS/GDFX/ISO container devices;
- profile/content-manager policy and save-container semantics;
- title-specific DLC/update discovery.

## Research references

The architecture was cross-checked against Xenia's VFS/device model and the
ReXGlue VFS used by current static-recompilation projects. Unleashed Recompiled
and AC6_recomp were also treated as practical title-integration references.
Xenon keeps its own implementation and follows its project rule that the shared
runtime must not depend on any one game.
