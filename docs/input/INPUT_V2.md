# Xenon Input V2

Input V2 keeps the Xbox 360-facing controller contract stable while replacing
the host side with a broader, more resilient controller layer. Xbox, DualSense,
Steam, Nintendo and generic controllers all normalize into the same four-user
Xbox model used by XAM and recompiled titles.

Input V1 remains the historical architecture document. V2 is additive at the
guest boundary: existing XAM imports and the native module API v1 remain binary
compatible.

## Backend selection

The launcher's `Automatic` mode now starts with SDL on every platform. SDL is
the only existing backend that can cover Xbox controllers, PlayStation 5
DualSense, Steam Controller/Steam Deck, Nintendo controllers and generic USB or
Bluetooth pads through one identity and mapping model.

On Windows, native XInput remains available as an explicit choice and is used
as the automatic fallback if SDL cannot initialize. Xenon does not start SDL
and XInput over the same hardware at once, avoiding the double-controller
problem common with compatibility and remapping software.

## Controller families

Device metadata separates the physical `ControllerFamily` from the guest-facing
Xbox `DeviceSubtype`. For example, a DualSense reports `PlayStation` as its
family while still behaving as an Xbox gamepad to an Xbox 360 title.

Families currently identified by the SDL hosts are:

- Xbox 360, Xbox One and Xbox Series-compatible controllers;
- PlayStation 3, PlayStation 4 and PlayStation 5 controllers;
- Nintendo Switch Pro and Joy-Con variants;
- Steam Controller and Steam Deck controls;
- generic, virtual and unknown controllers.

SDL3 supplies a native Steam family. SDL2 additionally recognizes Valve's USB
vendor ID and Steam/Valve device names because SDL2 predates that enum.

## PlayStation 5 DualSense

SDL's PS5 HID driver is enabled before controller initialization. Extended PS5
reports are enabled so Bluetooth DualSense controllers can rumble, and SDL's
player-light support remains available.

The standard Xbox-facing path includes:

- face buttons, shoulders, sticks, stick clicks and D-pad;
- Options/Create through SDL's Start/Back mapping;
- triggers and standard two-motor rumble;
- USB/Bluetooth connection and battery information when SDL can provide it;
- stable Sony family, vendor/product, serial and host-path metadata.

V2 also exposes optional host-native operations for controller-aware frontend
features:

- accelerometer and gyroscope readings in SI units;
- up to four normalized touchpad contacts;
- RGB/mono controller-light control.

These extensions do not invent Xbox 360 controls for the touchpad, microphone
button or adaptive triggers. A title still sees the controls an Xbox 360 title
understands; native launcher or module features may opt into the additional
data explicitly.

## Steam Controller and Steam Deck

SDL's Steam and Steam Deck HID paths are enabled before initialization. Physical
Steam hardware and Steam-provided mappings are normalized through SDL's gamepad
interface, including trackpad-as-stick and trigger mappings supplied by SDL or
`gamecontrollerdb.txt`.

When Steam Input presents a virtual Xbox controller, Xenon intentionally treats
that virtual device as the usable controller instead of also opening a native
XInput stack and producing duplicate users.

## Hotplug and persistent routing

Explicit assignments are now identity-based rather than connection-based.
Assigning a primary device or a multi-source route changes that user from
automatic to explicit routing. The requested stable identities remain stored
when a controller disconnects and are restored when it reconnects, even if its
live SDL instance ID changed.

Clearing a user is also sticky; a later device refresh no longer silently
auto-assigns that user. Automatic assignment can be enabled again explicitly.
The launcher persists both the route and its automatic/explicit mode.
It performs a quiet coarse hotplug scan every 1.5 seconds and updates the UI
only when the connected topology changes; manual refresh remains available.

SDL2 identity quality is improved in this order:

1. GUID, vendor, product and product version;
2. serial number when available;
3. host device path when SDL exposes it;
4. device name plus deterministic duplicate suffix as the final fallback.

SDL3 continues to prefer serial and path information. Stable Xenon `DeviceId`
values and ordinals survive ordinary reconnects.

## Robustness changes

- SDL driver state, hotplug cache, keystroke repeat state and output operations
  are serialized so frontend, guest and refresh callers cannot race the same
  backend objects.
- Routed device identities are copied before a backend call rather than kept as
  string views into a container that hotplug reconciliation may rehash.
- SDL3 capability reporting reads the backend's actual rumble and LED
  properties instead of claiming every controller supports them.
- SDL3 now maps the Guide button, matching the SDL2 and XInput paths.
- Complete multi-source routes can be replaced transactionally, preventing a
  half-applied launcher configuration.

## Compatibility boundary

The XAM guest ABI, packet-number behavior, profile transforms, keystroke model
and module API v1 remain unchanged. The new controller-family and extended-input
operations are host-side C++ APIs. A future module ABI version can expose them
without changing or invalidating existing v1 consumers.

## Validation boundary

The host-neutral behavior, extended-device routing, sticky assignments and SDL
adapter contract are covered by automated tests. SDL2 compilation is validated
against the repository's managed SDL 2.32 headers and the complete launcher is
built with the new backend selection and QML metadata.

Physical USB/Bluetooth qualification still requires real DualSense, Steam
Controller and Steam Deck hardware. In particular, Bluetooth firmware,
operating-system HID ownership and Steam Input configuration can affect which
SDL features a host exposes; Xenon reports those capabilities instead of
fabricating them.
