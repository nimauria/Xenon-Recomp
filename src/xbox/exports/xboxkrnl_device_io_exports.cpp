// xboxkrnl low-level block-device IOCTL exports (NtDeviceIoControlFile,
// IoDismountVolume, IoDismountVolumeByFileHandle, StfsCreateDevice,
// StfsControlDevice). Registered into session.cpp's kSyncBindings table (the
// same KernelProcess&-taking SyncHandler convention every other simple
// xboxkrnl export there uses), not through the separate ImportRegistry/
// GuestIoBridge bridge xboxkrnl_io_exports.cpp's NtReadFile/NtWriteFile
// family uses - none of these five calls need real filesystem access or
// KernelProcess state, only guest-memory IOCTL buffer semantics, but they
// take an unused KernelProcess& first argument to match that table's shared
// function-pointer type.

#include <cstdint>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/xbox/xboxkrnl_device_io_exports.hpp"

namespace xenon::xbox {

using core::ExportCallContext;

// NtDeviceIoControlFile (ordinal 0xD9) - handles the two real IOCTLs the
// AC6 boot path's XMountUtilityDrive cache-mounting code issues against the
// disk device, verified against rexglue-sdk's NtDeviceIoControlFile_entry:
//   IOCTL_DISK_GET_DRIVE_GEOMETRY (0x70000): writes {total_sectors (u32),
//   bytes_per_sector (u32)} for a fixed 0xFF000-byte (~1044 KiB) cache
//   volume - the same size value that reference's own comment documents as
//   passing the title's own sanity checks.
//   IOCTL_DISK_GET_PARTITION_INFO (0x74004): writes {starting_offset (u64),
//   partition_length (u64)} for the same volume (offset 0, length
//   0xFF000).
// Any other IOCTL code is not modeled - real hardware would route it to a
// real device driver this codebase does not implement, so this reports the
// real "invalid parameter" outcome rather than fabricating a plausible-
// looking response for an unverified IOCTL.
bool nt_device_io_control_file_export(kernel::KernelProcess&, ExportCallContext& context) {
  constexpr std::uint32_t kIoctlDiskGetDriveGeometry = 0x70000u;
  constexpr std::uint32_t kIoctlDiskGetPartitionInfo = 0x74004u;
  constexpr std::uint64_t kCacheVolumeBytes = 0xFF000u;
  constexpr std::uint32_t kBytesPerSector = 512u;

  const auto io_control_code = static_cast<std::uint32_t>(context.cpu.gpr[8]);
  const auto output_buffer = static_cast<cpu::GuestAddress>(context.cpu.gpr[9]);
  const auto output_buffer_len = static_cast<std::uint32_t>(context.cpu.gpr[10]);

  if (io_control_code == kIoctlDiskGetDriveGeometry) {
    if (output_buffer_len < 8u) {
      context.cpu.gpr[3] = kernel::xbox::status::BufferTooSmall;
      return true;
    }
    context.memory.write32_be(output_buffer + 0u,
                              static_cast<std::uint32_t>(kCacheVolumeBytes / kBytesPerSector));
    context.memory.write32_be(output_buffer + 4u, kBytesPerSector);
  } else if (io_control_code == kIoctlDiskGetPartitionInfo) {
    if (output_buffer_len < 16u) {
      context.cpu.gpr[3] = kernel::xbox::status::BufferTooSmall;
      return true;
    }
    context.memory.write64_be(output_buffer + 0u, 0u);
    context.memory.write64_be(output_buffer + 8u, kCacheVolumeBytes);
  } else {
    context.cpu.gpr[3] = kernel::xbox::status::InvalidParameter;
    return true;
  }

  context.cpu.gpr[3] = kernel::xbox::status::Success;
  return true;
}

// IoDismountVolume/IoDismountVolumeByFileHandle (ordinals 0x3B/0x3C) - real
// hardware tears down a mounted volume's in-kernel device-object state.
// Xenon's VFS has no dynamic per-volume mount-table entry to tear down (its
// mounts are configured once at session setup, not created/destroyed by
// guest code at runtime), so there is nothing for either call to actually
// release - matching rexglue-sdk's own IoDismountVolumeByFileHandle_entry
// (real, complete behavior: warn and report success, no real multi-volume
// state exists to track either there).
bool io_dismount_volume_export(kernel::KernelProcess&, ExportCallContext& context) {
  context.cpu.gpr[3] = kernel::xbox::status::Success;
  return true;
}

// StfsCreateDevice/StfsControlDevice (ordinals 0x259/0x25A) - the legacy
// low-level STFS block-device NT path. Modern content access in this
// codebase goes through XamContent*/ContentManager against a real parsed
// STFS package (src/filesystem/stfs_package.hpp), not this raw device
// layer; verified against rexglue-sdk, which also leaves both as plain
// success stubs with no real backing device object - no available
// reference backs this low-level path with real STFS device semantics.
bool stfs_device_export(kernel::KernelProcess&, ExportCallContext& context) {
  context.cpu.gpr[3] = kernel::xbox::status::Success;
  return true;
}

}  // namespace xenon::xbox
