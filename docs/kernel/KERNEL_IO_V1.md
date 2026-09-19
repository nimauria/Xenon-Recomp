# Xenon kernel I/O object layer

Generation 7 introduced the first host-side Xbox/NT-style kernel object boundary
above `Xenon::Filesystem`. Generation 8 extends it with file-information semantics,
root-directory-relative opens, rename state, completion ports and events. It still
deliberately does **not** marshal guest pointers or implement `Nt*` exports; those
remain a thin ABI bridge once Memory v2 is ready.

## Layering

```text
future xboxkrnl Nt* exports
          |
          v
     KernelIoManager
          |
   HandleTable / KernelFileObject
          |
          v
     Xenon::Filesystem
          |
          v
   VFS devices and content sources
```

`Xenon::Kernel` depends on `Xenon::Filesystem`, but the filesystem does not know
about kernel handles, guest memory, APCs, events, or completion ports. Event and
completion objects live entirely in the kernel layer.

## Object and handle semantics

A `KernelFileObject` represents one open file object. Every independent open
creates a new object with its own current byte offset and directory scan state.
Duplicating a handle creates another handle to the **same** object, so duplicated
handles share the byte offset, directory cursor, delete-pending state, and active
I/O requests.

The generic `HandleTable` stores:

- an opaque 32-bit handle,
- a shared kernel object reference,
- the access granted to that handle,
- inherit/protect-from-close flags.

Duplicate access may be equal to or narrower than the source handle, never
broader. Closing the final handle invokes the object's last-handle-close hook
even if a temporary host `shared_ptr` still exists.

## File object state

`KernelFileObject` owns:

- the resolved VFS path/device,
- the underlying `FileHandle` for normal files,
- synchronous/asynchronous-open intent,
- current byte position,
- resumable directory-query cursor,
- delete-pending state,
- active `IoRequest` objects.

Positional reads/writes may opt out of updating the shared current position. The
future `NtReadFile` / `NtWriteFile` bridge can therefore apply the exact Xbox
byte-offset rules without changing the VFS.

## Delete pending

The kernel I/O manager has its own open/share registry so guest-facing share
semantics also cover directory handles. Marking a file delete-pending:

1. requires DELETE access on both the open object and the calling handle,
2. rejects new opens of that path,
3. survives closure of the handle that originally requested deletion,
4. physically removes the path after the final independent open object closes.

This deferred removal avoids depending on host-specific unlink-while-open
behaviour. Generation 8 also tracks the file object across a successful rename.
For now rename is deliberately conservative and requires the renaming file object
to be the only independent open object for that path; duplicated handles are fine.
This prevents stale path identity until a later object-identity layer can update
multiple independent opens atomically.

## Directory queries

Directory scan state belongs to the file object, not to individual duplicated
handles. A new query installs a filtered `DirectoryCursor`; subsequent queries
continue it, and restart rewinds it. This is the host-side state needed by
`NtQueryDirectoryFile` before guest `FILE_*` structure packing is added.

## Asynchronous-ready requests

`IoRequest` provides a scheduler-neutral lifecycle:

```text
Pending -> Completed
Pending -> Cancelled
```

Requests carry a 64-bit request ID, operation, opaque context, result and byte
count. Closing the final handle cancels outstanding requests. Synchronous I/O
already uses the same request objects internally. Generation 8 can associate one
`IoCompletionPort` with a file object; completed synchronous requests enqueue a
packet containing the completion key, opaque request context and final `IoStatus`.

## Generation 8 file-information and namespace semantics

`KernelIoManager::open_at` resolves a relative guest path beneath an already-open
directory handle. Absolute paths are rejected when a root handle is supplied, so
`..` traversal cannot escape the root through an accidental host-path join.

The host-side `FileInformationClass` model now covers:

- basic attributes / last-write time,
- standard EOF/allocation/delete-pending/directory state,
- deterministic internal path identity,
- current byte position,
- canonical name,
- network-open metadata,
- synchronous mode and alignment,
- disposition (delete pending),
- end-of-file resize,
- rename with optional root directory,
- completion-port association.

Allocation/preallocation is intentionally reported as unsupported because the
generic device API cannot yet reserve allocation independently of EOF.

`KernelEvent` supports manual-reset and auto-reset signaling, and
`IoCompletionPort` provides queued completion packets plus polling/timed removal.
These are host-side primitives only; Xbox event pointers, APC routines and
`IO_STATUS_BLOCK` packing remain ABI-layer work.

## Deferred until the Memory/kernel bridge phase

- guest pointer and Xbox structure marshalling,
- NTSTATUS numeric mapping and `Nt*`/XAM exports,
- APC delivery and thread alerting,
- wiring per-request Xbox event handles into I/O completion,
- truly asynchronous host execution,
- scatter/gather guest buffers,
- broader rename identity when multiple independent opens exist,
- device-specific allocation/preallocation semantics,
- save-state serialization of kernel objects.

## Generation 9 Xbox/ReXGlue compatibility facade

Generation 9 adds `xenon::kernel::xbox::IoFacade`, the semantic boundary that
sits immediately below the future guest ABI exports. It mirrors the file-call
contract used by Xenia/ReXGlue without introducing a ReXGlue dependency.

