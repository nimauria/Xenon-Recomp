# x86-64 backend boundary

The current production CPU path is the portable C++ AOT backend in
`src/cpu/codegen`. It lowers Xenon IR to C++ at recompilation time, and the host
compiler produces native x86-64 code without a guest-opcode interpreter.

This directory is reserved for genuinely x86-64-specific work after the native
GPU is complete: measured SIMD lowering, ABI thunks, code-cache support and, if
startup/runtime profiling justifies it, a direct machine-code emitter. The
portable AOT backend remains the correctness reference and is not moved here
because it is also valid for ARM64 hosts.
