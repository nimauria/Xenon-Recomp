# Xbox kernel export reference reconciliation

Reference: [Xenia xboxkrnl table at `997d055`](https://github.com/xenia-project/xenia/blob/997d0555dbd6358dffd2950097424993763051af/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc). This is a pinned research inventory, not an official Microsoft ABI specification or a version-qualified Xbox 360 kernel guarantee.

**922 reference entries:** 254 exact Xenon source registrations (27.5%); 668 entries have no matching Xenon source registration. These are registration counts, not implementation or game-compatibility percentages.

| Subsystem | Reference entries | Exact Xenon registrations | No matching source registration |
| --- | ---: | ---: | ---: |
| Crypto | 155 | 3 | 152 |
| Debug | 6 | 3 | 3 |
| Executive | 29 | 15 | 14 |
| Kernel | 120 | 40 | 80 |
| Memory | 25 | 12 | 13 |
| Modules | 38 | 3 | 35 |
| NT / I/O | 87 | 40 | 47 |
| Objects | 16 | 4 | 12 |
| Other | 288 | 66 | 222 |
| RTL | 56 | 38 | 18 |
| Variables | 35 | 10 | 25 |
| Video | 67 | 20 | 47 |

The absence of a matching registration means this source audit found no handler; it does not establish that the export is needed by a title, unsupported on every kernel version, or absent from a dynamic path. Optional audio registration is build-conditional. The source-derived [dashboard](README.md) keeps its own denominator and audited behavior states.

First unmatched entries by ordinal: `0x0003 DbgPrint`, `0x0005 DumpGetRawDumpInfo`, `0x0006 DumpWriteDump`, `0x000C ExConsoleGameRegion`, `0x000E ExEventObjectType`, `0x0012 ExMutantObjectType`, `0x0014 ExRegisterThreadNotification`, `0x0017 ExSemaphoreObjectType`, `0x0018 ExSetXConfigSetting`, `0x001A ExTerminateTitleProcess`, `0x001C ExTimerObjectType`, `0x001D MmDoubleMapMemory`, `0x001E MmUnmapMemory`, `0x001F XeKeysGetConsoleCertificate`, `0x0022 HalGetCurrentAVPack`, `0x0023 HalGpioControl`.
