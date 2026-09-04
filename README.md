# Xenon Recomp

Xenon Recomp is a modular runtime and static recompilation framework intended to support native recompilation of Xbox 360 software for modern hardware and operating systems.

The goal of the project is to provide reusable infrastructure that can be shared between individual game recompilation projects rather than requiring every game to independently recreate the same Xbox 360 runtime functionality.

## Current Status

Xenon Recomp is currently in the architecture and early-development stage.

The project is not currently a complete Xbox 360 runtime and should not be considered an emulator replacement.

Initial development is focused on defining modular interfaces and building the minimum runtime functionality required by supported recompilation projects.

## Goals

The project is intended to provide reusable implementations of:

- Xbox 360 PowerPC instruction handling
- Static recompilation infrastructure
- Intermediate representation
- x86-64 code generation
- ARM64 code generation
- Memory management
- Threading
- Synchronisation
- Timing
- Filesystem services
- Input
- Audio
- Xbox runtime APIs
- Xbox profile functionality
- Content mounting
- Achievement handling
- Networking abstractions
- Graphics abstractions
- Platform-specific services

## Host Architectures

Planned host CPU support:

- x86-64
- ARM64

The architecture should avoid assuming that the host CPU is x86-64.

Where practical, architecture-independent intermediate representations and interfaces should be used so that additional host architectures can be supported later without rewriting game-specific code.

## Supported Platforms

Planned host platforms include:

- Windows
- Linux
- Android

Initial development will primarily focus on desktop systems.

## Graphics

Xenon Recomp will provide a generic graphics abstraction for use by supported recompilation projects.

Planned graphics backends include:

- Vulkan
- Direct3D 12

Vulkan is intended to provide a common graphics backend across Windows, Linux, and Android.

Game-specific native renderers should remain within their respective game projects.

The generic runtime should not contain game-specific rendering behaviour.

## Runtime Design

The intended architecture is:

Game Project
    |
    v
Xenon Recomp Runtime
    |
    +-- CPU Backend
    +-- Xbox Runtime Services
    +-- Graphics API
    +-- Audio
    +-- Input
    +-- Filesystem
    +-- Networking
    |
    v
Host Platform

The dependency direction should remain one-way.

Game projects may depend on Xenon Recomp.

Xenon Recomp must not depend on any individual game project.

## Modularity

The runtime is intended to be split into independent modules wherever practical.

Examples include:

- xenon-core
- xenon-cpu
- xenon-cpu-x64
- xenon-cpu-arm64
- xenon-kernel
- xenon-xbox
- xenon-graphics
- xenon-graphics-vulkan
- xenon-graphics-d3d12
- xenon-audio
- xenon-input
- xenon-network
- xenon-platform-windows
- xenon-platform-linux
- xenon-platform-android

The exact module names may change during development.

## Xbox Services

Xbox-facing APIs should be implemented through modular compatibility layers.

This includes services such as:

- Profiles
- Sign-in state
- Storage
- Achievements
- Presence
- Friends
- Matchmaking
- Sessions
- Content
- Networking

Online functionality should not be tightly coupled to Microsoft's original Xbox Live infrastructure.

A native replacement backend may be used by supported recompilation projects in the future.

## Game-Specific Behaviour

Game-specific functionality does not belong in Xenon Recomp.

For example, the runtime should never contain logic such as:

    if (game == ACE_COMBAT_6)

Individual projects should instead provide their own:

- Hooks
- Patches
- Symbol maps
- Renderers
- Content handling
- Version definitions
- Game-specific networking adapters

## Legal

Xenon Recomp is an independent open-source development project.

It is not affiliated with, endorsed by, or sponsored by Microsoft.

Xbox, Xbox 360, Xbox Live, and related names are trademarks of Microsoft Corporation.

No proprietary Xbox 360 firmware, game executable, copyrighted game data, or Microsoft-owned software is distributed with this repository.
