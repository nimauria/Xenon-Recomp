# Xenos shader decoder and reflection (GPU 06)

GPU 06 turns loaded Xenos shader microcode into a host-independent model. It
does not emit HLSL, DXIL or SPIR-V and does not emulate a command processor.

`ShaderDecoder::decode` consumes the existing `ShaderProgram`, preserves its
stage, start slot and stable GPU 05 hash, and produces:

- decoded 48-bit control flow, including exec ranges, calls, jumps, predicates,
  loops, boolean/loop constants and allocation records;
- scheduled 96-bit ALU, vertex-fetch and texture-fetch instructions;
- vector/scalar opcodes, operands, swizzles, masks, relative addressing,
  clamping, exports and predicates;
- vertex formats, strides and offsets plus texture dimensions, LOD and offsets;
- reflection for constants, resources, exports, temporary registers, dynamic
  addressing, predication, loops, memory export and pixel-kill usage;
- diagnostics for incomplete or invalid microcode ranges.

Every `ShaderLoad` IR command carries both the original `ShaderProgram` and its
decoded representation. Vulkan and D3D12 therefore never need to parse Xenos
microcode themselves.

The representation stores architectural opcode values rather than HLSL
spellings. GPU 07 will define exact operation lowering to common HLSL and invoke
DXC for SPIR-V or DXIL.

No vertex locations, semantics, bindings, title IDs or game-specific shader
assumptions are hardcoded.