The facade owns numeric Xbox `NTSTATUS`, create-action and file/volume
information-class translation. It accepts ordinary host strings/spans and
returns ordinary Xenon kernel/file-information models, so the kernel object
layer remains testable without Memory v2.

Raw create/open requests now decode Xbox generic/data/delete access, share bits,
all six NT create dispositions, directory/non-directory constraints,
synchronous mode and delete-on-close. Successful and failed create/open results
produce the corresponding Xbox `FILE_ACTION` information value.

The facade also implements the host-side semantics for read/write/flush,
directory enumeration, query/set file information, query volume information and
full-attributes queries. For compatibility with the current ReXGlue I/O path,
a non-synchronous file may complete a host operation immediately while the
facade returns `STATUS_PENDING`; the I/O status block still contains the real
completed result and an optional kernel event is signaled.

Device geometry now exposes portable filesystem name/attributes, component-name
limit, sectors per allocation unit and bytes per sector, allowing Xbox
`FILE_FS_SIZE_INFORMATION` and `FILE_FS_ATTRIBUTE_INFORMATION` to be produced
without teaching the VFS about guest structures.

### Remaining bridge work

After Generation 9, the architecture no longer needs another filesystem layer
before AC6 testing. The remaining integration work is intentionally thin:

- Memory v2 guest-address validation and translation;
- big-endian Xbox `X_OBJECT_ATTRIBUTES`, `X_ANSI_STRING`, `X_IO_STATUS_BLOCK`,
  `FILE_*` and `FILE_FS_*` packing/unpacking;
- exported xboxkrnl entrypoints that call `IoFacade`;
- APC delivery/thread alerting and truly asynchronous host execution;
- scatter/gather guest buffers;
- compatibility additions discovered from Project Gracemeria traces.

SVOD, deeper STFS cryptographic verification, FATX/system-storage emulation and
less common IOCTL/information classes remain later compatibility work rather
than blockers for the v1 filesystem architecture.

## Generation 10 Memory v2 guest ABI bridge

Generation 10 connects the Generation 9 Xbox I/O facade to the completed
Memory v2 address space without moving guest-pointer knowledge into the VFS or
host-side kernel object layer.

`xenon::kernel::xbox::GuestIoBridge` is the marshalling boundary:

```text
recompiled Xbox call
        |
        v
GuestIoBridge
  - validates guest ranges through Memory v2
  - decodes big-endian Xbox structures
  - copies file payloads through AddressSpace read_bytes/write_bytes
        |
        v
IoFacade
        |
        v
KernelIoManager -> KernelFileObject -> Xenon::Filesystem
```

The bridge currently covers guest-memory forms of `NtCreateFile`, `NtOpenFile`,
`NtReadFile`, `NtReadFileScatter`, `NtWriteFile`, `NtFlushBuffersFile`,
`NtQueryDirectoryFile`, `NtQueryInformationFile`, `NtSetInformationFile`,
`NtQueryVolumeInformationFile` and `NtQueryFullAttributesFile` semantics.

The bridge understands the Xbox big-endian layouts for `X_ANSI_STRING`,
`X_OBJECT_ATTRIBUTES`, `X_IO_STATUS_BLOCK`, the supported `FILE_*` information
classes, `FILE_DIRECTORY_INFORMATION`, and the supported `FILE_FS_*` volume
classes. Invalid/uncommitted/protected guest buffers are rejected before host
file I/O where practical, preventing a bad guest pointer from advancing the
shared file position.

Memory v2 remains authoritative for mapping, protection, physical aliases and
CPU/GPU coherency. Filesystem code never caches or manufactures host pointers to
guest memory. A read from the host filesystem is copied back with
`AddressSpace::write_bytes`, and a write to the host filesystem is sourced with
`AddressSpace::read_bytes`.

APC execution is deliberately scheduler-neutral. `GuestIoBridge` exposes an APC
queue callback so the later Xbox thread/APC subsystem can own delivery without
creating a kernel-I/O -> CPU threading dependency. ReXGlue-compatible immediate
completion plus `STATUS_PENDING` for non-synchronous handles remains implemented
by `IoFacade`.

Generation 10 also repairs a branch-integration issue discovered when Memory v2
was merged: the filesystem/kernel source tree was present, but its CMake targets
and several Generation 6 STFS source files had been dropped from the active
branch. The merged build now preserves the completed Memory v2 configuration
while restoring `Xenon::Filesystem`, `Xenon::Kernel`, STFS support and the new
`Xenon::XboxKernelIo` bridge target.

### Remaining runtime integration

The filesystem architecture itself no longer needs another redesign before
Project Gracemeria testing. Remaining work belongs to adjacent runtime layers:

- bind the actual xboxkrnl import/export dispatcher to `GuestIoBridge` methods;
- connect its APC queue callback to the Xbox thread/APC implementation;
- expose guest completion-port create/set/remove calls once the general object
  export surface is wired;
- trace Project Gracemeria/AC6 filesystem calls and add only compatibility
  semantics demonstrated by the title;
- later optional coverage: SVOD, FATX/system-storage policy, deeper STFS
  cryptographic verification and uncommon filesystem IOCTLs.
