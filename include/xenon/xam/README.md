# Xenon XAM Headers

This directory contains the public interface for Xenon's XAM (Xbox Application Management) subsystem.

## Headers

- `types.hpp` - Common XAM types (XUID, XResult, enums, constants)
- `user_manager.hpp` - User management interface
- `locale_manager.hpp` - Locale/language state interface
- `content_manager.hpp` - Storage/content device management interface
- `content_graph.hpp` - Content/DLC/title-update dependency graph interface
- `dlc_manager.hpp` - DLC catalogue and installed-state interface
- `save_manager.hpp` - Save data management interface
- `title_update_manager.hpp` - Title update management interface
- `notification_manager.hpp` - System notification queueing interface
- `achievement_manager.hpp` - Achievements and per-title statistics interface
- `xam_session.hpp` - XAM subsystem coordinator
- `xam_exports.hpp` - XAM export ordinal definitions
- `xam_user_exports.hpp` - User export registration interface
- `xam_net_exports.hpp` - Networking-related export registration interface
- `xam_socket_manager.hpp` - Socket management interface used by the networking exports
- `xam_system_exports.hpp` - System export registration interface

## Usage

Include `xenon/xam/xam_session.hpp` to access the XAM subsystem:

```cpp
#include "xenon/xam/xam_session.hpp"

// Access via XenonSession
auto* xam = session->xam();
auto& users = xam->users();

// Query user information
bool signed_in = users.is_signed_in(0);
XUID xuid = users.xuid(0);
std::string gamertag = users.gamertag(0);
```

## Documentation

See `docs/xam/XAM_V1.md` for complete documentation.
