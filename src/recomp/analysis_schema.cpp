#include "xenon/recomp/analysis_schema.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace xenon::recomp::analysis {

namespace {

std::string hex_address(std::uint32_t address) {
  std::ostringstream out;
  out << "0x" << std::hex << address;
  return out.str();
}

}  // namespace

const char* native_replacement_kind_name(NativeReplacementKind kind) noexcept {
  switch (kind) {
    case NativeReplacementKind::Unsupported: return "Unsupported";
    case NativeReplacementKind::Memcpy: return "Memcpy";
    case NativeReplacementKind::Memmove: return "Memmove";
    case NativeReplacementKind::Memset: return "Memset";
    case NativeReplacementKind::MemcpyChecked: return "MemcpyChecked";
    case NativeReplacementKind::MemmoveChecked: return "MemmoveChecked";
    case NativeReplacementKind::Memcmp: return "Memcmp";
    case NativeReplacementKind::Strlen: return "Strlen";
    case NativeReplacementKind::Strncmp: return "Strncmp";
    case NativeReplacementKind::Strncpy: return "Strncpy";
    case NativeReplacementKind::Strchr: return "Strchr";
    case NativeReplacementKind::Strstr: return "Strstr";
    case NativeReplacementKind::Strrchr: return "Strrchr";
    case NativeReplacementKind::StrcpyChecked: return "StrcpyChecked";
    case NativeReplacementKind::HeapAllocate: return "HeapAllocate";
    case NativeReplacementKind::HeapFree: return "HeapFree";
    case NativeReplacementKind::HeapSize: return "HeapSize";
    case NativeReplacementKind::HeapReAllocate: return "HeapReAllocate";
  }
  return "Unsupported";
}

NativeReplacementKind native_replacement_kind_from_name(const std::string& name) noexcept {
  // Names as spelled in sal063/AC6_recomp's public ac6recomp_config.toml
  // [rexcrt] section (research reference per CLAUDE.md) plus the obvious
  // non-"_s"-suffixed / Rtl* spellings other rexglue-family titles use.
  static const std::unordered_map<std::string, NativeReplacementKind> kNames = {
      {"memcpy", NativeReplacementKind::Memcpy},
      {"memmove", NativeReplacementKind::Memmove},
      {"memset", NativeReplacementKind::Memset},
      {"memcpy_s", NativeReplacementKind::MemcpyChecked},
      {"memmove_s", NativeReplacementKind::MemmoveChecked},
      {"memcmp", NativeReplacementKind::Memcmp},
      {"strlen", NativeReplacementKind::Strlen},
      {"strncmp", NativeReplacementKind::Strncmp},
      {"strncpy", NativeReplacementKind::Strncpy},
      {"strchr", NativeReplacementKind::Strchr},
      {"strstr", NativeReplacementKind::Strstr},
      {"strrchr", NativeReplacementKind::Strrchr},
      {"strcpy_s", NativeReplacementKind::StrcpyChecked},
      {"RtlAllocateHeap", NativeReplacementKind::HeapAllocate},
      {"RtlFreeHeap", NativeReplacementKind::HeapFree},
      {"RtlSizeHeap", NativeReplacementKind::HeapSize},
      {"RtlReAllocateHeap", NativeReplacementKind::HeapReAllocate},
  };
  const auto it = kNames.find(name);
  return it != kNames.end() ? it->second : NativeReplacementKind::Unsupported;
}

bool runtime_helper_kind_is_register_range(RuntimeHelperKind kind) noexcept {
  switch (kind) {
    case RuntimeHelperKind::SaveGprLr:
    case RuntimeHelperKind::RestoreGprLr:
    case RuntimeHelperKind::SaveFpr:
    case RuntimeHelperKind::RestoreFpr:
    case RuntimeHelperKind::SaveVmx:
    case RuntimeHelperKind::RestoreVmx:
    case RuntimeHelperKind::SaveVmx128:
    case RuntimeHelperKind::RestoreVmx128:
      return true;
    case RuntimeHelperKind::SetJmp:
    case RuntimeHelperKind::LongJmp:
      return false;
  }
  return false;
}

