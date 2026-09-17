# Native graphics backends (GPU 01–04 restoration)

Xenon keeps PM4, Xenos registers, draw IR, shader IR, EDRAM and guest memory in
the common graphics layer. Vulkan and Direct3D 12 consume that state as native
renderers; neither backend implements an Xbox GPU command processor.

## Runtime and development discovery

`discover_backend_capabilities` distinguishes an installed graphics runtime
from development files. On Windows it probes `vulkan-1.dll`, reports its API
version, and independently checks hardware D3D12 feature-level 12_0 support.
The Vulkan target is built only when CMake finds the Vulkan SDK. End users need
the loader and driver, not the full SDK; developers need the SDK headers and
import library.

## Vulkan

`Xenon::GraphicsVulkan` contains Vulkan 1.3 instance/device and graphics-queue
selection, timeline-semaphore submission, synchronization2, buffer allocation,
and a bounded-staging 512 MiB Xbox physical-memory mirror. Dynamic rendering,
timeline semaphores and synchronization2 are required explicitly. Dirty guest
pages are copied using locked memory snapshots. The Windows validation fixture
runs with `VK_LAYER_KHRONOS_validation` and verifies a real GPU readback from
the physical-memory mirror.

## Direct3D 12

`Xenon::GraphicsD3D12` selects a high-performance hardware adapter at feature
level 12_0, records device capabilities, owns a direct queue/allocator/list and
fence, provides committed buffer allocation, and implements the same bounded
dirty-page guest-memory mirror. The target uses Windows SDK components only.

Both backend classes implement the common `gpu::Backend` contract. GPU 08 adds
the shared Xenos resource-state decoder, stage-correct shader constant uploads,
native descriptor/root layouts, guest-memory shader bindings and stable native
pipeline-state identities. Concrete image-backed pipelines and draw execution
follow once GPU 09 supplies exact texture formats and tiled layout conversion.
