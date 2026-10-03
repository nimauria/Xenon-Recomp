#pragma once

namespace xenon::core {
struct ExportCallContext;
}
namespace xenon::kernel {
class KernelProcess;
}

namespace xenon::xbox {

// Low-level block-device IOCTL exports (ordinals 0x3B, 0x3C, 0xD9, 0x259,
// 0x25A) - see xboxkrnl_device_io_exports.cpp for the real behavior each
// implements. Take an unused KernelProcess& first argument so they match
// session.cpp's shared SyncHandler function-pointer type exactly, the same
// convention xboxkrnl_video_exports.hpp's Vd* exports use.
[[nodiscard]] bool nt_device_io_control_file_export(xenon::kernel::KernelProcess& process,
                                                    xenon::core::ExportCallContext& context);
[[nodiscard]] bool io_dismount_volume_export(xenon::kernel::KernelProcess& process,
                                             xenon::core::ExportCallContext& context);
[[nodiscard]] bool stfs_device_export(xenon::kernel::KernelProcess& process,
                                      xenon::core::ExportCallContext& context);

}  // namespace xenon::xbox