bool runtime_helper_kind_is_save(RuntimeHelperKind kind) noexcept {
  switch (kind) {
    case RuntimeHelperKind::SaveGprLr:
    case RuntimeHelperKind::SaveFpr:
    case RuntimeHelperKind::SaveVmx:
    case RuntimeHelperKind::SaveVmx128:
      return true;
    default:
      return false;
  }
}

std::uint32_t runtime_helper_register_limit(RuntimeHelperKind kind) noexcept {
  switch (kind) {
    case RuntimeHelperKind::SaveVmx128:
    case RuntimeHelperKind::RestoreVmx128:
      return 128u;
    default:
      return 32u;
  }
}

std::uint32_t runtime_helper_register_family_base(RuntimeHelperKind kind) noexcept {
  switch (kind) {
    case RuntimeHelperKind::SaveVmx128:
    case RuntimeHelperKind::RestoreVmx128:
      return 64u;
    default:
      return 14u;
  }
}

std::uint32_t runtime_helper_register_stride(RuntimeHelperKind kind) noexcept {
  switch (kind) {
    case RuntimeHelperKind::SaveVmx:
    case RuntimeHelperKind::RestoreVmx:
    case RuntimeHelperKind::SaveVmx128:
    case RuntimeHelperKind::RestoreVmx128:
      return 8u;
    default:
      return 4u;
  }
}

std::vector<RuntimeHelper> expand_runtime_helpers(const std::vector<RuntimeHelper>& helpers) {
  std::vector<RuntimeHelper> expanded;
  for (const auto& helper : helpers) {
    if (!runtime_helper_kind_is_register_range(helper.kind)) {
      expanded.push_back(helper);
      continue;
    }
    const auto family_base = runtime_helper_register_family_base(helper.kind);
    const auto start = helper.register_start.value_or(family_base);
    const auto limit = runtime_helper_register_limit(helper.kind);
    const auto stride = runtime_helper_register_stride(helper.kind);
    for (auto reg = start; reg < limit; ++reg) {
      RuntimeHelper variant{};
      variant.address = helper.address + (reg - start) * stride;
      variant.kind = helper.kind;
      variant.register_start = reg;
      expanded.push_back(variant);
    }
  }
  return expanded;
}

bool hint_set_matches_identity(const AnalysisHintSetV2& hint_set,
                               const xbox::XexEffectiveIdentity& effective) noexcept {
  return hint_set.identity.title_id == effective.title_id &&
         hint_set.identity.media_id == effective.media_id &&
         hint_set.identity.effective_image_hash == effective.effective_image_hash;
}

