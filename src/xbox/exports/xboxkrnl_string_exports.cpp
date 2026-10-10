#include "xenon/xbox/xboxkrnl_string_exports.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/xbox/string_format.hpp"

namespace xenon::xbox {
namespace {

using core::ExportCallContext;

// Each variant differs only in which fixed parameters precede the arguments and
// in character width:
//   buffer   - the first parameter is the destination buffer
//   count    - a buffer capacity in characters follows the buffer (_snprintf)
//   va_list  - the arguments come from a guest va_list, not registers/stack
struct StringVariant {
  std::uint32_t ordinal;
  const char* name;
  bool wide;
  bool buffer;
  bool count;
  bool va_list;
};

// Ordinals verified against the xenia xboxkrnl export table.
constexpr StringVariant kVariants[] = {
    {0x139u, "_scprintf", false, false, false, false},
    {0x13Au, "_snprintf", false, true, true, false},
    {0x13Bu, "sprintf", false, true, false, false},
    {0x13Cu, "_scwprintf", true, false, false, false},
    {0x13Du, "_snwprintf", true, true, true, false},
    {0x13Eu, "swprintf", true, true, false, false},
    {0x14Cu, "_vscprintf", false, false, false, true},
    {0x14Du, "_vsnprintf", false, true, true, true},
    {0x14Eu, "vsprintf", false, true, false, true},
    {0x14Fu, "_vscwprintf", true, false, false, true},
    {0x150u, "_vsnwprintf", true, true, true, true},
    {0x151u, "vswprintf", true, true, false, true},
};

// Sign-extends an `int` result into the 64-bit return register, as the PowerPC
// ABI does for a 32-bit int return.
void set_int_result(ExportCallContext& context, std::int32_t value) {
  context.cpu.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

void write_char(cpu::MemoryPort& memory, cpu::GuestAddress buffer, std::uint32_t index, bool wide,
                char16_t value) {
  if (wide) {
    memory.write16_be(buffer + index * 2u, static_cast<std::uint16_t>(value));
  } else {
    memory.write8(buffer + index, static_cast<std::uint8_t>(value));
  }
}

bool run_variant(const StringVariant& variant, ExportCallContext& context) {
  auto& cpu = context.cpu;
  auto& memory = context.memory;

  // Fixed parameters in order: [buffer] [count] format [va_list].
  std::uint32_t position = 0;
  const auto buffer =
      variant.buffer ? static_cast<cpu::GuestAddress>(cpu.gpr[3u + position++]) : 0u;
  std::int32_t capacity = 0;
  if (variant.count) capacity = static_cast<std::int32_t>(cpu.gpr[3u + position++]);
  const auto format_ptr = static_cast<cpu::GuestAddress>(cpu.gpr[3u + position++]);

  // The CRT rejects a missing buffer/format or a non-positive capacity.
  if ((variant.buffer && buffer == 0u) || format_ptr == 0u || (variant.count && capacity <= 0)) {
    set_int_result(context, -1);
    return true;
  }

  const auto format_text = format::read_guest_string(memory, format_ptr, variant.wide);
  format::Result result;
  if (variant.va_list) {
    format::ArrayArgumentSource args(memory, static_cast<cpu::GuestAddress>(cpu.gpr[3u + position]));
    result = format::format(memory, format_text, args, variant.wide);
  } else {
    format::RegisterArgumentSource args(cpu, memory, position);
    result = format::format(memory, format_text, args, variant.wide);
  }

  if (!variant.buffer) {  // _scprintf family: only the length.
    set_int_result(context, result.count);
    return true;
  }
  if (result.count < 0) {
    write_char(memory, buffer, 0u, variant.wide, u'\0');
    set_int_result(context, -1);
    return true;
  }
  const auto produced = static_cast<std::uint32_t>(result.text.size());
  if (!variant.count) {  // sprintf: the whole string plus a terminator.
    for (std::uint32_t i = 0; i < produced; ++i) {
      write_char(memory, buffer, i, variant.wide, result.text[i]);
    }
    write_char(memory, buffer, produced, variant.wide, u'\0');
    set_int_result(context, result.count);
    return true;
  }
  // _snprintf: fits (terminated only if there is room) or truncates and fails.
  const auto limit = static_cast<std::uint32_t>(capacity);
  if (produced <= limit) {
    for (std::uint32_t i = 0; i < produced; ++i) {
      write_char(memory, buffer, i, variant.wide, result.text[i]);
    }
    if (produced < limit) write_char(memory, buffer, produced, variant.wide, u'\0');
    set_int_result(context, result.count);
    return true;
  }
  for (std::uint32_t i = 0; i < limit; ++i) {
    write_char(memory, buffer, i, variant.wide, result.text[i]);
  }
  set_int_result(context, -1);
  return true;
}

// DbgPrint (ordinal 0x03)
// Guest ABI: r3 = guest pointer to a narrow format string, further arguments
// variadic -> r3 = NTSTATUS. Retail consoles route this to a debugger that is
// not attached, so its only observable effect is the message; Xenon delivers
// the formatted text (trailing whitespace trimmed, as the reference
// implementations do) to the logger at Info level under the "dbgprint"
// category. A NULL format is STATUS_INVALID_PARAMETER; a formatting failure
// prints nothing and still succeeds, like the reference implementations.
bool dbg_print_export(ExportCallContext& context) {
  const auto format_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[3]);
  if (format_ptr == 0u) {
    context.cpu.gpr[3] = 0xC000000Du;
    return true;
  }
  const auto format_text = format::read_guest_string(context.memory, format_ptr, false);
  format::RegisterArgumentSource args(context.cpu, context.memory, 1u);
  const auto result = format::format(context.memory, format_text, args, false);
  if (result.count > 0) {
    std::string message;
    message.reserve(result.text.size());
    for (const char16_t c : result.text) message.push_back(static_cast<char>(c));
    while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back()))) {
      message.pop_back();
    }
    logging::Logger::instance().log(logging::Level::Info, "dbgprint", message);
  }
  context.cpu.gpr[3] = 0u;
  return true;
}

