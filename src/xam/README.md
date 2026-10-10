# Xenon XAM Implementation

This directory contains the implementation of Xenon's XAM (Xbox Application Management) subsystem.

## Structure

- `xam_session.cpp` - XAM subsystem coordinator
- `user_manager.cpp` - User sign-in, XUID, and profile management
- `locale_manager.cpp` - Locale/language state
- `content_manager.cpp` - Storage/content device management
- `content_graph.cpp` - Content/DLC/title-update dependency graph
- `dlc_manager.cpp` - DLC catalogue and installed-state tracking
- `save_manager.cpp` - Save data operations
- `title_update_manager.cpp` - Title update management
- `notification_manager.cpp` - System notification queueing
- `achievement_manager.cpp` - Achievements and per-title statistics
- `xam_user_exports.cpp` - User-related XAM export implementations
- `xam_locale_exports.cpp` - Locale-related XAM export implementations
- `xam_content_exports.cpp` - Content-related XAM export implementations
- `xam_notification_exports.cpp` - Notification-related XAM export implementations
- `xam_achievement_exports.cpp` - Achievement-related XAM export implementations
- `xam_system_exports.cpp` - System-related XAM export implementations
- `xam_net_exports.cpp` - Networking-related XAM export implementations
- `xam_socket_manager.cpp` - Socket management backing `xam_net_exports.cpp`
- `xam_voice_exports.cpp` - Voice-related XAM export implementations
- `xam_task_exports.cpp` - Task-related XAM export implementations
- `xam_msg_exports.cpp` - Message-related XAM export implementations
- `xam_enum_exports.cpp` - Enumerator-related XAM export implementations
- `xam_session_handle_exports.cpp` - Session-handle-related XAM export implementations
- `xam_ui_exports.cpp` - UI-related XAM export implementations

## Current Status

Offline users/XUIDs, locale/language state, storage/content management, notifications,
achievements/per-title statistics, and DLC/title-update/content-graph services are all
implemented and wired into the export registry (see the root [`README.md`](../../README.md)'s
XAM status entry and `CMakeLists.txt` for how these sources are built).

For the authoritative design/status reference, including what is still unsupported or
offline-only, see [`docs/xam/XAM_V1.md`](../../docs/xam/XAM_V1.md).
