# CPU research provenance

Xenon CPU is an independent static-recompilation implementation. Reference projects and
public architectural documents were used to identify guest-visible behavior and validate
coverage; their implementation structures are not runtime dependencies of Xenon.

Primary research references used during CPU bring-up:

- IBM / PowerPC architectural documentation for fixed-point, floating-point, control,
  synchronization and vector semantics.
- AltiVec Technology Programming Environments Manual for VMX element, vector memory and
  vector arithmetic behavior.
- Xenia (`xenia-project/xenia`, BSD-licensed) as a broad Xbox 360 PPC/VMX128 research
  reference and opcode-coverage cross-check.
- ReXGlue / AC6_recomp (`sal063/AC6_recomp`) as a static-recompilation reference for the
  instruction families exercised by Ace Combat 6.
- Public VMX128 reverse-engineering documentation for Xbox-specific register encodings and
  instructions.

The checked-in `src/opcode_catalog.inc` is Xenon's declarative architecture metadata:
mnemonic, canonical bit pattern, encoding form and classification. No Xenia source file is
required to build or regenerate Xenon's CPU implementation.

Where a reference implementation differed from the architecture specification, Xenon uses
architectural semantics. One concrete example is `lvebx/lvehx/lvewx`: Xenon implements
AltiVec element-load semantics rather than Xenia's current full-vector-load shortcut.

## GPU Phase 1 public research

The Xenos command-stream layout, PM4 opcode identifiers, register-file extent,
shader command formats, shader 96-bit/48-bit packing and EDRAM geometry were
cross-checked against public Xenia GPU documentation/source and publicly cited
R400/R500-era research. Xenon implements its own types, command processor,
graphics IR, shader container and EDRAM model rather than copying a host GPU
backend.

## Native Xbox 360 recompilation renderer references — GPU 05

For the native-renderer direction, the following public projects were compared
in addition to the Xenia hardware research already listed above:

- `hedge-dev/UnleashedRecomp`: strongest public example found of replacing the
  Xbox 360 GPU path with a purpose-built modern renderer. It supports Vulkan and
  D3D12, bindless resources, shader specialization and recompilation-oriented
  draw translation.
- `hedge-dev/XenosRecomp` and active forks: reference for decoding Xenos shader
  containers/instructions and producing HLSL suitable for DXIL or SPIR-V. Its
  own documentation states that parts are Unleashed-specific or incomplete, so
  Xenon must generalize and validate all behaviour rather than assume title-
  specific mappings are universal.
- `rexglue/rexglue-sdk` plus Project8Recomp, The Simpsons Game Recompiled,
  reDAHM, GTA IV recomp and other ReXGlue ports: useful compatibility and Xenos
  behaviour references. Their general renderer remains Xenia-derived and is not
  the target architecture for Xenon.
- `sal063/AC6_recomp`: useful Ace Combat 6 title-specific bring-up reference for
  Project Gracemeria later, but no AC6-specific behaviour is allowed in the
  generic Xenon GPU frontend.

The rule remains: external projects may establish hardware behaviour and useful
translation techniques, but Xenon owns its graphics IR and native backend
architecture.

### Binned draw packet completion

GPU 05 binned draw lowering was cross-checked against public AMD/Yamato/Adreno
A2xx PM4 material in addition to Xenia's Xenos opcode/register research. The
A2xx command stream documents the six-dword `DRAW_INDX_BIN` ordering as viz
query, draw initiator, bin base, bin size, DMA base and DMA size. This matches
the Xenos opcode family and its `SET_BIN_BASE_OFFSET` description. Public native
Xbox 360 recompilation renderer work also demonstrates that host-native full
render targets can execute a binned draw once while treating guest bin-ID
predication as an original tiled-rendering implementation detail. Xenon retains
the decoded bin metadata in IR but does not emulate the guest binning engine.

## Generic shader decoder — GPU 06

Xenos instruction layouts and opcode values were cross-checked against Xenia's
public `ucode.h`, whose comments identify the official XNA shader assembler,
public ATI R400 patent-dispute material, AMD R600 ISA documentation and
Freedreno A2xx research as underlying sources. XenosRecomp was used to compare
the native-recompilation boundary and identify title-specific limitations to
avoid. Xenon's decoder and reflection model are independent, backend-neutral,
and contain no Unleashed- or game-specific semantics, locations or bindings.

## Native backend foundation — GPU 01–04 restoration

Vulkan device/queue, synchronization and memory behavior was based on the
Khronos Vulkan 1.3 specification and headers. Direct3D 12 device creation,
command submission, fences, resources and state transitions were checked
against Microsoft Learn documentation. Xenia's public D3D12 command processor,
shared-memory implementation and GPU documentation were used only as hardware
behavior and resource-lifetime cross-checks. UnleashedRecomp remains the public
native-renderer reference for the overall Vulkan/D3D12 direction. Xenon's
backends consume its own normalized IR and do not adopt an emulated Xenos
command processor.

## HLSL and DXC lowering — GPU 07

Xenos ALU, fetch, export and sequencer behavior was cross-checked against
Xenia's public `ucode.h` and shader translator. XenosRecomp was used as a
read-only reference for the practical `IDxcCompiler3` boundary and the shared
DXIL/SPIR-V direction. Xenon's HLSL ABI, control-flow state machine, compiler
API, cache key and backend integration are independent implementations. In
particular, no Unleashed-specific vertex locations, semantics, bindings or
resource assumptions are present.

## Native resource ABI — GPU 08

Fetch-constant and render-state bitfields were cross-checked against Xenia's
public Xenos register and fetch descriptor definitions. Vulkan descriptor-set
layout rules follow the Khronos Vulkan 1.3 API, while D3D12 root signatures,
separate sampler/resource heaps and device-specific descriptor increments
follow Microsoft documentation. Xenia and XenosRecomp remain read-only
behavioral references; Xenon's `ResourceStateTracker`, hashes, constant layout
and backend resource ownership are independent and host-neutral.

## Native texture realization — GPU 09

The 64-format surface catalogue, mip storage rules and tiled address behavior
were cross-checked against Xenia's public Xenos format and texture-address
research, including its cited AMD AddrLib lineage. Vulkan image layout/copy
rules follow the Khronos Vulkan 1.3 API; D3D12 upload footprints and SRV rules
follow Microsoft documentation. Xenon's layout objects, detiler, cache and
backend image ownership are independent implementations and contain no game-
specific format or binding assumptions.

## Third GPU correctness audit — 2026-09-18

Current Xenia packet execution, EDRAM transfer/sample ordering and texture-fetch
definitions were consulted read-only for Type-3 predication, standard host MSAA
sample mapping and fetch result exponent adjustment. FH1 recomp findings were
used as real-title evidence for predicated tiled passes, empty resolves and
exponent-adjusted lighting. The resulting behavior lives in Xenon's common
command, EDRAM and shader-resource layers rather than in either native backend;
no emulator command processor or title-specific binding was imported.

## GPU completion audit — native fixed function and resolves

The current Xenia `registers.h`, `draw_util.h/.cc`, DXBC output-merger lowering
and Vulkan/D3D12 pipeline code were used read-only to verify polygon-offset
units, alpha-to-mask thresholds and quadrant ordering, target-zero alpha-test
behavior, independent stencil state, copy-mode register fields, sample
selection sanitization and resolve-rectangle raster rules. UnleashedRecomp's
native renderer was cross-checked for frame-context/upload allocation,
descriptor, barrier and alpha-test practices. Xenon retains independent common
IR, shader generation, resource planning and native backends; no command-
processor emulation or title-specific bindings were imported.
