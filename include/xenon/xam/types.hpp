#pragma once

#include <cstdint>

namespace xenon::xam {

// Xbox User ID (XUID) - 64-bit unique identifier
using XUID = std::uint64_t;

// XAM result codes
using XResult = std::uint32_t;

namespace result {
constexpr XResult Success = 0x00000000u;
constexpr XResult Busy = 0x000000AAu;
constexpr XResult NoSuchUser = 0x00000002u;
constexpr XResult NotLoggedOn = 0x00000003u;
constexpr XResult InvalidParameter = 0x00000057u;
constexpr XResult AccessDenied = 0x00000005u;
constexpr XResult FunctionFailed = 0x0000065Bu;
constexpr XResult NoMoreFiles = 0x00000012u;
constexpr XResult Empty = 0x000010D2u;
// Win32 ERROR_INVALID_STATE: a content-services manager method was called
// before its owning manager finished initialize().
constexpr XResult InvalidState = 0x0000139Fu;
// Win32 ERROR_NOT_READY: a host filesystem operation (create/delete of a
// save container) failed, e.g. because the backing storage disappeared.
constexpr XResult Unavailable = 0x00000015u;
}  // namespace result

// User sign-in states
enum class SigninState : std::uint32_t {
  NotSignedIn = 0,
  SignedInLocally = 1,
  SignedInToLive = 2
};

// User flags
namespace user_flag {
constexpr std::uint32_t Guest = 0x00000001u;
constexpr std::uint32_t PasscodeEnabled = 0x00000002u;
constexpr std::uint32_t OnlineEnabled = 0x00000004u;
constexpr std::uint32_t LiveEnabled = 0x00000008u;
}  // namespace user_flag

// Language codes (subset)
enum class LanguageId : std::uint32_t {
  Invalid = 0,
  English = 1,
  Japanese = 2,
  German = 3,
  French = 4,
  Spanish = 5,
  Italian = 6,
  Korean = 7,
  TraditionalChinese = 8,
  SimplifiedChinese = 9,
  Portuguese = 10,
  Polish = 11,
  Russian = 12
};

// Storage device IDs
namespace storage_device {
constexpr std::uint32_t Invalid = 0x00000000u;
constexpr std::uint32_t HardDisk = 0x00000001u;
constexpr std::uint32_t MemoryUnit = 0x00000002u;
constexpr std::uint32_t Usb = 0x00000003u;
}  // namespace storage_device

}  // namespace xenon::xam
