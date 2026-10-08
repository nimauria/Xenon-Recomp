#include "xenon/recomp/analysis_schema_json.hpp"

#include <array>
#include <cctype>
#include <unordered_map>

namespace xenon::recomp::analysis {

namespace {

using core::JsonValue;

std::string hash_to_hex(const std::array<std::byte, 20>& hash) {
  return xbox::format_effective_image_hash(hash);
}

bool hex_to_hash(const std::string& text, std::array<std::byte, 20>& out, std::string& error) {
  if (text.size() != 40) {
    error = "effectiveImageHash must be exactly 40 hex characters, got " + std::to_string(text.size());
    return false;
  }
  for (std::size_t i = 0; i < 20; ++i) {
    const auto hi = text[i * 2];
    const auto lo = text[i * 2 + 1];
    if (!std::isxdigit(static_cast<unsigned char>(hi)) || !std::isxdigit(static_cast<unsigned char>(lo))) {
      error = "effectiveImageHash contains a non-hex character";
      return false;
    }
    const auto nibble = [](char c) -> unsigned {
      if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
      return static_cast<unsigned>(std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
    };
    out[i] = static_cast<std::byte>((nibble(hi) << 4) | nibble(lo));
  }
  return true;
}

const char* function_flag_name(FunctionFlags flag) {
  switch (flag) {
    case FunctionFlags::NoReturn: return "NoReturn";
    case FunctionFlags::Leaf: return "Leaf";
    case FunctionFlags::Thunk: return "Thunk";
    case FunctionFlags::None: return "";
  }
  return "";
}

const char* switch_entry_format_name(SwitchEntryFormat format) {
  switch (format) {
    case SwitchEntryFormat::AbsoluteWord32: return "AbsoluteWord32";
    case SwitchEntryFormat::RelativeWord32: return "RelativeWord32";
    case SwitchEntryFormat::RelativeInt16: return "RelativeInt16";
  }
  return "AbsoluteWord32";
}

SwitchEntryFormat switch_entry_format_from_name(const std::string& name) {
  if (name == "RelativeWord32") return SwitchEntryFormat::RelativeWord32;
  if (name == "RelativeInt16") return SwitchEntryFormat::RelativeInt16;
  return SwitchEntryFormat::AbsoluteWord32;
}

const char* runtime_helper_kind_name(RuntimeHelperKind kind) {
  switch (kind) {
    case RuntimeHelperKind::SetJmp: return "SetJmp";
    case RuntimeHelperKind::LongJmp: return "LongJmp";
    case RuntimeHelperKind::SaveGprLr: return "SaveGprLr";
    case RuntimeHelperKind::RestoreGprLr: return "RestoreGprLr";
    case RuntimeHelperKind::SaveFpr: return "SaveFpr";
    case RuntimeHelperKind::RestoreFpr: return "RestoreFpr";
    case RuntimeHelperKind::SaveVmx: return "SaveVmx";
    case RuntimeHelperKind::RestoreVmx: return "RestoreVmx";
    case RuntimeHelperKind::SaveVmx128: return "SaveVmx128";
    case RuntimeHelperKind::RestoreVmx128: return "RestoreVmx128";
  }
  return "SetJmp";
}

RuntimeHelperKind runtime_helper_kind_from_name(const std::string& name) {
  static const std::unordered_map<std::string, RuntimeHelperKind> kNames = {
      {"SetJmp", RuntimeHelperKind::SetJmp},
      {"LongJmp", RuntimeHelperKind::LongJmp},
      {"SaveGprLr", RuntimeHelperKind::SaveGprLr},
      {"RestoreGprLr", RuntimeHelperKind::RestoreGprLr},
      {"SaveFpr", RuntimeHelperKind::SaveFpr},
      {"RestoreFpr", RuntimeHelperKind::RestoreFpr},
      {"SaveVmx", RuntimeHelperKind::SaveVmx},
      {"RestoreVmx", RuntimeHelperKind::RestoreVmx},
      {"SaveVmx128", RuntimeHelperKind::SaveVmx128},
      {"RestoreVmx128", RuntimeHelperKind::RestoreVmx128},
  };
  const auto it = kNames.find(name);
  return it != kNames.end() ? it->second : RuntimeHelperKind::SetJmp;
}

const char* region_kind_name(RegionKind kind) {
  switch (kind) {
    case RegionKind::Data: return "Data";
    case RegionKind::Ignored: return "Ignored";
    case RegionKind::InvalidInstruction: return "InvalidInstruction";
    case RegionKind::CodeOverride: return "CodeOverride";
  }
  return "Data";
}

RegionKind region_kind_from_name(const std::string& name) {
  if (name == "Ignored") return RegionKind::Ignored;
  if (name == "InvalidInstruction") return RegionKind::InvalidInstruction;
  if (name == "CodeOverride") return RegionKind::CodeOverride;
  return RegionKind::Data;
}

JsonValue uint_array(const std::vector<std::uint32_t>& values) {
  auto array = JsonValue::make_array();
  for (const auto value : values) array.append(JsonValue(value));
  return array;
}

std::vector<std::uint32_t> read_uint_array(const JsonValue& value) {
  std::vector<std::uint32_t> result;
  if (const auto* array = value.as_array()) {
    for (const auto& entry : *array) result.push_back(entry.as_uint32());
  }
  return result;
}

}  // namespace

JsonValue to_json(const AnalysisHintSetV2& hint_set) {
  auto root = JsonValue::make_object();
  root.set("schemaVersion", JsonValue(hint_set.schema_version));
  root.set("moduleName", hint_set.module_name);

  auto identity = JsonValue::make_object();
  identity.set("titleId", JsonValue(hint_set.identity.title_id));
  identity.set("mediaId", JsonValue(hint_set.identity.media_id));
  identity.set("baseVersion", JsonValue(hint_set.identity.base_version.value));
  identity.set("effectiveVersion", JsonValue(hint_set.identity.effective_version.value));
  identity.set("effectiveImageHash", hash_to_hex(hint_set.identity.effective_image_hash));
  identity.set("titleUpdateApplied", hint_set.identity.title_update_applied);
  root.set("identity", std::move(identity));

  auto functions = JsonValue::make_array();
  for (const auto& function : hint_set.functions) {
    auto entry = JsonValue::make_object();
    entry.set("address", JsonValue(function.address));
    if (function.end) entry.set("end", JsonValue(*function.end));
    if (function.size) entry.set("size", JsonValue(*function.size));
    if (!function.name.empty()) entry.set("name", function.name);
    if (function.parent_function) entry.set("parentFunction", JsonValue(*function.parent_function));
    auto flags = JsonValue::make_array();
    for (const auto flag : {FunctionFlags::NoReturn, FunctionFlags::Leaf, FunctionFlags::Thunk})
      if (has_flag(function.flags, flag)) flags.append(JsonValue(std::string(function_flag_name(flag))));
    entry.set("flags", std::move(flags));
    functions.append(std::move(entry));
  }
  root.set("functions", std::move(functions));

  auto chunks = JsonValue::make_array();
  for (const auto& chunk : hint_set.chunks) {
    auto entry = JsonValue::make_object();
    entry.set("start", JsonValue(chunk.start));
    entry.set("end", JsonValue(chunk.end));
    entry.set("parentFunction", JsonValue(chunk.parent_function));
    chunks.append(std::move(entry));
  }
  root.set("chunks", std::move(chunks));

  auto switches = JsonValue::make_array();
  for (const auto& table : hint_set.switches) {
    auto entry = JsonValue::make_object();
    entry.set("site", JsonValue(table.site));
    if (table.table_address) entry.set("tableAddress", JsonValue(*table.table_address));
    if (table.entry_count) entry.set("entryCount", JsonValue(*table.entry_count));
    entry.set("entryFormat", std::string(switch_entry_format_name(table.entry_format)));
    if (table.index_register) entry.set("indexRegister", JsonValue(*table.index_register));
    entry.set("explicitTargets", uint_array(table.explicit_targets));
    switches.append(std::move(entry));
  }
  root.set("switches", std::move(switches));

  auto indirect_calls = JsonValue::make_array();
  for (const auto& call : hint_set.indirect_calls) {
    auto entry = JsonValue::make_object();
    entry.set("callsite", JsonValue(call.callsite));
    entry.set("targets", uint_array(call.targets));
    indirect_calls.append(std::move(entry));
  }
  root.set("indirectCalls", std::move(indirect_calls));

  auto indirect_branches = JsonValue::make_array();
  for (const auto& branch : hint_set.indirect_branches) {
    auto entry = JsonValue::make_object();
    entry.set("site", JsonValue(branch.site));
    entry.set("targets", uint_array(branch.targets));
    indirect_branches.append(std::move(entry));
  }
  root.set("indirectBranches", std::move(indirect_branches));

  auto native_replacements = JsonValue::make_array();
  for (const auto& replacement : hint_set.native_replacements) {
    auto entry = JsonValue::make_object();
    entry.set("guestAddress", JsonValue(replacement.guest_address));
    entry.set("kind", std::string(native_replacement_kind_name(replacement.kind)));
    entry.set("sourceName", replacement.source_name);
    native_replacements.append(std::move(entry));
  }
  root.set("nativeReplacements", std::move(native_replacements));

  auto runtime_helpers = JsonValue::make_array();
  for (const auto& helper : hint_set.runtime_helpers) {
    auto entry = JsonValue::make_object();
    entry.set("address", JsonValue(helper.address));
    entry.set("kind", std::string(runtime_helper_kind_name(helper.kind)));
    if (helper.register_start) entry.set("registerStart", JsonValue(*helper.register_start));
    runtime_helpers.append(std::move(entry));
  }
  root.set("runtimeHelpers", std::move(runtime_helpers));

  auto instruction_patterns = JsonValue::make_array();
  for (const auto& pattern : hint_set.instruction_patterns) {
    auto entry = JsonValue::make_object();
    entry.set("value", JsonValue(pattern.value));
    entry.set("mask", JsonValue(pattern.mask));
    entry.set("skipBytes", JsonValue(pattern.skip_bytes));
    if (!pattern.reason.empty()) entry.set("reason", pattern.reason);
    if (pattern.scope_start) entry.set("scopeStart", JsonValue(*pattern.scope_start));
    if (pattern.scope_end) entry.set("scopeEnd", JsonValue(*pattern.scope_end));
    instruction_patterns.append(std::move(entry));
  }
  root.set("instructionPatterns", std::move(instruction_patterns));

  auto regions = JsonValue::make_array();
  for (const auto& region : hint_set.regions) {
    auto entry = JsonValue::make_object();
    entry.set("start", JsonValue(region.start));
    entry.set("end", JsonValue(region.end));
    entry.set("kind", std::string(region_kind_name(region.kind)));
    if (!region.reason.empty()) entry.set("reason", region.reason);
    regions.append(std::move(entry));
  }
  root.set("regions", std::move(regions));

  auto symbols = JsonValue::make_array();
  for (const auto& symbol : hint_set.symbols) {
    auto entry = JsonValue::make_object();
    entry.set("address", JsonValue(symbol.address));
    entry.set("name", symbol.name);
    symbols.append(std::move(entry));
  }
  root.set("symbols", std::move(symbols));

  auto patches = JsonValue::make_array();
  for (const auto& patch : hint_set.patches) {
    auto entry = JsonValue::make_object();
    entry.set("address", JsonValue(patch.address));
    entry.set("patchBytesHex", patch.patch_bytes_hex);
    if (!patch.description.empty()) entry.set("description", patch.description);
    patches.append(std::move(entry));
  }
  root.set("patches", std::move(patches));

  auto hooks = JsonValue::make_array();
  for (const auto& hook : hint_set.hooks) {
    auto entry = JsonValue::make_object();
    entry.set("address", JsonValue(hook.address));
    entry.set("nativeReplacementIdentity", hook.native_replacement_identity);
    if (!hook.description.empty()) entry.set("description", hook.description);
    hooks.append(std::move(entry));
  }
  root.set("hooks", std::move(hooks));

  return root;
}

bool from_json(const JsonValue& json, AnalysisHintSetV2& out, std::string& error) {
  out = AnalysisHintSetV2{};
  if (!json.is_object()) {
    error = "analysis hint set JSON must be an object";
    return false;
  }

  out.schema_version = static_cast<int>(json.get_number("schemaVersion", kAnalysisSchemaVersion));
  out.module_name = json.get_string("moduleName");

  if (const auto* identity = json.find("identity")) {
    out.identity.title_id = identity->get("titleId").as_uint32();
    out.identity.media_id = identity->get("mediaId").as_uint32();
    out.identity.base_version.value = identity->get("baseVersion").as_uint32();
    out.identity.effective_version.value = identity->get("effectiveVersion").as_uint32();
    out.identity.title_update_applied = identity->get_bool("titleUpdateApplied", false);
    const auto hash_hex = identity->get_string("effectiveImageHash");
    if (!hash_hex.empty()) {
      if (!hex_to_hash(hash_hex, out.identity.effective_image_hash, error)) return false;
    }
  }

  if (const auto* functions = json.find("functions")) {
    if (const auto* array = functions->as_array()) {
      for (const auto& entry : *array) {
        FunctionHint hint{};
        hint.address = entry.get("address").as_uint32();
        if (const auto* end = entry.find("end")) hint.end = end->as_uint32();
        if (const auto* size = entry.find("size")) hint.size = size->as_uint32();
        hint.name = entry.get_string("name");
        if (const auto* parent = entry.find("parentFunction")) hint.parent_function = parent->as_uint32();
        if (const auto* flags = entry.find("flags")) {
          if (const auto* flags_array = flags->as_array()) {
            for (const auto& flag_value : *flags_array) {
              const auto flag_name = flag_value.as_string();
              if (flag_name == "NoReturn") hint.flags = hint.flags | FunctionFlags::NoReturn;
              else if (flag_name == "Leaf") hint.flags = hint.flags | FunctionFlags::Leaf;
              else if (flag_name == "Thunk") hint.flags = hint.flags | FunctionFlags::Thunk;
            }
          }
        }
        out.functions.push_back(std::move(hint));
      }
    }
  }

  if (const auto* chunks = json.find("chunks")) {
    if (const auto* array = chunks->as_array()) {
      for (const auto& entry : *array) {
        FunctionChunk chunk{};
        chunk.start = entry.get("start").as_uint32();
        chunk.end = entry.get("end").as_uint32();
        chunk.parent_function = entry.get("parentFunction").as_uint32();
        out.chunks.push_back(chunk);
      }
    }
  }

  if (const auto* switches = json.find("switches")) {
    if (const auto* array = switches->as_array()) {
      for (const auto& entry : *array) {
        SwitchTableHint table{};
        table.site = entry.get("site").as_uint32();
        if (const auto* v = entry.find("tableAddress")) table.table_address = v->as_uint32();
        if (const auto* v = entry.find("entryCount")) table.entry_count = v->as_uint32();
        table.entry_format = switch_entry_format_from_name(entry.get_string("entryFormat", "AbsoluteWord32"));
        if (const auto* v = entry.find("indexRegister")) table.index_register = v->as_uint32();
        if (const auto* v = entry.find("explicitTargets")) table.explicit_targets = read_uint_array(*v);
        out.switches.push_back(std::move(table));
      }
    }
  }

  if (const auto* calls = json.find("indirectCalls")) {
    if (const auto* array = calls->as_array()) {
      for (const auto& entry : *array) {
        KnownIndirectCall call{};
        call.callsite = entry.get("callsite").as_uint32();
        if (const auto* v = entry.find("targets")) call.targets = read_uint_array(*v);
        out.indirect_calls.push_back(std::move(call));
      }
    }
  }

  if (const auto* branches = json.find("indirectBranches")) {
    if (const auto* array = branches->as_array()) {
      for (const auto& entry : *array) {
        KnownIndirectBranch branch{};
        branch.site = entry.get("site").as_uint32();
        if (const auto* v = entry.find("targets")) branch.targets = read_uint_array(*v);
        out.indirect_branches.push_back(std::move(branch));
      }
    }
  }

  if (const auto* replacements = json.find("nativeReplacements")) {
    if (const auto* array = replacements->as_array()) {
      for (const auto& entry : *array) {
        NativeReplacement replacement{};
        replacement.guest_address = entry.get("guestAddress").as_uint32();
        replacement.source_name = entry.get_string("sourceName");
        const auto kind_name = entry.get_string("kind");
        // Prefer the explicit kind name; fall back to resolving sourceName
        // for hint sets migrated straight from a symbolic rexcrt table.
        bool matched = false;
        for (int k = 0; k <= static_cast<int>(NativeReplacementKind::HeapReAllocate); ++k) {
          const auto candidate = static_cast<NativeReplacementKind>(k);
          if (kind_name == native_replacement_kind_name(candidate)) {
            replacement.kind = candidate;
            matched = true;
            break;
          }
        }
        if (!matched) replacement.kind = native_replacement_kind_from_name(replacement.source_name);
        out.native_replacements.push_back(std::move(replacement));
      }
    }
  }

  if (const auto* helpers = json.find("runtimeHelpers")) {
    if (const auto* array = helpers->as_array()) {
      for (const auto& entry : *array) {
        RuntimeHelper helper{};
        helper.address = entry.get("address").as_uint32();
        helper.kind = runtime_helper_kind_from_name(entry.get_string("kind"));
        if (const auto* register_start = entry.find("registerStart"))
          helper.register_start = register_start->as_uint32();
        out.runtime_helpers.push_back(helper);
      }
    }
  }

  if (const auto* patterns = json.find("instructionPatterns")) {
    if (const auto* array = patterns->as_array()) {
      for (const auto& entry : *array) {
        InstructionPatternHint pattern{};
        pattern.value = entry.get("value").as_uint32();
        pattern.mask = entry.find("mask") ? entry.get("mask").as_uint32() : 0xFFFFFFFFu;
        pattern.skip_bytes = entry.find("skipBytes") ? entry.get("skipBytes").as_uint32() : 4u;
        pattern.reason = entry.get_string("reason");
        if (const auto* scope_start = entry.find("scopeStart")) pattern.scope_start = scope_start->as_uint32();
        if (const auto* scope_end = entry.find("scopeEnd")) pattern.scope_end = scope_end->as_uint32();
        out.instruction_patterns.push_back(std::move(pattern));
      }
    }
  }

  if (const auto* regions = json.find("regions")) {
    if (const auto* array = regions->as_array()) {
      for (const auto& entry : *array) {
        RegionHint region{};
        region.start = entry.get("start").as_uint32();
        region.end = entry.get("end").as_uint32();
        region.kind = region_kind_from_name(entry.get_string("kind", "Data"));
        region.reason = entry.get_string("reason");
        out.regions.push_back(std::move(region));
      }
    }
  }

  if (const auto* symbols = json.find("symbols")) {
    if (const auto* array = symbols->as_array()) {
      for (const auto& entry : *array) {
        SymbolHint symbol{};
        symbol.address = entry.get("address").as_uint32();
        symbol.name = entry.get_string("name");
        out.symbols.push_back(std::move(symbol));
      }
    }
  }

  if (const auto* patches = json.find("patches")) {
    if (const auto* array = patches->as_array()) {
      for (const auto& entry : *array) {
        PatchDeclaration patch{};
        patch.address = entry.get("address").as_uint32();
        patch.patch_bytes_hex = entry.get_string("patchBytesHex");
        patch.description = entry.get_string("description");
        out.patches.push_back(std::move(patch));
      }
    }
  }

  if (const auto* hooks = json.find("hooks")) {
    if (const auto* array = hooks->as_array()) {
      for (const auto& entry : *array) {
        HookDeclaration hook{};
        hook.address = entry.get("address").as_uint32();
        hook.native_replacement_identity = entry.get_string("nativeReplacementIdentity");
        hook.description = entry.get_string("description");
        out.hooks.push_back(std::move(hook));
      }
    }
  }

  return true;
}

}  // namespace xenon::recomp::analysis
