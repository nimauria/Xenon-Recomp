#include "xenon/cpu/dynamic_fallback.hpp"

#include "cpu/codegen/fallback/fallback_interpreter.hpp"

namespace xenon::cpu {
namespace {

// The original single dispatch chain, regrouped by instruction class. The
// classes accept disjoint mnemonics, so the order only affects how many
// string comparisons a lookup performs; it follows the original chain.
[[nodiscard]] bool execute_simple(const DecodedInstruction& i,
                                  ExecutionContext& context) {
  return fallback::execute_scalar_memory(i, context) ||
         fallback::execute_integer(i, context) ||
         fallback::execute_system(i, context) ||
         fallback::execute_floating_point(i, context) ||
         fallback::execute_vector(i, context);
}

}  // namespace

DynamicFallbackExecutor::DynamicFallbackExecutor(DynamicFallbackConfig config,
                                                 ObservationCallback observer)
    : config_(config), observer_(std::move(observer)) {
  config_.max_instructions_per_dispatch =
      std::max<std::uint32_t>(1u, config_.max_instructions_per_dispatch);
  config_.max_nested_calls = std::max<std::uint32_t>(1u, config_.max_nested_calls);
}

void DynamicFallbackExecutor::bind(ExecutionContext& context) noexcept {
  context.dynamic_fallback_executor = this;
  context.dynamic_fallback = &DynamicFallbackExecutor::callback;
}

DynamicFallbackResult DynamicFallbackExecutor::callback(
    void* executor, ExecutionContext& context, GuestAddress target,
    CompiledLookupKind kind) {
  if (!executor) return {};
  return static_cast<DynamicFallbackExecutor*>(executor)->try_execute(
      context, target, kind);
}

DynamicFallbackResult DynamicFallbackExecutor::try_execute(
    ExecutionContext& context, GuestAddress target, CompiledLookupKind kind) {
  const auto site = context.state.cia;
  auto run_result = run(context, target, kind, 0u);
  if (run_result.public_result.handled) {
    executed_blocks_.fetch_add(1u, std::memory_order_relaxed);
    executed_instructions_.fetch_add(run_result.instructions,
                                     std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(pc_hits_mutex_);
      ++pc_hits_[target];
    }
    if (observer_) {
      observer_(DynamicFallbackObservation{
          target, site, run_result.exit, kind, run_result.reason,
          run_result.instructions, run_result.fingerprint});
    }
  }
  return run_result.public_result;
}

std::size_t DynamicFallbackExecutor::fallback_unique_pc_count() const {
  std::lock_guard<std::mutex> lock(pc_hits_mutex_);
  return pc_hits_.size();
}

std::vector<std::pair<GuestAddress, std::uint64_t>>
DynamicFallbackExecutor::fallback_hot_pcs(std::size_t top_n) const {
  std::vector<std::pair<GuestAddress, std::uint64_t>> hits;
  {
    std::lock_guard<std::mutex> lock(pc_hits_mutex_);
    hits.reserve(pc_hits_.size());
    for (const auto& [pc, count] : pc_hits_) hits.emplace_back(pc, count);
  }
  std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;  // Deterministic tie-break for reproducible reports.
  });
  if (hits.size() > top_n) hits.resize(top_n);
  return hits;
}

