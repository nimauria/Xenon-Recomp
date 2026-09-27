#pragma once

// Real Xenon-owned implementations for the NativeReplacementKind identities a
// module's analysis metadata may bind a guest address to (Part 1.10 of the
// Gracemeria readiness pass - see analysis_schema.hpp). Each function has
// exactly the CPU V2 NativeCompiledEntry ABI (xenon/cpu/runtime.hpp) - guest
// integer arguments in gpr[3..], guest integer return in gpr[3], and a
// `blr`-shaped ExecutionResult (FlowReason::Return to state.lr) - so
// generate_project() can install one directly into a module's generated
// lookup_compiled() switch in place of compiling the (often unusual/
// hand-tuned) original guest CRT bytes at that address. Xenon owns these
// implementations; Project Gracemeria only ever supplies the guest_address ->
// kind mapping (see NativeReplacement).
//
// Heap identities are backed by the current KernelProcess guest heap, which
// allocates real Memory V2 guest virtual addresses and owns them for the
// process lifetime.

#include "xenon/cpu/runtime.hpp"
#include "xenon/recomp/analysis_schema.hpp"

namespace xenon::recomp::native_replacements {

// Returns the real compiled-entry implementation for `kind`, or nullptr only
// when `kind` is Unsupported.
[[nodiscard]] cpu::NativeCompiledEntry entry_for(analysis::NativeReplacementKind kind) noexcept;

cpu::ExecutionResult memcpy_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult memmove_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult memset_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult memcpy_checked_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult memmove_checked_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult memcmp_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strlen_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strncmp_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strncpy_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strchr_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strstr_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strrchr_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult strcpy_checked_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult heap_allocate_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult heap_free_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult heap_size_v2(cpu::ExecutionContext& context);
cpu::ExecutionResult heap_reallocate_v2(cpu::ExecutionContext& context);

}  // namespace xenon::recomp::native_replacements
