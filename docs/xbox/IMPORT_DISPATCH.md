# Xbox Import Dispatch

Generation 11 adds the module-neutral import surface used by statically
recompiled Xbox 360 titles.

## Boundary

```text
recompiled title/module
        |
        | import name or xboxkrnl ordinal
        v
xenon::xbox::ImportRegistry
        |
        | PPC ABI thunk (r3-r10, then r1+0x54 argument slots)
        v
xenon::kernel::xbox::GuestIoBridge
        |
        v
Memory v2 + Xenon::Kernel + Xenon::Filesystem
```

The registry does not depend on Project Gracemeria or any other game. A game
loader/recompiler may resolve imports by ordinal (the normal XEX case) or by
name (useful for generated/static projects and diagnostics).

Module names are case-insensitive and `.exe` / `.dll` suffixes are normalized,
so `xboxkrnl`, `XBOXKRNL.EXE`, and `xboxkrnl.dll` identify the same provider.

## PPC calling convention

Integer/pointer arguments 0 through 7 are read from r3 through r10. Additional
integer arguments use the Xbox PPC64 parameter save area:

```text
argument 8 -> [r1 + 0x54]
argument 9 -> [r1 + 0x5C]
...
```

Each slot is eight bytes wide. Xbox 32-bit arguments occupy the big-endian
32-bit value at the beginning of the slot. Memory v2 mapping/protection is
validated before spilled arguments are read.

Return values from the current xboxkrnl I/O thunks are 32-bit NTSTATUS values
written to r3.

## Registered xboxkrnl filesystem imports

| Ordinal | Import |
| ---: | --- |
| `0x00D2` | `NtCreateFile` |
| `0x00DB` | `NtFlushBuffersFile` |
| `0x00DF` | `NtOpenFile` |
| `0x00E4` | `NtQueryDirectoryFile` |
| `0x00E7` | `NtQueryFullAttributesFile` |
| `0x00E8` | `NtQueryInformationFile` |
| `0x00EF` | `NtQueryVolumeInformationFile` |
| `0x00F0` | `NtReadFile` |
| `0x00F1` | `NtReadFileScatter` |
| `0x00F7` | `NtSetInformationFile` |
| `0x00FF` | `NtWriteFile` |

These ordinals match the Xbox 360 xboxkrnl export table used by Xenia/ReXGlue.

## Game-loader contract

A loader should:

1. construct one `ImportRegistry` for the runtime;
2. call `register_xboxkrnl_io_imports` during Xbox platform initialization;
3. parse each XEX import library entry;
4. resolve the requested ordinal/name against the registry;
5. bind the generated/recompiled callsite to the returned thunk or retain the
   module+ordinal key for runtime dispatch;
6. invoke the thunk with the current `CpuState`, Memory v2 `AddressSpace`, and
   runtime `GuestIoBridge`.

Missing imports are explicit. The registry never fabricates a successful stub.
Future XAM, XAPI, networking, audio, threading and other xboxkrnl exports can be
registered into the same mechanism without changing recompiled game modules.

## CPU v2 relationship

The import registry does not execute or decode PPC instructions. CPU v2 only
needs to expose the architectural `CpuState` at an imported-call boundary. This
keeps import development independent from the CPU v2 implementation and allows
CPU backends/AOT code generators to use the same registered services.
