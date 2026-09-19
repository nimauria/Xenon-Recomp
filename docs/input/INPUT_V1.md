# Xenon Input v1

Xenon Input is the host-neutral controller/input layer shared by recompiled Xbox
360 game modules. It is intentionally separate from XAM ABI marshalling and from
platform APIs such as SDL or Windows XInput.

The architecture follows the proven split used by Xenia and ReXGlue while
keeping Xenon's multi-game/module requirements explicit:

```text
future XAM input exports
        |
        v
   Xenon::Input
        |
  InputSystem
        |
 +------+------------------+
 |                         |
SDL backend          native backends
 |                         |
host controllers / keyboard / mouse
```

Game-specific action injection or control schemes do not belong in this layer.
For example, AC6-specific keyboard/mouse action-layer patches remain Project
Gracemeria concerns; Xenon provides the generic Xbox controller service beneath
them.

## Generation 1

Generation 1 establishes the portable core with no SDL, Windows or guest-memory
dependency:

- `InputDriver` backend contract for setup, enumeration, state, capabilities,
  vibration and keystrokes;
- process-stable cross-driver `DeviceId` values;
- persistent device identities so reconnects recover the same ID and ordinal;
- monotonic device ordinals which are never recycled while the process lives;
- four Xbox user slots with deterministic automatic assignment;
- explicit user/device assignment for future launcher/profile integration;
- driver namespacing so two backends may report the same native/persistent ID
  without colliding;
- XInput-compatible gamepad buttons, triggers, signed thumb axes, vibration and
  capability models;
- packet numbers maintained by the core and incremented only when guest-visible
  state changes;
- `ANY_USER` keystroke routing;
- frontend/focus gating that produces neutral input and stops outstanding
  vibration without embedding window-system policy in every backend;
- duplicate vibration suppression;
- a `NullInputDriver` for tool/headless configurations.

The core deliberately does not apply controller deadzones. Xbox titles commonly
own deadzone/response behaviour themselves, while platform backends should only
normalise host devices into the Xbox signed-axis/trigger range. Configurable
host transforms can be added as a separate mapping layer rather than silently
changing guest-visible controller semantics.

## Generation 2

Generation 2 adds the first real host controller backend while preserving the
portable Generation 1 core. `SdlInputDriver` contains all Xbox-facing routing,
capability and keystroke behaviour, while a small `SdlHost` adapter isolates the
actual SDL API. This keeps the driver testable on systems where SDL development
packages are not installed and leaves room for an SDL3 host without rewriting
the input model.

The SDL2 host provides:

- optional SDL 2.0.9+ discovery through CMake; builds remain valid when SDL is
  absent and the default SDL driver reports `Unsupported` truthfully;
- game-controller subsystem setup/shutdown without any Qt or window dependency;
- controller mapping database loading through `gamecontrollerdb.txt`;
- hotplug reconciliation by scanning the current SDL game-controller set each
  time `InputSystem::refresh_devices()` enumerates the driver;
- SDL joystick instance IDs as live native routing IDs;
- persistent keys based on controller GUID, vendor/product and serial when SDL
  exposes one; identical controllers without serials receive a deterministic
  duplicate suffix for the current enumeration;
- direct Xbox button mapping, signed thumbsticks, Y-axis inversion and 0-255
  trigger normalisation;
- SDL rumble with a long-lived duration matching XInput's persistent vibration
  model as closely as SDL permits;
- wired/wireless classification using SDL's power-level information;
- capabilities routed through the host-neutral Xenon structures.

`SdlInputDriver` also synthesises XInput-style keystrokes for physical buttons,
triggers and eight-way thumbstick directions. Release events are emitted before
new press events for multi-direction transitions, and held keys implement a
400 ms delay / 100 ms repeat cadence by default, matching the behaviour used by
Xenia/ReXGlue's SDL input path. These thresholds and repeat timings remain
backend options rather than game-specific constants.

The SDL backend does not own Xbox user slots. `InputSystem` still assigns the
first four connected devices by stable cross-driver ordinal, so SDL player-index
LEDs or host XInput user numbers cannot silently reorder guest users. Explicit
launcher/profile assignment from Generation 1 remains authoritative.

### Persistent identity limitation

SDL2 does not expose a stable physical path on every platform. When a controller
has no serial number and multiple identical devices share GUID/vendor/product/name,
Generation 2 distinguishes them by deterministic occurrence order. This is good
enough for ordinary hotplug and single-identical-pad use, but true per-device
persistence for indistinguishable hardware should use an SDL3/native path or a
future launcher-side pairing token. Xenon does not pretend this ambiguity is
solved.

