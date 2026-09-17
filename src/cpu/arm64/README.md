# ARM64 backend boundary

ARM64 and Android bring-up is deliberately deferred until Ace Combat 6 runs
end to end through the Windows x86-64 path. The portable C++ AOT backend is
architecture-neutral and will be the initial correctness path on ARM64.

Later architecture-specific work belongs here: ABI thunks, NEON lowering,
code-cache policy and any direct emitter selected after profiling. No ARM64
implementation should fork Xenon guest semantics or duplicate the shared IR.
