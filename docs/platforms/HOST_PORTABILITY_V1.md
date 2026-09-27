# Host Portability V1

Host Portability V1 is a defensive portability pass. It does **not** add a new
Xbox execution model, a Metal renderer, or an ARM64-native code emitter. The
goal is to make shared Xenon code describe host capabilities explicitly so the
existing portable paths can run correctly on more hosts.

## Rules

- Xbox 360 guest semantics remain host-independent.
- Guest base pages remain 4 KiB even when a host OS uses larger VM pages.
- Host VM, graphics, audio, input, and packaging code should branch on
  capabilities where practical rather than platform names.
- Unsupported acceleration paths must fall back to a correct portable path.
- ARM64 initially uses the portable C++ AOT path. Architecture-specific native
  emitters are deferred until real-title profiling justifies them.

## Memory V2

`host_vm::Capabilities` now reports host page size, allocation granularity,
fixed-shared-mapping support, and the smallest fixed mapping granularity.
Memory V2's optional 4 GiB direct guest aperture is enabled only when the host
can replace mappings at the Xbox base-page granularity (4 KiB).

This distinction matters on hosts whose general VM APIs support fixed mappings
but only at a larger granularity. Such hosts remain fully supported by Memory
V2's compact translation path rather than partially constructing an aperture
that later fails on individual 4 KiB mappings.

## Runtime diagnostics

The runtime host logs normalized host platform/architecture and VM capability
information at session startup, including whether the host is compatible with
Xenon's 4 KiB direct-aperture acceleration. These diagnostics are intended to
help Windows/Linux qualification now and future ARM64/macOS bring-up later.

## Vulkan

Vulkan instance extension requests are validated before instance creation. If
`VK_KHR_portability_enumeration` is exposed by the host loader, Xenon enables
it and the required instance-create flag automatically. Selected devices also
record and enable `VK_KHR_portability_subset` when exposed.

This is capability-driven and is not hard-coded to macOS. It prepares the
existing Vulkan backend for portability implementations such as MoltenVK while
also improving diagnostics for unusual Windows/Linux Vulkan stacks.

## Build and dependency naming

CMake now normalizes Apple's Darwin host name to `macos`, matching Xenon's
managed dependency triplets, and recognizes common x64/ARM64 processor aliases.
The managed Vulkan-loader staging code also understands macOS `.dylib` names.
MoltenVK itself is intentionally not provisioned by this pass.

## Deferred work

The following remain later platform work:

- MoltenVK provisioning and macOS surface/window qualification.
- A `macos-arm64` CMake preset and CI runner.
- Qt `.app` deployment, signing, notarization, and DMG/ZIP packaging.
- macOS audio/controller qualification.
- ARM64/NEON-specific lowering or a direct native emitter.
- Native Metal backend work, if profiling eventually justifies one.

These should be tackled after the shared Windows x64 real-title path is stable
enough that portability failures can be distinguished from guest-semantics
failures.
