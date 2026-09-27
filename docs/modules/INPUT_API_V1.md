# Xenon Input Module API v1

Xenon Input exposes a small native ABI for game modules that need access to the
same guest-visible input state used by the runtime. It is intentionally separate
from the Xbox/XAM guest ABI and from the launcher's Qt interfaces.

A game module such as Project Gracemeria should consume this API rather than
including `InputSystem`, SDL, XInput, launcher headers, or platform controller
APIs directly.

## Manifest negotiation

Declare the required API in the module manifest:

```json
{
  "id": "org.example.project-gracemeria",
  "name": "Project Gracemeria",
  "runtimeApis": {
    "input": {
      "version": 1,
      "required": true
    }
  }
}
```

The launcher validates this requirement before launch. A required version newer
than the runtime-supported version is rejected instead of silently starting with
an incompatible native ABI.

`required: false` may be used by a module that can operate without the native
Input API and wants to feature-detect it at runtime.

## Build contract

The public ABI header is:

```cpp
#include <xenon/input/module_api.hpp>
```

When consuming Xenon as a CMake subproject, modules should use the header-only
ABI target:

```cmake
target_link_libraries(project_gracemeria PRIVATE Xenon::InputAPI)
```

`Xenon::InputAPI` intentionally does **not** link or instantiate Xenon's input
implementation. The runtime owns exactly one `InputSystem`. Modules receive a
pointer to the runtime-owned `xenon::input::module_api::ApiV1` function table.

Runtime/frontend code that owns an `InputSystem` uses the separate provider
header internally:

```cpp
#include <xenon/input/module_api_provider.hpp>
```

Game modules should not instantiate `Provider` themselves.

## ABI rules

Before using the table, validate it:

```cpp
using namespace xenon::input::module_api;

bool bind_input(const ApiV1* input) {
  if (!compatible_v1(input)) return false;
  if (!has_feature(input, FeatureState)) return false;
  return true;
}
```

A v1 consumer must:

- check `abi_version` and `struct_size`;
- check the relevant feature bit before calling an optional function;
- treat `context` as opaque;
- never retain pointers to Xenon-private C++ objects;
- never mutate launcher-owned device assignment or profile state directly.

The function table and its public value structures use fixed-width integer
fields and standard-layout/trivially-copyable types. Result values are defined
by `ResultV1` and are independent from Xenon's private internal enum values.

## Available operations

Input API v1 provides:

- merged/profile-processed Xbox-visible state per Xbox user;
- capabilities;
- vibration output;
- Xbox-style keystrokes;
- battery/power information;
- primary-device metadata;
- enumeration of all host sources contributing to an Xbox user.

Profiles, calibration, deadzones, response curves, user routing and multi-source
selection remain user/runtime-owned. A module receives their result rather than
reimplementing them.

This is important for title modules: Project Gracemeria can query the final
controller state or identify that the active source is a flight device, but it
does not need to know whether the user's hardware arrived through SDL3, SDL2,
Windows XInput, keyboard/mouse synthesis or a HOTAS adapter.

## Example state query

```cpp
using namespace xenon::input::module_api;

ResultV1 read_player_one(const ApiV1* api, StateV1& state) {
  if (!compatible_v1(api) || !has_feature(api, FeatureState) ||
      api->get_state == nullptr) {
    return ResultV1::Unsupported;
  }

  return static_cast<ResultV1>(api->get_state(api->context, 0, &state));
}
```

`StateV1` is host-native module data. It is **not** the big-endian Xbox guest
structure. Xenon's Generation 7 `Xenon::InputGuest` bridge separately handles
`X_INPUT_STATE`, guest pointers and Memory v2 for recompiled Xbox imports.

## Project Gracemeria integration model

The intended ownership is:

```text
launcher/user settings
        |
        v
runtime InputSystem
  |            |
  |            +--> Xbox/XAM guest ABI -> recompiled AC6 imports
  |
  +--> Input Module API v1 -> Project Gracemeria native hooks/features
```

Both paths observe the same assignments, profiles and merged controller state.
A title module therefore does not create a parallel controller stack.

Project Gracemeria may use the native API for title-specific enhancements (for
example diagnostics or optional flight-device-aware presentation) while normal
Xbox input calls continue through the XAM guest bridge.

## Launcher/runtime handoff

The launcher now records the module's `runtimeApis` requirements plus the
selected input backend, stable device/source identities, profile-store path and
background-input policy in `LaunchConfiguration`.

The current generic Xenon runtime still does not expose the final game-session /
native-module loader API, so the launcher does not fabricate a module pointer
handoff today. When that session loader is introduced, it should instantiate the
runtime-owned `module_api::Provider` and pass its `ApiV1` table to modules whose
manifest negotiated `runtimeApis.input`.

No Input redesign is required for that step; it is a runtime/module-loader
connection using this already-versioned contract.
