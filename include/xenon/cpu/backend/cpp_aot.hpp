#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::backend {

// Explicit static-AOT link supplied by function analysis/module activation.
// Merely having a constant guest target is not sufficient: callers only add a
// binding after proving the target is a safe native translation to call
// directly. Noncanonical returns are still propagated at runtime.
struct DirectCallBinding {
  GuestAddress guest_target{};
  std::string native_symbol{};
  // Set for a runtime routine (a register save/restore helper, a native
  // replacement): the header declaring `native_symbol`, a qualified name the
  // generated code includes instead of forward-declaring it. A header-only
  // template the compiler can then inline into the call site.
  std::string header{};
  // The routine returns to its caller and never calls or branches on (the
  // register save/restore helpers). A tail branch to it becomes a direct
  // tail call; to anything else it stays a Branch result the caller follows
  // in a loop, since guest functions that tail-branch to each other in a
  // cycle would otherwise recurse without bound.
  bool leaf{};

  // For a register save/restore helper: the moves it makes, so a function
  // holding guest registers in locals makes them inline, from and into its
  // locals, instead of writing every promoted register back for the call.
  // Registers `spill_first`..31 (64..127 for VMX128) go to or come from
  // base + spill_offset + (register - 14 or 64) * width, the base being r1
  // (GPR, FPR) or r12 (VMX, VMX128); a GPR save also stores r12 (the
  // caller's LR) as a word at r1 + spill_link_offset, and the GPR restore
  // reloads LR from there and returns through it.
  enum class Spill : std::uint8_t {
    None,
    SaveGprLr, RestoreGprLr,
    SaveFpr, RestoreFpr,
    SaveVmx, RestoreVmx,
    SaveVmx128, RestoreVmx128,
  };
  Spill spill{Spill::None};
  std::uint32_t spill_first{};
  std::int32_t spill_offset{};
  std::int32_t spill_link_offset{};
};

// Stable generated symbol used for a dispatchable alternate guest entry into
// a canonical compiled function/region. Kept in the backend API so registry
// generation and C++ emission cannot drift to different naming conventions.
[[nodiscard]] std::string alternate_entry_symbol(std::string_view function_name,
                                                 GuestAddress entry);

// Portable native AOT backend. It emits C++20 containing no guest decoder or
// PPC runtime dispatch. The host C++ compiler performs final x86-64/ARM64
// instruction selection and register allocation.
class CppAotBackend {
 public:
  // Register promotion (whole-function path): the guest GPRs, FPRs and
  // vector registers a function uses live in C++ locals for the whole
  // function, written back to CpuState before anything else can look at it
  // (calls, hooks, traps, system calls, a taken safepoint) and reloaded
  // after, and written back on every exit, exceptions included. Guest
  // stores go through a byte pointer that may alias anything, so with the
  // registers in CpuState the host compiler had to reload every one after
  // every store. Off, every access goes to CpuState as before.
  //
  // Indirect call and branch sites (whole-function path) each keep a one-entry
  // cache of the native function their last guest target resolved to
  // (aot::lookup_cached), valid while the host dispatch generation stands.
  explicit CppAotBackend(bool promote_registers = true,
                         bool cache_indirect_sites = true) noexcept
      : promote_registers_(promote_registers),
        cache_indirect_sites_(cache_indirect_sites) {}

  [[nodiscard]] std::string emit_function(const ir::Block& block,
                                          std::string_view function_name) const;
  [[nodiscard]] std::string emit_translation_unit(const ir::Block& block,
                                                   std::string_view function_name) const;

  // Whole-function AOT path. Local direct branches are emitted as native host
  // control flow rather than returning to an instruction dispatcher.
  // `hook_symbols[n]` names the module routine for HostHook ordinal n.
  [[nodiscard]] std::string emit_function(
      const ir::Function& function, std::string_view function_name,
      std::span<const DirectCallBinding> direct_calls = {},
      std::span<const GuestAddress> alternate_entries = {},
      std::span<const std::string> hook_symbols = {}) const;
  [[nodiscard]] std::string emit_translation_unit(
      const ir::Function& function, std::string_view function_name,
      std::span<const DirectCallBinding> direct_calls = {},
      std::span<const GuestAddress> alternate_entries = {},
      std::span<const std::string> hook_symbols = {}) const;

  // Every statically known call target of the function (direct and
  // conditional calls with a constant target) that is not one of its own
  // basic blocks - exactly the targets emit_function() would consult
  // direct_calls for. Sorted and unique, so a caller can bind only these
  // instead of declaring every compiled symbol in every translation unit.
  [[nodiscard]] static std::vector<GuestAddress> static_call_targets(
      const ir::Function& function);
  // The constant targets of the function's tail branches (b and conditional
  // bc that leave it): where a leaf binding (DirectCallBinding::leaf) is
  // called directly. Sorted and unique.
  [[nodiscard]] static std::vector<GuestAddress> static_tail_branch_targets(
      const ir::Function& function);

  [[nodiscard]] bool promotes_registers() const noexcept { return promote_registers_; }
  [[nodiscard]] bool caches_indirect_sites() const noexcept { return cache_indirect_sites_; }

 private:
  bool promote_registers_{true};
  bool cache_indirect_sites_{true};
};

}  // namespace xenon::cpu::backend