DynamicFallbackExecutor::RunResult DynamicFallbackExecutor::run(
    ExecutionContext& context, GuestAddress target, CompiledLookupKind kind,
    std::uint32_t depth) {
  RunResult out{};
  out.exit = target;
  out.fingerprint = kFnvOffset;

  if ((target & 3u) != 0u) {
    out.reason = DynamicFallbackStopReason::InvalidTarget;
    return out;
  }

  const auto first_stamp = context.memory.executable_page_stamp(target);
  if (!first_stamp.executable()) return out;  // Not ours; imports may handle it.

  // A recognized import thunk's bytes are loader-owned placeholder metadata
  // (ordinal/attributes/record-type), never real guest PPC, even though the
  // page containing them is marked executable (native XEX import records
  // commonly live inside the same executable section as real code). Decoding
  // them here would hit the exact "unsupported instruction" trap this
  // interpreter exists to avoid, for what is really a perfectly resolvable
  // import call - route straight to the runtime's own import dispatch
  // instead, the same way execute_call() below already falls through to
  // context.runtime.call() as its own last resort for a target this
  // interpreter cannot otherwise handle. Checked once here (covering both
  // try_execute()'s top-level entry and this same run() being re-entered for
  // a nested call below) rather than on every compiled-call hot path.
  if (context.runtime.is_recognized_import_thunk(target)) {
    out.public_result.handled = true;
    out.public_result.result = context.runtime.call(target, context.state, context.memory);
    return out;
  }

  out.public_result.handled = true;
  if (depth > config_.max_nested_calls) {
    out.reason = DynamicFallbackStopReason::CallDepthLimit;
    out.public_result.result = {FlowReason::Trap, target, kFallbackCallDepthDetail};
    return out;
  }
  const auto entry_return = static_cast<GuestAddress>(context.state.lr & ~3ull);
  std::unordered_map<GuestAddress, ExecutablePageStamp> page_stamps;
  Decoder decoder;
  GuestAddress pc = target;

  const auto trap_result = [&](DynamicFallbackStopReason reason,
                               std::uint32_t detail) {
    out.reason = reason;
    out.exit = pc;
    out.public_result.result = {FlowReason::Trap, pc, detail};
  };

  const auto validate_page = [&](GuestAddress address) -> bool {
    const auto page = address & kGuestPageMask;
    const auto current = context.memory.executable_page_stamp(address);
    if (!current.executable()) return false;
    const auto [it, inserted] = page_stamps.emplace(page, current);
    if (!inserted && it->second != current) {
      source_invalidations_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::SourceChanged,
                  kFallbackSourceChangedDetail);
      return false;
    }
    return true;
  };

  const auto finish_call = [&](const ExecutionResult& result,
                               GuestAddress expected_return,
                               GuestAddress& next_pc) -> std::optional<ExecutionResult> {
    if (result.reason == FlowReason::Return) {
      if (result.next_address == expected_return) {
        next_pc = expected_return;
        return std::nullopt;
      }
      return result;
    }
    if (result.reason == FlowReason::Fallthrough) {
      next_pc = expected_return;
      return std::nullopt;
    }
    return result;
  };

  const auto execute_call = [&](GuestAddress call_target,
                                GuestAddress expected_return,
                                GuestAddress& next_pc) -> std::optional<ExecutionResult> {
    if (auto* native = context.lookup_compiled(call_target, CompiledLookupKind::Call)) {
      const auto result = native(context);
      return finish_call(result, expected_return, next_pc);
    }
    if (context.memory.executable_page_stamp(call_target).executable()) {
      auto nested = run(context, call_target, CompiledLookupKind::Call, depth + 1u);
      out.instructions += nested.instructions;
      out.fingerprint ^= nested.fingerprint + 0x9E3779B97F4A7C15ull +
                         (out.fingerprint << 6u) + (out.fingerprint >> 2u);
      if (nested.public_result.handled) {
        // Nested dynamically discovered callees are first-class learned facts,
        // not hidden inside the outer observation. Instruction totals remain
        // accounted once by the outer dispatch; block counts/observations are
        // emitted here for each nested entry.
        executed_blocks_.fetch_add(1u, std::memory_order_relaxed);
        if (observer_) {
          observer_(DynamicFallbackObservation{
              call_target, pc, nested.exit, CompiledLookupKind::Call,
              nested.reason, nested.instructions, nested.fingerprint});
        }
        return finish_call(nested.public_result.result, expected_return, next_pc);
      }
    }
    const auto result = context.runtime.call(call_target, context.state, context.memory);
    return finish_call(result, expected_return, next_pc);
  };

  // An UNLINKED branch (bx/bcx/bclrx/bcctrx with LK=0 - a tail call/tail
  // branch, common compiler output for e.g. "if (x) return f();") never goes
  // through execute_call() above, so it never re-entered run() and never hit
  // the is_recognized_import_thunk() check at this function's own top for
  // the ORIGINAL entry target. Without this, a tail branch landing on an
  // import-thunk address (the exact same loader-owned placeholder bytes a
  // `bl` into the same address is already correctly routed around) would
  // fall through to `next_pc = branch_target` and get decoded as PPC on the
  // next loop iteration, producing a spurious "unsupported instruction"
  // trap for what is really a perfectly resolvable import call - real guest
  // code make no ABI distinction between reaching an import via `bl` or via
  // a tail branch, so neither should Xenon.
  const auto execute_tail_branch = [&](GuestAddress branch_target) -> bool {
    if (auto* native = context.lookup_compiled(branch_target, CompiledLookupKind::Branch)) {
      out.reason = DynamicFallbackStopReason::CompiledHandoff;
      out.exit = branch_target;
      out.public_result.result = native(context);
      return true;
    }
    if (context.runtime.is_recognized_import_thunk(branch_target)) {
      out.reason = DynamicFallbackStopReason::CompiledHandoff;
      out.exit = branch_target;
      out.public_result.handled = true;
      auto result = context.runtime.call(branch_target, context.state, context.memory);
      // XenonSession::call()'s Fallthrough convention ("the export ran;
      // continue at state.cia") is only meaningful to a caller that already
      // knows the real continuation address itself - a linked `bl` call site
      // does (its own expected_return, computed from pc+4 - see
      // execute_call()/finish_call() above), so it can safely ignore
      // Fallthrough's next_address field entirely. An UNLINKED tail branch
      // has no such fallback: state.cia at this point is still this bx
      // instruction's own address (nothing updates it before the call), so
      // forwarding Fallthrough as-is would make the OUTER dispatch loop
      // re-dispatch this exact same branch forever. A tail branch never
      // expects control back at all - it IS this function's own return, so
      // completing it means returning to the function's real caller via the
      // untouched LR, exactly as the equivalent real guest bytes
      // (`bl <import-thunk-copy>`/`blr`, or the loader-owned thunk's own
      // `mtctr`/`bctr`) would.
      if (result.reason == FlowReason::Fallthrough) {
        result = {FlowReason::Return, static_cast<GuestAddress>(context.state.lr & ~3ull), 0u};
      }
      out.public_result.result = result;
      return true;
    }
    return false;
  };

  for (std::uint32_t local_count = 0u;
       local_count < config_.max_instructions_per_dispatch; ++local_count) {
    if (!validate_page(pc)) {
      if (out.reason != DynamicFallbackStopReason::SourceChanged)
        trap_result(DynamicFallbackStopReason::InvalidTarget,
                    kFallbackInvalidTargetDetail);
      return out;
    }

    context.state.cia = pc;
    context.state.nia = pc + 4u;
    const auto word = context.memory.fetch32_be(pc);
    // Revalidate after the fetch as well. A host thread may rewrite/remap the
    // executable page between the pre-fetch stamp check and the actual read;
    // observations must never bless a mixed-generation block.
    if (!validate_page(pc)) {
      if (out.reason != DynamicFallbackStopReason::SourceChanged)
        trap_result(DynamicFallbackStopReason::InvalidTarget,
                    kFallbackInvalidTargetDetail);
      return out;
    }
    out.fingerprint = fingerprint_step(out.fingerprint, word);
    ++out.instructions;
    const auto insn = decoder.decode(pc, word);
    if (!insn.valid()) {
      unsupported_instructions_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::UnsupportedInstruction,
                  kFallbackUnsupportedDetail);
      return out;
    }

    const auto mnemonic = insn.mnemonic();
    GuestAddress next_pc = pc + 4u;

    if (mnemonic == "bx") {
      const auto branch_target = insn.direct_branch_target() & ~3u;
      if (insn.lk()) {
        context.state.lr = pc + 4u;
        if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
          out.reason = escaped->reason == FlowReason::Trap
                           ? DynamicFallbackStopReason::Trap
                           : DynamicFallbackStopReason::CompiledHandoff;
          out.exit = escaped->next_address;
          out.public_result.result = *escaped;
          return out;
        }
      } else {
        if (execute_tail_branch(branch_target)) return out;
        next_pc = branch_target;
      }
    } else if (mnemonic == "bcx") {
      const bool taken = branch_condition(insn, context.state, true);
      if (insn.lk()) context.state.lr = pc + 4u;
      if (taken) {
        const auto branch_target = insn.direct_branch_target() & ~3u;
        if (insn.lk()) {
          if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
            out.reason = escaped->reason == FlowReason::Trap
                             ? DynamicFallbackStopReason::Trap
                             : DynamicFallbackStopReason::CompiledHandoff;
            out.exit = escaped->next_address;
            out.public_result.result = *escaped;
            return out;
          }
        } else {
          if (execute_tail_branch(branch_target)) return out;
          next_pc = branch_target;
        }
      }
    } else if (mnemonic == "bclrx" || mnemonic == "bcctrx") {
      const bool use_ctr = mnemonic == "bclrx";
      const auto raw_target = mnemonic == "bclrx" ? context.state.lr : context.state.ctr;
      const auto branch_target = static_cast<GuestAddress>(raw_target & ~3ull);
      const bool taken = branch_condition(insn, context.state, use_ctr);
      if (insn.lk()) context.state.lr = pc + 4u;
      if (taken) {
        if (insn.lk()) {
          if (const auto escaped = execute_call(branch_target, pc + 4u, next_pc)) {
            out.reason = escaped->reason == FlowReason::Trap
                             ? DynamicFallbackStopReason::Trap
                             : DynamicFallbackStopReason::CompiledHandoff;
            out.exit = escaped->next_address;
            out.public_result.result = *escaped;
            return out;
          }
        } else if (mnemonic == "bclrx" && kind == CompiledLookupKind::Call &&
                   branch_target == entry_return) {
          // NIA is architecturally the taken return target. Gen 8 differential
          // verification caught the fallback returning the correct
          // ExecutionResult while leaving CpuState::nia at pc+4.
          context.state.nia = branch_target;
          out.reason = DynamicFallbackStopReason::Returned;
          out.exit = branch_target;
          out.public_result.result = {FlowReason::Return, branch_target, 0u};
          return out;
        } else if (mnemonic == "bclrx" &&
                   !context.memory.executable_page_stamp(branch_target).executable() &&
                   !context.lookup_compiled(branch_target, CompiledLookupKind::Branch) &&
                   !context.runtime.is_recognized_import_thunk(branch_target)) {
          // A return to an address that is not guest code at all (the thread-exit
          // sentinel, a host-owned return address) cannot be interpreted. This is
          // reached after an instruction-budget yield re-entered the function as a
          // Branch, where entry_return no longer identifies the real caller. Hand
          // the return back to the dispatcher, which owns those sentinels.
          context.state.nia = branch_target;
          out.reason = DynamicFallbackStopReason::Returned;
          out.exit = branch_target;
          out.public_result.result = {FlowReason::Return, branch_target, 0u};
          return out;
        } else {
          if (execute_tail_branch(branch_target)) return out;
          next_pc = branch_target;
        }
      }
    } else if (mnemonic == "sc") {
      const auto result = context.runtime.syscall((insn.word >> 5u) & 0x7Fu,
                                                  context.state, context.memory);
      out.reason = DynamicFallbackStopReason::Syscall;
      out.exit = result.next_address;
      out.public_result.result = result;
      return out;
    } else if (mnemonic == "td" || mnemonic == "tdi" || mnemonic == "tw" ||
               mnemonic == "twi") {
      const auto to = (insn.word >> 21u) & 31u;
      const auto ra = (insn.word >> 16u) & 31u;
      const auto rb = (insn.word >> 11u) & 31u;
      const bool word_form = mnemonic == "tw" || mnemonic == "twi";
      const bool immediate = mnemonic == "tdi" || mnemonic == "twi";
      const auto rhs = immediate
                           ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                 static_cast<std::int16_t>(insn.word & 0xFFFFu)))
                           : context.state.gpr[rb];
      if (aot::trap_condition(to, context.state.gpr[ra], rhs, word_form)) {
        const auto result = context.runtime.trap(to, context.state, context.memory);
        out.reason = DynamicFallbackStopReason::Trap;
        out.exit = result.next_address;
        out.public_result.result = result;
        return out;
      }
    } else if (!execute_simple(insn, context)) {
      unsupported_instructions_.fetch_add(1u, std::memory_order_relaxed);
      trap_result(DynamicFallbackStopReason::UnsupportedInstruction,
                  kFallbackUnsupportedDetail);
      return out;
    }

    pc = next_pc;
  }

  // The per-dispatch budget bounds how long one host call runs, it is not a
  // fault. Real guest code legitimately runs far more than the budget in one
  // stretch (table initialisers, memcpy-style loops - Ace Combat 6's startup
  // walks an 80-entry constructor table right after entry), so yield at the
  // next instruction as an ordinary Branch: all architectural state is already
  // in CpuState/guest memory, and the dispatcher re-enters here (or in a
  // compiled function) at `pc`. A truly endless guest loop is still bounded by
  // the session's top-level dispatch limit.
  out.reason = DynamicFallbackStopReason::InstructionLimit;
  out.exit = pc;
  context.state.cia = pc;
  context.state.nia = pc;
  out.public_result.result = {FlowReason::Branch, pc, 0u};
  return out;
}

bool dynamic_fallback_supports(const DecodedInstruction& insn, MemoryPort& scratch_memory) {
  if (!insn.valid()) return false;
  const auto m = insn.mnemonic();
  // Control flow and traps the block loop handles inline (see the dispatch chain
  // in DynamicFallbackExecutor::run_block).
  if (m == "bx" || m == "bcx" || m == "bclrx" || m == "bcctrx" || m == "sc" || m == "td" ||
      m == "tdi" || m == "tw" || m == "twi") {
    return true;
  }
  NullRuntimeServices runtime;
  CpuState state{};
  ExecutionContext context(state, scratch_memory, runtime);
  state.cia = insn.address;
  state.nia = insn.address + 4u;
  try {
    return execute_simple(insn, context);
  } catch (...) {
    // The interpreter recognized the instruction and got as far as touching the
    // scratch memory / an operand it could not use.
    return true;
  }
}

}  // namespace xenon::cpu
