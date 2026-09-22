#include "xenon/recomp/native_replacements.hpp"

#include <algorithm>
#include <vector>

#include "xenon/kernel/process.hpp"

namespace xenon::recomp::native_replacements {

namespace {

using cpu::ExecutionContext;
using cpu::ExecutionResult;
using cpu::FlowReason;
using cpu::GuestAddress;

// Every replacement ends its guest-visible effects exactly like a compiled
// `blr`: return to state.lr with whatever integer result was placed in
// gpr[3] (see backend_cpp_aot.cpp's Op::Return codegen, which this mirrors
// byte-for-byte so a caller's `bl` dispatch cannot tell the difference
// between calling real compiled guest code and one of these).
ExecutionResult guest_return(cpu::CpuState& state) {
  return {FlowReason::Return, static_cast<GuestAddress>(state.lr & ~3ull), 0};
}

std::vector<std::byte> read_guest_bytes(cpu::MemoryPort& memory, GuestAddress address,
                                        std::uint32_t count) {
  std::vector<std::byte> bytes(count);
  memory.read_bytes(address, bytes);
  return bytes;
}

// Reads a NUL-terminated guest string, bounded by `max_length` (0 = no
// bound) to guarantee termination even against a malformed/unterminated
// guest buffer - a genuine safety property real hardware's string routines
// do not need but a host-side loop must have.
std::string read_guest_cstring(cpu::MemoryPort& memory, GuestAddress address,
                               std::uint32_t max_length = 0xFFFFu) {
  std::string result;
  for (std::uint32_t i = 0; i < max_length; ++i) {
    const auto byte = memory.read8(address + i);
    if (byte == 0) break;
    result.push_back(static_cast<char>(byte));
  }
  return result;
}

}  // namespace

ExecutionResult memcpy_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto dst = static_cast<GuestAddress>(state.gpr[3]);
  const auto src = static_cast<GuestAddress>(state.gpr[4]);
  const auto count = static_cast<std::uint32_t>(state.gpr[5]);
  const auto bytes = read_guest_bytes(context.memory, src, count);
  context.memory.write_bytes(dst, bytes);
  state.gpr[3] = dst;  // memcpy returns dst
  return guest_return(state);
}

ExecutionResult memmove_v2(ExecutionContext& context) {
  // Overlap-safe: read the full source range into a host buffer first (an
  // independent copy), then write it out - matching real memmove semantics
  // regardless of whether [src,src+count) and [dst,dst+count) overlap.
  return memcpy_v2(context);
}

ExecutionResult memset_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto dst = static_cast<GuestAddress>(state.gpr[3]);
  const auto value = static_cast<std::uint8_t>(state.gpr[4] & 0xFFu);
  const auto count = static_cast<std::uint32_t>(state.gpr[5]);
  context.memory.fill_bytes(dst, count, value);
  state.gpr[3] = dst;  // memset returns dst
  return guest_return(state);
}

ExecutionResult memcpy_checked_v2(ExecutionContext& context) {
  // memcpy_s(dst, dst_size, src, count) -> errno_t (0 on success). Refuses
  // to copy past dst_size, matching the real CRT contract rather than
  // silently behaving like unchecked memcpy.
  auto& state = context.state;
  const auto dst = static_cast<GuestAddress>(state.gpr[3]);
  const auto dst_size = static_cast<std::uint32_t>(state.gpr[4]);
  const auto src = static_cast<GuestAddress>(state.gpr[5]);
  const auto count = static_cast<std::uint32_t>(state.gpr[6]);
  if (count > dst_size) {
    state.gpr[3] = 34u;  // ERANGE
    return guest_return(state);
  }
  const auto bytes = read_guest_bytes(context.memory, src, count);
  context.memory.write_bytes(dst, bytes);
  state.gpr[3] = 0u;
  return guest_return(state);
}

ExecutionResult memmove_checked_v2(ExecutionContext& context) {
  return memcpy_checked_v2(context);
}

