# Xenon XAM Headers

This directory contains the public interface for Xenon's XAM (Xbox Application Management) subsystem.

## Headers

- `types.hpp` - Common XAM types (XUID, XResult, enums, constants)
- `user_manager.hpp` - User management interface
- `xam_session.hpp` - XAM subsystem coordinator
- `xam_exports.hpp` - XAM export ordinal definitions
- `xam_user_exports.hpp` - User export registration interface

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

See `docs/XAM_V1.md` for complete documentation.
