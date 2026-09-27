# Xenon XAM Implementation

This directory contains the implementation of Xenon's XAM (Xbox Application Management) subsystem.

## Structure

- `user_manager.cpp` - User sign-in, XUID, and profile management
- `xam_session.cpp` - XAM subsystem coordinator
- `xam_user_exports.cpp` - User-related XAM export implementations

## Current Status

**Phase 1: Foundation** - ✅ Complete

Implemented:
- Default offline user (XUID `0xE000000000000001`, gamertag "XenonPlayer")
- User management for 4 slots
- Core user exports: `XamUserGetXUID`, `XamUserGetSigninState`, `XamUserGetName`, `XamUserCheckPrivilege`
- Export registry integration

## Future Work

Future phases will add:
- Locale and language management
- Storage device selection
- Content enumeration
- Save data operations
- Notifications
- Achievements

See `docs/xam/XAM_V1.md` for complete specification.