ExecutionResult memcmp_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto lhs = static_cast<GuestAddress>(state.gpr[3]);
  const auto rhs = static_cast<GuestAddress>(state.gpr[4]);
  const auto count = static_cast<std::uint32_t>(state.gpr[5]);
  const auto lhs_bytes = read_guest_bytes(context.memory, lhs, count);
  const auto rhs_bytes = read_guest_bytes(context.memory, rhs, count);
  std::int32_t result = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto a = std::to_integer<std::uint8_t>(lhs_bytes[i]);
    const auto b = std::to_integer<std::uint8_t>(rhs_bytes[i]);
    if (a != b) { result = static_cast<std::int32_t>(a) - static_cast<std::int32_t>(b); break; }
  }
  state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
  return guest_return(state);
}

ExecutionResult strlen_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto str = static_cast<GuestAddress>(state.gpr[3]);
  const auto text = read_guest_cstring(context.memory, str);
  state.gpr[3] = static_cast<std::uint64_t>(text.size());
  return guest_return(state);
}

ExecutionResult strncmp_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto lhs = static_cast<GuestAddress>(state.gpr[3]);
  const auto rhs = static_cast<GuestAddress>(state.gpr[4]);
  const auto count = static_cast<std::uint32_t>(state.gpr[5]);
  std::int32_t result = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto a = context.memory.read8(lhs + i);
    const auto b = context.memory.read8(rhs + i);
    if (a != b) { result = static_cast<std::int32_t>(a) - static_cast<std::int32_t>(b); break; }
    if (a == 0) break;  // both strings ended identically within count
  }
  state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
  return guest_return(state);
}

ExecutionResult strncpy_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto dst = static_cast<GuestAddress>(state.gpr[3]);
  const auto src = static_cast<GuestAddress>(state.gpr[4]);
  const auto count = static_cast<std::uint32_t>(state.gpr[5]);
  bool ended = false;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t byte = 0;
    if (!ended) {
      byte = context.memory.read8(src + i);
      if (byte == 0) ended = true;
    }
    context.memory.write8(dst + i, byte);  // real strncpy zero-pads the remainder
  }
  state.gpr[3] = dst;  // strncpy returns dst
  return guest_return(state);
}

ExecutionResult strchr_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto str = static_cast<GuestAddress>(state.gpr[3]);
  const auto target = static_cast<std::uint8_t>(state.gpr[4] & 0xFFu);
  GuestAddress result = 0;
  for (GuestAddress offset = 0;; ++offset) {
    const auto byte = context.memory.read8(str + offset);
    if (byte == target) { result = str + offset; break; }
    if (byte == 0) break;  // target not found before the terminator
  }
  state.gpr[3] = result;
  return guest_return(state);
}

ExecutionResult strstr_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto haystack_addr = static_cast<GuestAddress>(state.gpr[3]);
  const auto needle_addr = static_cast<GuestAddress>(state.gpr[4]);
  const auto haystack = read_guest_cstring(context.memory, haystack_addr);
  const auto needle = read_guest_cstring(context.memory, needle_addr);
  GuestAddress result = 0;
  if (needle.empty()) {
    result = haystack_addr;
  } else {
    const auto position = haystack.find(needle);
    if (position != std::string::npos) {
      result = haystack_addr + static_cast<GuestAddress>(position);
    }
  }
  state.gpr[3] = result;
  return guest_return(state);
}

ExecutionResult strrchr_v2(ExecutionContext& context) {
  auto& state = context.state;
  const auto str = static_cast<GuestAddress>(state.gpr[3]);
  const auto target = static_cast<std::uint8_t>(state.gpr[4] & 0xFFu);
  const auto text = read_guest_cstring(context.memory, str);
  GuestAddress result = 0;
  const auto position = text.find_last_of(static_cast<char>(target));
  if (position != std::string::npos) {
    result = str + static_cast<GuestAddress>(position);
  } else if (target == 0) {
    result = str + static_cast<GuestAddress>(text.size());  // strrchr(s, '\0') -> terminator
  }
  state.gpr[3] = result;
  return guest_return(state);
}

