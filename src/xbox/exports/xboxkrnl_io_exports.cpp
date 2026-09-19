#include "xenon/xbox/imports.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace xenon::xbox {
namespace {

using kernel::xbox::status::AccessViolation;
using kernel::xbox::Status;

constexpr std::string_view kXboxkrnl = "xboxkrnl";

void finish(PpcArgumentReader& args, Status status) noexcept {
  args.set_u32_result(status);
}

bool collect(PpcArgumentReader& args, std::span<std::uint32_t> values) noexcept {
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto value = args.u32(i);
    if (!value) return false;
    values[i] = *value;
  }
  return true;
}

template <std::size_t N, typename F>
void dispatch(ImportCallContext& context, F&& callback) {
  PpcArgumentReader args(context.cpu, context.memory);
  std::array<std::uint32_t, N> a{};
  if (!collect(args, a)) {
    finish(args, AccessViolation);
    return;
  }
  finish(args, callback(a));
}

void NtCreateFile_thunk(ImportCallContext& c) {
  dispatch<9>(c, [&](const auto& a) {
    return c.io.nt_create_file(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
  });
}
void NtOpenFile_thunk(ImportCallContext& c) {
  dispatch<5>(c, [&](const auto& a) { return c.io.nt_open_file(a[0], a[1], a[2], a[3], a[4]); });
}
void NtReadFile_thunk(ImportCallContext& c) {
  dispatch<8>(c, [&](const auto& a) {
    return c.io.nt_read_file(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  });
}
void NtReadFileScatter_thunk(ImportCallContext& c) {
  dispatch<8>(c, [&](const auto& a) {
    return c.io.nt_read_file_scatter(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  });
}
void NtWriteFile_thunk(ImportCallContext& c) {
  dispatch<8>(c, [&](const auto& a) {
    return c.io.nt_write_file(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  });
}
void NtFlushBuffersFile_thunk(ImportCallContext& c) {
  dispatch<2>(c, [&](const auto& a) { return c.io.nt_flush_buffers_file(a[0], a[1]); });
}
void NtQueryDirectoryFile_thunk(ImportCallContext& c) {
  dispatch<9>(c, [&](const auto& a) {
    return c.io.nt_query_directory_file(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                        a[8] != 0);
  });
}
void NtQueryInformationFile_thunk(ImportCallContext& c) {
  dispatch<5>(c, [&](const auto& a) {
    return c.io.nt_query_information_file(a[0], a[1], a[2], a[3], a[4]);
  });
}
void NtSetInformationFile_thunk(ImportCallContext& c) {
  dispatch<5>(c, [&](const auto& a) {
    return c.io.nt_set_information_file(a[0], a[1], a[2], a[3], a[4]);
  });
}
void NtQueryVolumeInformationFile_thunk(ImportCallContext& c) {
  dispatch<5>(c, [&](const auto& a) {
    return c.io.nt_query_volume_information_file(a[0], a[1], a[2], a[3], a[4]);
  });
}
void NtQueryFullAttributesFile_thunk(ImportCallContext& c) {
  dispatch<2>(c, [&](const auto& a) { return c.io.nt_query_full_attributes_file(a[0], a[1]); });
}

struct BuiltinImport {
  std::uint16_t ordinal;
  const char* name;
  ImportThunk thunk;
};

// Xbox 360 xboxkrnl ordinals, matching the export table used by Xenia/ReXGlue.
constexpr BuiltinImport kImports[] = {
    {0x00D2, "NtCreateFile", &NtCreateFile_thunk},
    {0x00DB, "NtFlushBuffersFile", &NtFlushBuffersFile_thunk},
    {0x00DF, "NtOpenFile", &NtOpenFile_thunk},
    {0x00E4, "NtQueryDirectoryFile", &NtQueryDirectoryFile_thunk},
    {0x00E7, "NtQueryFullAttributesFile", &NtQueryFullAttributesFile_thunk},
    {0x00E8, "NtQueryInformationFile", &NtQueryInformationFile_thunk},
    {0x00EF, "NtQueryVolumeInformationFile", &NtQueryVolumeInformationFile_thunk},
    {0x00F0, "NtReadFile", &NtReadFile_thunk},
    {0x00F1, "NtReadFileScatter", &NtReadFileScatter_thunk},
    {0x00F7, "NtSetInformationFile", &NtSetInformationFile_thunk},
    {0x00FF, "NtWriteFile", &NtWriteFile_thunk},
};

}  // namespace

bool register_xboxkrnl_io_imports(ImportRegistry& registry) {
  for (const auto& entry : kImports) {
    if (!registry.register_import(
            {std::string(kXboxkrnl), entry.name, entry.ordinal, entry.thunk})) {
      return false;
    }
  }
  return true;
}

}  // namespace xenon::xbox