// DbgBreakPoint / DbgBreakPointWithStatus: no guest debugger exists on Xenon, so
// the break is reported at Warning level and the call returns.
bool dbg_break_point_export(ExportCallContext& context) {
  logging::Logger::instance().log_if_enabled(logging::Level::Warning, "dbgprint", [&] {
    return std::string("guest executed a debug breakpoint (r3=0x") + [&] {
      char text[24];
      std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(context.cpu.gpr[3]));
      return std::string(text);
    }() + ")";
  });
  return true;
}

// DbgPrompt: r3 = prompt, r4 = response buffer, r5 = buffer length -> r3 =
// characters read. With no debugger attached nothing is ever read: the response
// is an empty string and the result 0.
bool dbg_prompt_export(ExportCallContext& context) {
  const auto response = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto length = static_cast<std::uint32_t>(context.cpu.gpr[5]);
  if (response != 0u && length > 0u) context.memory.write8(response, 0u);
  context.cpu.gpr[3] = 0u;
  return true;
}

// KiApcNormalRoutineNop: `void (*)(PVOID, PVOID, PVOID)` whose whole body is a
// return. Leaving the registers untouched is the exact semantics.
bool ki_apc_normal_routine_nop_export(ExportCallContext&) {
  return true;
}

}  // namespace

bool register_xboxkrnl_string_exports(core::ExportRegistry& registry) {
  for (const auto& variant : kVariants) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = variant.name;
    descriptor.ordinal = variant.ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.handler = [variant](ExportCallContext& context) {
      return run_variant(variant, context);
    };
    if (!registry.register_export(std::move(descriptor))) return false;
  }

  core::ExportDescriptor dbg{};
  dbg.library = "xboxkrnl.exe";
  dbg.name = "DbgPrint";
  dbg.ordinal = 0x03u;
  dbg.requirement = core::ExportRequirement::Required;
  dbg.handler = &dbg_print_export;
  return registry.register_export(std::move(dbg));
}

bool register_xboxkrnl_debug_exports(core::ExportRegistry& registry) {
  struct Spec {
    std::uint32_t ordinal;
    const char* name;
    core::ExportHandler handler;
  };
  const Spec specs[] = {
      {0x01u, "DbgBreakPoint", &dbg_break_point_export},
      {0x02u, "DbgBreakPointWithStatus", &dbg_break_point_export},
      {0x04u, "DbgPrompt", &dbg_prompt_export},
      {0x1DFu, "KiApcNormalRoutineNop", &ki_apc_normal_routine_nop_export},
  };
  for (const auto& spec : specs) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = spec.name;
    descriptor.ordinal = spec.ordinal;
    descriptor.requirement = core::ExportRequirement::Required;
    descriptor.handler = spec.handler;
    if (!registry.register_export(std::move(descriptor))) return false;
  }
  return true;
}

bool register_xboxkrnl_bugcheck_exports(core::ExportRegistry& registry, BugCheckHandler handler) {
  const auto make = [handler](bool extended) {
    return [handler, extended](ExportCallContext& context) {
      BugCheckInfo info;
      info.code = static_cast<std::uint32_t>(context.cpu.gpr[3]);
      if (extended) {
        for (std::size_t i = 0; i < info.parameters.size(); ++i) {
          info.parameters[i] = static_cast<std::uint32_t>(context.cpu.gpr[4u + i]);
        }
      }
      char text[128];
      std::snprintf(text, sizeof(text),
                    "guest kernel bugcheck 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)", info.code,
                    info.parameters[0], info.parameters[1], info.parameters[2], info.parameters[3]);
      info.description = text;
      if (handler) handler(info);
      return true;
    };
  };
  core::ExportDescriptor plain{};
  plain.library = "xboxkrnl.exe";
  plain.name = "KeBugCheck";
  plain.ordinal = 0x52u;
  plain.requirement = core::ExportRequirement::Required;
  plain.handler = make(false);
  core::ExportDescriptor extended{};
  extended.library = "xboxkrnl.exe";
  extended.name = "KeBugCheckEx";
  extended.ordinal = 0x53u;
  extended.requirement = core::ExportRequirement::Required;
  extended.handler = make(true);
  return registry.register_export(std::move(plain)) && registry.register_export(std::move(extended));
}

}  // namespace xenon::xbox
