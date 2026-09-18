# Windows-native runtime and GPU validation

Project Xenon's production `AddressSpace` is supported on Windows and POSIX.
On Windows, the 512 MiB Xbox physical-memory backing is one reserved virtual
range committed with `VirtualAlloc`. Reset decommits and recommits the same
range, preserving the stable base pointer while restoring zero-filled pages.
Physical reset, allocation/free and non-contiguous cross-page writes notify
native GPU mirrors so host resources cannot retain stale guest RAM.

The Windows Debug and Release presets enable the production memory subsystem
and Xenos graphics frontend. This allows the same memory, CPU integration,
command processor, draw IR and shader decoder tests to run natively on Windows.

The graphics development setup uses LunarG Vulkan SDK 1.4.357.0 from
`C:\VulkanSDK\1.4.357.0`. CMake discovers the SDK through `VULKAN_SDK`; the
installed loader, validation layers and NVIDIA driver are exercised by
`xenon_backend_capability_tests` alongside D3D12.

This is host platform support, not Xbox GPU emulation. Guest-visible mapping,
page allocation, endian handling and reservations remain in the common Xenon
memory model. Vulkan and D3D12 consume that common model through renderer
backends.