### Validation boundary

The SDL-independent driver logic is fully unit tested with a fake host. The real
SDL2 adapter is also syntax-checked against the SDL2 API shape when SDL headers
are unavailable in the build environment. Runtime physical-controller testing
remains a platform validation task for a machine with SDL2 and gamepads attached.

## Planned Input v1 generations

1. **Portable core and stable device assignment** — Generation 1, complete.
2. **SDL gamepad backend** — Generation 2, complete.
3. **Keyboard/mouse virtual gamepad mapping** — Generation 3, complete.
4. **Xbox/XAM semantic facade** — Generation 4, complete.
5. **Profiles/calibration/persistence** — Generation 5, complete.
6. **Advanced devices/polish** — Generation 6, complete.
7. **Memory v2 ABI bridge** — complete: big-endian guest structure marshalling,
   production Memory v2 access, Xbox XAM input ordinals and PPC argument/return
   dispatch are implemented.

The first real-title validation target remains Project Gracemeria / Ace Combat
6, but no AC6-specific input logic belongs in Xenon Input.

## Generation 3

Generation 3 adds a host-neutral keyboard/mouse virtual Xbox controller. It is
fed by frontend/platform events rather than owning a window-system API itself,
so the same mapping core can be used by SDL, Win32, Qt or a future launcher
input service without duplicating Xbox controller synthesis.

The virtual controller provides:

- configurable key-to-button, key-to-trigger and key-to-stick bindings;
- multiple bindings for one host key and multiple host controls contributing to
  the same Xbox control;
- signed keyboard analogue synthesis with configurable magnitude;
- mouse-delta to right-stick conversion with independent X/Y sensitivity and
  inversion;
- configurable mouse-button bindings;
- transient mouse motion consumption so deltas do not become stuck analogue
  state;
- Xbox controller capabilities and a stable virtual-device identity;
- queued controller keystrokes for digital button/trigger bindings;
- a generic default mapping (WASD left stick, mouse right stick and common Xbox
  face/shoulder/trigger keys) that contains no title-specific actions.

AC6-specific direct mouse or named-action controls remain Project Gracemeria
work. Xenon only exposes the generic virtual Xbox pad underneath them.

## Generation 4

Generation 4 adds the host-side Xbox/XAM semantic facade. It mirrors the
observable `XamInput*` behaviour currently used by Xenia/ReXGlue while still
using normal host structures rather than guest pointers.

`xam::InputFacade` implements:

- Xbox numeric `X_ERROR_*` results for success, bad arguments, disconnected
  devices, failed operations and empty keystroke queues;
- `XINPUT_FLAG_GAMEPAD` validation;
- `XINPUT_FLAG_ANY_USER` / low-byte `0xFF` user normalization matching the
  current Xenia/ReXGlue XAM boundary;
- `XamInputGetCapabilities` and `GetCapabilitiesEx` semantics;
- `XamInputGetState`, including null-output connectivity queries;
- `XamInputSetState` vibration routing;
- `XamInputGetKeystroke` and `GetKeystrokeEx` user-index writeback.

The Generation 4 facade deliberately remains host-side. Generation 7 now layers
the Xbox big-endian guest layouts and Memory v2 marshalling above it, keeping
those ABI details out of the portable controller system.

## Generation 5

Generation 5 adds controller profiles and calibration as a transformation layer
inside `InputSystem`. Profiles are applied after a backend has normalized a host
device but before guest-visible packet-number comparison.

A profile can configure:

- per-axis minimum/center/maximum calibration;
- radial inner and outer stick deadzones;
- response-curve exponent;
- stick sensitivity;
- X/Y inversion;
- trigger minimum/maximum calibration;
- trigger inner/outer deadzones, response curve, sensitivity and inversion.

Profile resolution is deterministic:

```text
user binding
    > device binding
        > default profile
```

Device bindings use the InputSystem's stable `driver:persistent-key` identity,
so settings survive live native-ID changes and ordinary reconnects. Profile
files use a small versioned dependency-free format, have deterministic output
ordering, and support load/save plus diagnostics counts/error state for launcher
or support-bundle integration.

Packet tracking compares the transformed state. Changing inversion/deadzones or
switching profiles therefore increments the packet number only when the Xbox
game can actually observe a different state.
## Generation 6

Generation 6 finishes the host-side Input v1 architecture with advanced device
metadata, power state, player indicators, focus/background policy and support
diagnostics. These features remain optional at the driver boundary: a backend
that cannot expose power or player-index control reports `Unsupported` rather
than fabricating Xbox-visible data.

The portable device model now carries:

