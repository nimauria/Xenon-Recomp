# Xenon XAM V1

## Overview

XAM (Xbox Application Management) is the Xbox 360 platform services layer that provides user management, profiles, content enumeration, save data, achievements, notifications, and system UI integration. XAM V1 implements the common retail-game platform services required by Xbox 360 titles, starting with Armored Core 6 (AC6) requirements and generalizing from there.

**Xenon owns XAM behaviour.** Unlike the input layer (which follows observable XAM semantics), this XAM implementation is the authoritative platform service layer for recompiled titles.

## Architecture

### Subsystem Structure

```
xenon/xam/
├── types.hpp              # Common types (XUID, XResult, enums)
├── xam_session.hpp/cpp    # XAM subsystem coordinator
├── user_manager.hpp/cpp   # User sign-in, XUID, profiles
└── xam_user_exports.hpp/cpp  # User export implementations
```

### Integration

XAM integrates into the Xenon runtime session as a first-class subsystem:

```cpp
xenon::core::XenonSession
  ├── memory::AddressSpace
  ├── filesystem::VirtualFileSystem
  ├── kernel::KernelIoManager
  ├── input::InputSystem
  ├── gpu::Backend
  ├── xam::XamSession       // ← XAM subsystem
  └── core::ExportRegistry  // ← XAM registers exports here
```

### Export Registry

All XAM exports use the common `core::ExportRegistry`. No separate ad-hoc dispatcher. Each XAM export:

- Registers with library name `"xam"`
- Uses the real xam.xex ordinal values, verified against
  `xenia-project/xenia`'s `src/xenia/kernel/xam/xam_table.inc` (primary
  catalogue) and cross-checked against `rexglue/rexglue-sdk`'s
  `src/kernel/xam/export_table.inc` - see `include/xenon/xam/xam_exports.hpp`
  for the full per-export rationale and rename history
- Implements PPC calling convention via `ExportCallContext`
- Classifies as `Required`, `Stubbed`, `Optional`, or `DiagnosticOnly`

**2026-09-20 ordinal correctness pass**: an audit against the authoritative
xam.xex export table found every previously-registered XAM ordinal below
`0x02AD` (i.e. everything except the XamInput* family) was wrong - some by a
missing/extra "Xam" prefix (`XNotifyGetNext`/`XNotifyPositionUI`), some by
range (content/locale/notification/UI ordinals were all numbered as if
sequential from a made-up base rather than their real, scattered xam.xex
positions), and three (`XamUserWriteAchievements`, `XamUserReadStats`,
`XamUserWriteStats`) had no real xam.xex export under any name at all and
were removed rather than renumbered. All tables below reflect the corrected
values; see `tests/xam/xam_export_ordinal_tests.cpp` for literal-ordinal
regression tests independent of Xenon's own enum constants.

## Default Offline Model

XAM V1 provides a coherent offline local user by default. **No Xbox Live connectivity is required** for normal offline games.

### Default User

- **User Slot**: 0
- **XUID**: `0xE000000000000001` (offline range)
- **Gamertag**: `"XenonPlayer"`
- **Sign-in State**: `SignedInLocally`
- **Language**: English
- **Flags**: None (not guest, not online)

The default user is automatically created during `XamSession::initialize()`.

### Additional Users

Applications can sign in up to 4 local users (indices 0-3):

```cpp
manager.sign_in_user(1, "Player2", false);  // Regular user
manager.sign_in_user(2, "Guest", true);     // Guest user
```

Each user receives a unique offline XUID in the `0xE000000000000000` range.

## Implemented APIs

### Phase 1: User Management ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x020A | `XamUserGetXUID` | ✅ Implemented | Get XUID for user index |
| 0x0210 | `XamUserGetSigninState` | ✅ Implemented | Get sign-in state |
| 0x020E | `XamUserGetName` | ✅ Implemented | Get gamertag |
| 0x0212 | `XamUserCheckPrivilege` | ✅ Stubbed | Check user privilege (grants all offline) |

### Phase 2: Locale & Language ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x03D2 | `XamGetLanguage` | ✅ Implemented | Get system language |
| 0x04A9 | `XamGetLocale` | ✅ Implemented | Get system locale (LCID) |
| 0x04AA | `XamQueryTimeZoneInformation` | ✅ Stubbed | Get timezone info (renamed from the invented "XamGetTimeZoneInformation") |

