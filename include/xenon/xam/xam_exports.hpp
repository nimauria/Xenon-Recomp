#pragma once

#include <cstdint>

namespace xenon::xam {

// XAM export ordinals
// These are the common ordinals used by Xbox 360 titles
// Sourced from Xenia and ReXGlue research
namespace ordinal {

// User management
constexpr std::uint32_t XamUserGetXUID = 0x0180;
constexpr std::uint32_t XamUserGetSigninState = 0x0181;
constexpr std::uint32_t XamUserGetSigninInfo = 0x0182;
constexpr std::uint32_t XamUserGetName = 0x0183;
constexpr std::uint32_t XamUserAreUsersFriends = 0x0184;
constexpr std::uint32_t XamUserCheckPrivilege = 0x0187;
constexpr std::uint32_t XamUserGetIndexFromXUID = 0x018A;

// Input (already implemented in xenon::input::xam::guest)
constexpr std::uint32_t XamInputGetCapabilities = 0x0190;
constexpr std::uint32_t XamInputGetState = 0x0191;
constexpr std::uint32_t XamInputSetState = 0x0192;
constexpr std::uint32_t XamInputGetKeystroke = 0x0193;
constexpr std::uint32_t XamInputGetKeystrokeEx = 0x0198;
constexpr std::uint32_t XamInputGetCapabilitiesEx = 0x02AD;

// Locale and language
constexpr std::uint32_t XamGetLanguage = 0x0206;
constexpr std::uint32_t XamGetLocale = 0x0207;
constexpr std::uint32_t XamGetTimeZoneInformation = 0x0208;

// Notifications
constexpr std::uint32_t XamNotifyCreateListener = 0x0210;
constexpr std::uint32_t XamNotifyGetNext = 0x0211;
constexpr std::uint32_t XamNotifyPositionUI = 0x0212;

// Content and storage
constexpr std::uint32_t XamContentGetCreator = 0x0230;
constexpr std::uint32_t XamContentGetThumbnail = 0x0231;
constexpr std::uint32_t XamContentGetLicenseMask = 0x0232;
constexpr std::uint32_t XamContentCreateEnumerator = 0x0234;
constexpr std::uint32_t XamContentCreateEx = 0x0235;
constexpr std::uint32_t XamContentOpenFile = 0x0236;
constexpr std::uint32_t XamContentClose = 0x0237;
constexpr std::uint32_t XamContentFlush = 0x0238;
constexpr std::uint32_t XamContentDelete = 0x0239;
// TODO(xam-export-ordinals): placeholder values, not yet verified against a
// captured retail XEX import table (see docs/RUNTIME_SESSION.md "Register
// XAM exports" - this content export set is not wired into live import
// resolution yet). Correct before shipping a module that imports either by
// ordinal.
constexpr std::uint32_t XamContentGetDeviceData = 0x023A;
constexpr std::uint32_t XamContentGetDeviceName = 0x023B;

// Storage devices
constexpr std::uint32_t XamShowDeviceSelectorUI = 0x0250;
constexpr std::uint32_t XamShowMessageBoxUI = 0x0251;
constexpr std::uint32_t XamShowSigninUI = 0x0252;

// Profile settings
constexpr std::uint32_t XamProfileCreate = 0x0260;
constexpr std::uint32_t XamProfileCreateEnumerator = 0x0261;
constexpr std::uint32_t XamProfileEnumerate = 0x0262;

// Achievements
constexpr std::uint32_t XamUserWriteAchievements = 0x0280;
constexpr std::uint32_t XamUserReadStats = 0x0281;
constexpr std::uint32_t XamUserWriteStats = 0x0282;
constexpr std::uint32_t XamUserCreateStatsEnumerator = 0x0283;
constexpr std::uint32_t XamUserCreateAchievementEnumerator = 0x0284;

// Presence
constexpr std::uint32_t XamPresenceSubscribe = 0x02A0;
constexpr std::uint32_t XamPresenceUnsubscribe = 0x02A1;
constexpr std::uint32_t XamPresenceCreateEnumerator = 0x02A2;

// Session / Matchmaking
constexpr std::uint32_t XamSessionCreate = 0x02C0;
constexpr std::uint32_t XamSessionDelete = 0x02C1;
constexpr std::uint32_t XamSessionJoinRemote = 0x02C2;
constexpr std::uint32_t XamSessionJoinLocal = 0x02C3;
constexpr std::uint32_t XamSessionLeaveRemote = 0x02C4;
constexpr std::uint32_t XamSessionLeaveLocal = 0x02C5;

// Market/Title
constexpr std::uint32_t XamShowMarketplaceUI = 0x02E0;
constexpr std::uint32_t XamGetExecutionId = 0x02E1;
constexpr std::uint32_t XamGetOnlineServiceInfo = 0x02E2;

}  // namespace ordinal

}  // namespace xenon::xam