- vendor ID, product ID and product/firmware version where the host API exposes
  them;
- serial and host path fields for stronger persistent identity and diagnostics;
- explicit power-info and player-indicator capability flags;
- cached dynamic power state and the last successfully-applied player index;
- connection type that may be refined dynamically when a backend learns more
  after initial enumeration.

`PowerInfo` separates power source (`Unknown`, `Wired`, `Battery`) from the
coarse host battery level (`Unknown`, `Empty`, `Low`, `Medium`, `Full`). A
backend may also provide a real percentage, but `0xFF` explicitly means that no
trustworthy percentage is available. Xenon does not invent percentages from
SDL's coarse power levels.

### SDL2 advanced-device support

The SDL2 host now exports vendor/product/product-version and serial data when
available, refreshes joystick power dynamically, and exposes SDL player-index
control as a host assignment hint. The persistent SDL key includes product
version in addition to GUID/vendor/product and prefers serial identity when SDL
provides it. This reduces accidental identity collisions without claiming that
SDL2 can uniquely distinguish every pair of physically identical serial-less
controllers.

SDL player index is intentionally treated as an indicator/hint. Xenon's own
four-user assignment remains authoritative. When users are assigned or hotplug
reconciliation changes, `InputSystem` asks capable backends to synchronize
their player index; unsupported devices simply ignore the feature.

### Foreground and background-input policy

Generation 1's hard `active` gate remains available, but focus policy is now a
separate concern:

```text
enabled
  AND
(focused OR background-policy == Always)
          |
          v
   effective input state
```

Losing effective input immediately stops outstanding rumble and returns neutral
controller state without polling the host backend. `BackgroundInputPolicy::Always`
allows a launcher or user profile to opt into input while unfocused without
disabling the global safety gate.

### Diagnostics

`InputSystem::diagnostics()` now returns a deterministic snapshot suitable for
the launcher/support bundle, including:

- subsystem setup/enabled/focused/effective-active state;
- background-input policy;
- driver, connected-device and assigned-user counts;
- per-device stable identity/ordinal and assigned user;
- state/capability/vibration/keystroke/power operation counters;
- failure counters and the most recent backend result.

These counters are intentionally host-side and contain no guest pointers or
platform-specific controller handles.

### Input v1 freeze point

With Generation 6, the independent host-side Input architecture is complete.
Generation 7 is now implemented against completed Memory v2, so Input v1 is
architecturally complete through the guest ABI boundary. Remaining work is
integration/validation rather than another Input generation: the future global
XAM import/export resolver must register the already-defined Input ordinal
dispatcher, and Project Gracemeria should validate the calls against real AC6
execution. Physical-controller testing, optional SDL3/native identity
improvements and frontend wiring remain normal platform polish.



## Generation 7

Generation 7 connects Input v1 to completed Memory v2 without contaminating the
host-neutral `Xenon::Input` library. A separate `Xenon::InputGuest` target owns
the guest ABI boundary and is built only when both Input and production Memory
are enabled. Host tools may therefore continue to build Input with Memory off.

`xam::guest::GuestInputBridge` implements the Xbox 360 big-endian layouts for:

- `X_INPUT_GAMEPAD` (12 bytes);
- `X_INPUT_STATE` (16 bytes);
- `X_INPUT_VIBRATION` (4 bytes);
- `X_INPUT_CAPABILITIES` (20 bytes);
- `X_INPUT_KEYSTROKE` (8 bytes).

The bridge uses `MemoryPort` for every guest access. In production this is the
Memory v2 `AddressSpace`, so mapping/protection/MMIO/fault policy remains owned
by Memory rather than Input. Invalid non-null guest pointers therefore raise the
canonical structured Memory v2 fault instead of being silently converted into
an input error. Xbox APIs that explicitly accept null outputs, notably
`XamInputGetState`, retain their connectivity-query behaviour.

Guest capabilities are translated from Xenon's portable model into Xbox flags:
force feedback, wireless connection and voice support are encoded using the
Xbox `X_INPUT_CAPS` bit values; internal host-only features such as keystroke
support are not leaked into those bits. Xbox device subtype values are also
marshalled explicitly rather than depending on host enum numbering.

The bridge exposes the input export catalogue and PPC ordinal dispatcher for:

- `0x190 XamInputGetCapabilities`;
- `0x191 XamInputGetState`;
- `0x192 XamInputSetState`;
- `0x193 XamInputGetKeystroke`;
- `0x198 XamInputGetKeystrokeEx`;
- `0x2AD XamInputGetCapabilitiesEx`.