### Phase 3: Storage & Content ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x02CB | `XamShowDeviceSelectorUI` | ✅ Stubbed | Returns default HDD (real identity is part of the XamShow* UI family, not content storage) |
| 0x025C | `XamContentCreateEnumerator` | ✅ Stubbed | Create content enumerator |
| 0x025A | `XamContentClose` | ✅ Stubbed | Close content handle |
| 0x025E | `XamContentGetDeviceData` | ✅ Implemented | Storage device capacity data |
| 0x025F | `XamContentGetDeviceName` | ✅ Implemented | Storage device name |

### Phase 5: Notifications ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x028A | `XamNotifyCreateListener` | ✅ Stubbed | Create notification listener |
| 0x028B | `XNotifyGetNext` | ✅ Stubbed | Get next notification (real identity has no "Xam" prefix) |
| 0x028C | `XNotifyPositionUI` | ✅ Stubbed | Position notification UI (no-op; real identity has no "Xam" prefix) |

### Phase 6: Achievements ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x02EE | `XamUserCreateAchievementEnumerator` | ✅ Stubbed | Create achievement enumerator |
| 0x02F7 | `XamUserCreateStatsEnumerator` | ✅ Stubbed | Create stats enumerator |

`XamUserWriteAchievements`, `XamUserReadStats`, and `XamUserWriteStats`
(previously listed at invented ordinals 0x0280-0x0282) have been removed:
no xam.xex export exists under these names in either xenia or rexglue-sdk's
tables. On real hardware, achievement unlocks and stat updates are written
directly into the title's cached profile GPD via the content APIs, not
through a dedicated XAM ordinal call. `AchievementManager::unlock_achievement()`
and `write_stat()` remain available as internal Xenon APIs (via
`XamSession::achievements()`) for a future GPD-backed write path.

### Phase 4: Save Data (Deferred)

Save data operations will use the existing filesystem VFS and kernel I/O layer. Content APIs above provide the infrastructure.

## Unimplemented UI

For dashboard/system UI calls Xenon cannot meaningfully reproduce, provide:

1. **Sensible Native Equivalents**: Where possible (e.g., file picker for storage selection)
2. **Explicit Unsupported Behaviour**: Clear diagnostic logging with appropriate error codes
3. **No Success Pretense**: Never return success if the game depends on returned state

## AC6 Requirements

Initial implementation prioritizes APIs required by Armored Core 6:

1. ✅ **User sign-in state** - Default offline user
2. ✅ **XUID queries** - Offline XUID generation
3. ✅ **Gamertag retrieval** - Default and custom names
4. 🔄 **Storage device selection** - Phase 3
5. 🔄 **Save data operations** - Phase 4
6. 🔄 **Language/locale** - Phase 2

## Implementation Status

### Phase 1: User Management ✅ COMPLETE

- ✅ UserManager with default offline user
- ✅ Core user exports implemented
- ✅ Unit tests

### Phase 2: Locale & Language ✅ COMPLETE

- ✅ LocaleManager
- ✅ Language/locale exports
- ✅ Timezone support (stubbed)

### Phase 3: Storage & Content ✅ COMPLETE

- ✅ ContentManager
- ✅ Storage device enumeration
- ✅ Content operations (stubbed)
- ✅ Default HDD device

### Phase 4: Save Data ⏳ DEFERRED

- ⏳ Uses existing VFS/Kernel I/O
- ⏳ Content APIs provide infrastructure

### Phase 5: Notifications ✅ COMPLETE

- ✅ NotificationManager
- ✅ Notification queue (logged)
- ✅ Listener management (stubbed)

### Phase 6: Achievements ✅ COMPLETE

- ✅ AchievementManager
- ✅ Offline achievement unlocking
- ✅ User statistics read/write
- ✅ Per-title tracking

### Phase 7: AC6 Validation ⏳ TODO

- ⏳ Trace AC6 XAM calls
- ⏳ Verify offline gameplay
- ⏳ Test all implemented APIs

## Testing

Unit tests: `tests/xam/user_manager_tests.cpp`

Build and run:

```bash
cmake --build build --target xenon_xam_user_manager_tests
./build/tests/xenon_xam_user_manager_tests
```

## Done When

AC6 and future offline titles can obtain normal Xbox user/profile/storage state through Xenon without game-specific XAM hacks.

## See Also

- [Runtime Session](../runtime/RUNTIME_SESSION.md)
- [Input V1](../input/INPUT_V1.md)
- [Kernel I/O V1](../kernel/KERNEL_IO_V1.md)
- [Filesystem V1](../filesystem/FILESYSTEM_V1.md)

## History

**2026-09-19**: Phase 1 foundation completed. User management, default offline user, and core user exports implemented.