ExecutionResult strcpy_checked_v2(ExecutionContext& context) {
  // strcpy_s(dst, dst_size, src) -> errno_t (0 on success).
  auto& state = context.state;
  const auto dst = static_cast<GuestAddress>(state.gpr[3]);
  const auto dst_size = static_cast<std::uint32_t>(state.gpr[4]);
  const auto src = static_cast<GuestAddress>(state.gpr[5]);
  const auto text = read_guest_cstring(context.memory, src, dst_size);
  if (dst_size == 0 || text.size() + 1u > dst_size) {
    if (dst_size > 0) context.memory.write8(dst, 0);
    state.gpr[3] = 34u;  // ERANGE
    return guest_return(state);
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    context.memory.write8(dst + static_cast<GuestAddress>(i), static_cast<std::uint8_t>(text[i]));
  }
  context.memory.write8(dst + static_cast<GuestAddress>(text.size()), 0);
  state.gpr[3] = 0u;
  return guest_return(state);
}


ExecutionResult heap_allocate_v2(ExecutionContext& context) {
  auto& state = context.state;
  auto* process = context.runtime.current_process();
  if (!process) {
    state.gpr[3] = 0;
    return guest_return(state);
  }
  state.gpr[3] = process->guest_heap().allocate(
      static_cast<std::uint32_t>(state.gpr[3]),
      static_cast<std::uint32_t>(state.gpr[4]),
      static_cast<std::uint32_t>(state.gpr[5]));
  return guest_return(state);
}

ExecutionResult heap_free_v2(ExecutionContext& context) {
  auto& state = context.state;
  auto* process = context.runtime.current_process();
  const bool ok = process && process->guest_heap().free(
      static_cast<std::uint32_t>(state.gpr[3]),
      static_cast<std::uint32_t>(state.gpr[4]),
      static_cast<std::uint32_t>(state.gpr[5]));
  state.gpr[3] = ok ? 1u : 0u;
  return guest_return(state);
}

ExecutionResult heap_size_v2(ExecutionContext& context) {
  auto& state = context.state;
  auto* process = context.runtime.current_process();
  state.gpr[3] = process
      ? process->guest_heap().size(static_cast<std::uint32_t>(state.gpr[3]),
                                   static_cast<std::uint32_t>(state.gpr[4]),
                                   static_cast<std::uint32_t>(state.gpr[5]))
      : xenon::kernel::GuestHeapManager::kInvalidSize;
  return guest_return(state);
}

ExecutionResult heap_reallocate_v2(ExecutionContext& context) {
  auto& state = context.state;
  auto* process = context.runtime.current_process();
  if (!process) {
    state.gpr[3] = 0;
    return guest_return(state);
  }
  state.gpr[3] = process->guest_heap().reallocate(
      static_cast<std::uint32_t>(state.gpr[3]),
      static_cast<std::uint32_t>(state.gpr[4]),
      static_cast<std::uint32_t>(state.gpr[5]),
      static_cast<std::uint32_t>(state.gpr[6]));
  return guest_return(state);
}

cpu::NativeCompiledEntry entry_for(analysis::NativeReplacementKind kind) noexcept {
  using analysis::NativeReplacementKind;
  switch (kind) {
    case NativeReplacementKind::Memcpy: return &memcpy_v2;
    case NativeReplacementKind::Memmove: return &memmove_v2;
    case NativeReplacementKind::Memset: return &memset_v2;
    case NativeReplacementKind::MemcpyChecked: return &memcpy_checked_v2;
    case NativeReplacementKind::MemmoveChecked: return &memmove_checked_v2;
    case NativeReplacementKind::Memcmp: return &memcmp_v2;
    case NativeReplacementKind::Strlen: return &strlen_v2;
    case NativeReplacementKind::Strncmp: return &strncmp_v2;
    case NativeReplacementKind::Strncpy: return &strncpy_v2;
    case NativeReplacementKind::Strchr: return &strchr_v2;
    case NativeReplacementKind::Strstr: return &strstr_v2;
    case NativeReplacementKind::Strrchr: return &strrchr_v2;
    case NativeReplacementKind::StrcpyChecked: return &strcpy_checked_v2;
    case NativeReplacementKind::HeapAllocate: return &heap_allocate_v2;
    case NativeReplacementKind::HeapFree: return &heap_free_v2;
    case NativeReplacementKind::HeapSize: return &heap_size_v2;
    case NativeReplacementKind::HeapReAllocate: return &heap_reallocate_v2;
    case NativeReplacementKind::Unsupported: return nullptr;
  }
  return nullptr;
}

}  // namespace xenon::recomp::native_replacements