bool validate_hint_set(const AnalysisHintSetV2& hint_set, std::vector<std::string>& errors) {
  errors.clear();

  if (hint_set.schema_version != kAnalysisSchemaVersion) {
    errors.push_back("unsupported analysis schema version " +
                     std::to_string(hint_set.schema_version) + " (this build understands version " +
                     std::to_string(kAnalysisSchemaVersion) + ")");
    // A version mismatch makes every other field's meaning untrustworthy;
    // stop here rather than validating fields whose semantics may differ.
    return false;
  }

  // Function hints: no duplicate addresses, end/size must not both be
  // zero-length or contradictory, parent references must resolve to a real
  // function address in this same set, and no parent cycles.
  std::unordered_map<std::uint32_t, const FunctionHint*> functions_by_address;
  for (const auto& function : hint_set.functions) {
    if (functions_by_address.contains(function.address)) {
      errors.push_back("duplicate function hint at " + hex_address(function.address));
      continue;
    }
    functions_by_address[function.address] = &function;
    if (function.end && *function.end <= function.address) {
      errors.push_back("function at " + hex_address(function.address) +
                       " has end <= address (" + hex_address(*function.end) + ")");
    }
    if (function.size && *function.size == 0) {
      errors.push_back("function at " + hex_address(function.address) + " declares size 0");
    }
  }
  for (const auto& function : hint_set.functions) {
    if (!function.parent_function) continue;
    if (*function.parent_function == function.address) {
      errors.push_back("function at " + hex_address(function.address) +
                       " declares itself as its own parent");
      continue;
    }
    if (!functions_by_address.contains(*function.parent_function)) {
      errors.push_back("function at " + hex_address(function.address) +
                       " declares parent " + hex_address(*function.parent_function) +
                       " which is not itself a function hint in this set");
    }
  }
  // Cycle detection over the parent_function graph (Part 1.5: "no parent
  // cycles"). Walk each function's parent chain with a visited-set per walk;
  // a repeated address means a cycle.
  for (const auto& function : hint_set.functions) {
    std::unordered_set<std::uint32_t> visited;
    std::uint32_t current = function.address;
    visited.insert(current);
    const FunctionHint* node = &function;
    while (node->parent_function) {
      const auto parent_address = *node->parent_function;
      if (visited.contains(parent_address)) {
        errors.push_back("parent-function cycle detected involving " + hex_address(parent_address));
        break;
      }
      visited.insert(parent_address);
      const auto it = functions_by_address.find(parent_address);
      if (it == functions_by_address.end()) break;  // already reported above
      node = it->second;
    }
  }

  // Function chunks: valid range, parent must resolve, no two chunks may
  // overlap with different parents (a contradictory ownership assignment).
  for (const auto& chunk : hint_set.chunks) {
    if (chunk.end <= chunk.start) {
      errors.push_back("function chunk [" + hex_address(chunk.start) + ", " + hex_address(chunk.end) +
                       ") has end <= start");
    }
    if (!functions_by_address.contains(chunk.parent_function)) {
      errors.push_back("function chunk [" + hex_address(chunk.start) + ", " + hex_address(chunk.end) +
                       ") declares parent " + hex_address(chunk.parent_function) +
                       " which is not a function hint in this set");
    }
  }
  for (std::size_t i = 0; i < hint_set.chunks.size(); ++i) {
    for (std::size_t j = i + 1; j < hint_set.chunks.size(); ++j) {
      const auto& a = hint_set.chunks[i];
      const auto& b = hint_set.chunks[j];
      const bool overlaps = a.start < b.end && b.start < a.end;
      if (overlaps && a.parent_function != b.parent_function) {
        errors.push_back("function chunks [" + hex_address(a.start) + ", " + hex_address(a.end) +
                         ") and [" + hex_address(b.start) + ", " + hex_address(b.end) +
                         ") overlap but declare different parent functions");
      }
    }
  }

  // Regions: valid ranges, and no two regions of DIFFERENT kinds may overlap
  // (a contradictory classification - e.g. the same bytes declared both Data
  // and InvalidInstruction). Two regions of the SAME kind overlapping is
  // redundant but not contradictory, so it is not an error.
  for (const auto& region : hint_set.regions) {
    if (region.end <= region.start) {
      errors.push_back("region [" + hex_address(region.start) + ", " + hex_address(region.end) +
                       ") has end <= start");
    }
  }
  for (std::size_t i = 0; i < hint_set.regions.size(); ++i) {
    for (std::size_t j = i + 1; j < hint_set.regions.size(); ++j) {
      const auto& a = hint_set.regions[i];
      const auto& b = hint_set.regions[j];
      const bool overlaps = a.start < b.end && b.start < a.end;
      if (overlaps && a.kind != b.kind) {
        errors.push_back("regions [" + hex_address(a.start) + ", " + hex_address(a.end) + ") and [" +
                         hex_address(b.start) + ", " + hex_address(b.end) +
                         ") overlap with contradictory classifications");
      }
    }
  }

  // Switch tables: entry_count without table_address (or vice versa without
  // explicit_targets) is a malformed hint the analyzer could not act on.
  for (const auto& table : hint_set.switches) {
    const bool has_table_shape = table.table_address.has_value() || table.entry_count.has_value();
    if (has_table_shape && !(table.table_address.has_value() && table.entry_count.has_value()) &&
        table.explicit_targets.empty()) {
      errors.push_back("switch table hint at " + hex_address(table.site) +
                       " gives only one of table_address/entry_count and no explicit_targets");
    }
    if (!has_table_shape && table.explicit_targets.empty()) {
      errors.push_back("switch table hint at " + hex_address(table.site) +
                       " gives neither a decodable table shape nor explicit_targets");
    }
  }

  // Runtime helpers: at most one of each kind per hint set - a title's
  // toolchain has exactly one setjmp/longjmp implementation, and exactly one
  // base address per register-helper family, per revision (matching
  // XenonRecomp's RecompilerConfig, which declares each as a single scalar
  // field, never an array).
  {
    std::unordered_set<int> seen_kinds;
    for (const auto& helper : hint_set.runtime_helpers) {
      if (!seen_kinds.insert(static_cast<int>(helper.kind)).second) {
        errors.push_back("more than one RuntimeHelper of the same kind declared (kind index " +
                         std::to_string(static_cast<int>(helper.kind)) + ")");
      }
      const bool is_register_range = runtime_helper_kind_is_register_range(helper.kind);
      if (is_register_range) {
        const auto family_base = runtime_helper_register_family_base(helper.kind);
        const auto limit = runtime_helper_register_limit(helper.kind);
        if (!helper.register_start || *helper.register_start < family_base ||
            *helper.register_start >= limit) {
          errors.push_back("RuntimeHelper at " + hex_address(helper.address) +
                           " needs register_start in [" + std::to_string(family_base) + ", " +
                           std::to_string(limit) + ")");
        }
      } else if (helper.register_start) {
        errors.push_back("RuntimeHelper at " + hex_address(helper.address) +
                         " declares register_start but its kind (SetJmp/LongJmp) does not use one");
      }
    }
  }
  // Post-expansion address collision check: two different families (or two
  // mis-declared instances of the same family) could still compute
  // overlapping concrete addresses despite the one-per-kind rule above -
  // this would silently break codegen's address->implementation dispatch
  // (two `case` labels for the same guest address), so it is reported here
  // as a schema error rather than left to surface as a build failure later.
  {
    std::unordered_map<std::uint32_t, RuntimeHelperKind> seen_addresses;
    for (const auto& helper : expand_runtime_helpers(hint_set.runtime_helpers)) {
      const auto [it, inserted] = seen_addresses.emplace(helper.address, helper.kind);
      if (!inserted) {
        errors.push_back("RuntimeHelper address collision at " + hex_address(helper.address));
      }
    }
  }

  // Instruction-pattern hints (Part 2): a skip of less than 4 bytes could
  // never even skip past the matched word itself, and PPC instructions are
  // always 4-byte aligned; an all-zero mask would match every instruction
  // word, which is never a meaningful pattern.
  for (const auto& pattern : hint_set.instruction_patterns) {
    if (pattern.skip_bytes < 4u || (pattern.skip_bytes % 4u) != 0u) {
      errors.push_back("instruction pattern " + hex_address(pattern.value) +
                       " has skip_bytes that is not a positive multiple of 4");
    }
    if (pattern.mask == 0u) {
      errors.push_back("instruction pattern " + hex_address(pattern.value) +
                       " has an all-zero mask (matches every instruction word)");
    }
    if (pattern.scope_start && pattern.scope_end && *pattern.scope_end <= *pattern.scope_start) {
      errors.push_back("instruction pattern " + hex_address(pattern.value) +
                       " has scope_end <= scope_start");
    }
  }

  return errors.empty();
}

}  // namespace xenon::recomp::analysis
