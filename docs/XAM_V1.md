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
- Uses documented ordinal values (from Xenia/ReXGlue research)
- Implements PPC calling convention via `ExportCallContext`
- Classifies as `Required`, `Stubbed`, `Optional`, or `DiagnosticOnly`

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
| 0x0180 | `XamUserGetXUID` | ✅ Implemented | Get XUID for user index |
| 0x0181 | `XamUserGetSigninState` | ✅ Implemented | Get sign-in state |
| 0x0183 | `XamUserGetName` | ✅ Implemented | Get gamertag |
| 0x0187 | `XamUserCheckPrivilege` | ✅ Stubbed | Check user privilege (grants all offline) |

### Phase 2: Locale & Language ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x0206 | `XamGetLanguage` | ✅ Implemented | Get system language |
| 0x0207 | `XamGetLocale` | ✅ Implemented | Get system locale (LCID) |
| 0x0208 | `XamGetTimeZoneInformation` | ✅ Stubbed | Get timezone info |

### Phase 3: Storage & Content ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x0250 | `XamShowDeviceSelectorUI` | ✅ Stubbed | Returns default HDD |
| 0x0234 | `XamContentCreateEnumerator` | ✅ Stubbed | Create content enumerator |
| 0x0237 | `XamContentClose` | ✅ Stubbed | Close content handle |

### Phase 5: Notifications ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x0210 | `XamNotifyCreateListener` | ✅ Stubbed | Create notification listener |
| 0x0211 | `XamNotifyGetNext` | ✅ Stubbed | Get next notification |
| 0x0212 | `XamNotifyPositionUI` | ✅ Stubbed | Position notification UI (no-op) |

### Phase 6: Achievements ✅

| Ordinal | Name | Status | Description |
|---------|------|--------|-------------|
| 0x0280 | `XamUserWriteAchievements` | ✅ Implemented | Unlock achievement (offline) |
| 0x0281 | `XamUserReadStats` | ✅ Implemented | Read user statistics |
| 0x0282 | `XamUserWriteStats` | ✅ Implemented | Write user statistics |
| 0x0284 | `XamUserCreateAchievementEnumerator` | ✅ Stubbed | Create achievement enumerator |

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

- [Runtime Session](RUNTIME_SESSION.md)
- [Input V1](input/INPUT_V1.md)
- [Kernel I/O V1](kernel/KERNEL_IO_V1.md)
- [Filesystem V1](filesystem/FILESYSTEM_V1.md)

## History

**2026-09-19**: Phase 1 foundation completed. User management, default offline user, and core user exports implemented.