The dispatcher reads PPC arguments from r3 onward and writes the 32-bit Xbox
result to r3. Unknown ordinals are not consumed and leave CPU state untouched.
This is intentionally the narrow seam the CPU v2/runtime import resolver can
register later; Input does not need its own global export framework.

Generation 7 validation uses the production Memory v2 `AddressSpace`, including
a deliberate uncommitted-pointer write to confirm that structured Memory faults
propagate through the XAM bridge.

## Generations 8–11: completion pass

The final Input v1 completion pass adds the remaining host/runtime capabilities
without changing the Gen 1–7 Xbox ABI architecture.

### Generation 8 — native backends and SDL3

- a native Windows XInput backend with fixed-slot enumeration, native state,
  capabilities, rumble, keystrokes and battery information;
- dynamic loading of XInput 1.4/1.3/9.1.0 so Xenon has no hard XInput import;
- optional `XInputGetStateEx` use when available;
- a real SDL3 `SdlHost` implementation alongside SDL2;
- SDL3 is preferred when present and SDL2 remains a fallback;
- SDL3 exposes current metadata, rumble, player index and percentage-capable
  power information through the same host-neutral interface.

### Generation 9 — multi-source users

A guest user may now have a primary device plus additional sources. Guest state
is merged with Xbox-friendly rules: buttons are ORed, triggers take the maximum
value and each stick axis uses the source with greatest magnitude. Packet
numbers describe the merged guest-visible state, not individual host polling.
Keystrokes are polled across all sources while vibration and device metadata
remain anchored to the primary device.

This supports combinations such as controller + keyboard/mouse or controller +
accessibility/flight input without teaching games about multiple host devices.

### Generation 10 — flight/HOTAS mapping

`FlightInputDriver` is a platform-neutral raw flight-device adapter. Platform
layers can connect a device and feed normalized roll, pitch, yaw, throttle,
hat and button state. The default mapping targets ordinary Xbox controller
semantics: roll/pitch map to the left stick, yaw to right-stick X, bidirectional
throttle to the two triggers, hats to the D-pad and buttons through a
configurable table. No AC6-specific engine actions exist in Xenon core.

### Generation 11 — CPU-v1 bridge and conformance hardening

CPU v1 now owns a generic `ExternalCallRegistry` keyed by module and ordinal.
Input registers its completed XAM guest bridge into that registry. The existing
`RuntimeServices` boundary has an optional `external_call` hook and
`RegistryRuntimeServices` provides a CPU-v1 implementation. This is explicitly
an integration seam, not a permanent CPU-v1 dispatcher design: CPU v2 may
replace import resolution while continuing to invoke the same registered
module/ordinal handlers.

The conformance suite now exercises native-backend contracts through fake hosts,
multi-source state/keystroke merging, flight-axis mapping, stable identity
across hundreds of hotplug cycles, Memory-v2 XAM marshalling and CPU registry
routing.

## Input v1 completion status

Input v1 is feature-complete at the architecture/runtime level. Further Input
work should be compatibility or platform validation rather than another core
generation. Remaining practical validation includes running the optional native
Windows and SDL3 adapters on their real platforms, broad physical-controller /
HOTAS matrices, and Project Gracemeria/Xenia/ReXGlue trace comparison under
real title execution. Those checks may expose bugs or device quirks, but they
no longer require a new Input architecture.

## Launcher and native module API integration

The frontend now consumes Input v1 through a live `InputFeature` rather than a
settings-only placeholder. The Input settings page can enumerate connected
controllers, route primary/additional sources to the four Xbox users, bind
input profiles, refresh devices, test rumble, select background-input policy
and expose Input diagnostics. Stable Xenon identity keys are persisted instead
of transient SDL/XInput instance IDs.

Launch configuration carries the selected input backend, background policy,
profile-store location and ordered stable source identities for all four users.
This keeps launcher configuration and the future game-session `InputSystem`
from becoming two unrelated settings models.

Native game modules have a separate versioned ABI in
`xenon/input/module_api.hpp`. `Xenon::InputAPI` is a header-only CMake target for
module consumers; it does not link a second `InputSystem` into the title module.
Runtime code owns `module_api::Provider` and supplies the `ApiV1` function table.
The table exposes state, capabilities, vibration, keystrokes, power, primary
metadata and source enumeration while leaving routing/profiles user-owned.

Module manifests negotiate this contract through:

```json
"runtimeApis": {
  "input": { "version": 1, "required": true }
}
```

See `docs/modules/INPUT_API_V1.md` for the full Project Gracemeria-facing
contract. This native module API is complementary to, not a replacement for,
the Generation 7 big-endian XAM/Memory-v2 guest ABI.
