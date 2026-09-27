#pragma once

#include <cstdint>

namespace xenon::xam {

// XAM export ordinals
//
// These values are the real xam.xex guest-visible ordinals, cross-checked
// against:
//   - xenia-project/xenia: src/xenia/kernel/xam/xam_table.inc (primary
//     ordinal/name catalogue for the final-XDK xam.xex export surface).
//   - rexglue/rexglue-sdk: src/kernel/xam/export_table.inc (independent
//     mirror of the same table; used as a cross-check, not a second source
//     of truth - it credits the same origin).
// A retail XEX resolves xam.xex imports strictly by (library, ordinal), so
// an ordinal here that does not match the real xam.xex export table makes
// the corresponding guest import unreachable even if Xenon's own tests only
// ever exercise these constants against themselves.
//
// Every constant below corresponds to a real, named xam.xex export. Where
// the real Xbox 360 name differs from what earlier Xenon code assumed (most
// often a missing/extra "Xam" prefix, or "Get" vs "Query"), the constant is
// named after the REAL export, not the old Xenon-invented name; see the
// per-constant comments for the rename history.
//
// A small number of ordinals previously declared here
// (XamUserWriteAchievements, XamUserReadStats, XamUserWriteStats,
// XamPresenceSubscribe/Unsubscribe/CreateEnumerator,
// XamSessionCreate/Delete/JoinRemote/JoinLocal/LeaveRemote/LeaveLocal,
// XamGetOnlineServiceInfo) were removed outright: no xam.xex export with
// these names exists in either reference table, and neither xenia nor
// rexglue-sdk implement anything under these names. They were invented
// placeholders with no real Xbox 360 counterpart - assigning them "a"
// ordinal would still leave them permanently unreachable from a real XEX,
// so the fix is removal, not renumbering. See docs/xam/XAM_V1.md for the real
// exports that cover the same functional area (achievement/stat
// enumeration, local presence caching, XamSessionCreateHandle) and for why
// full parity there remains out of scope for this pass.
namespace ordinal {

// ---------------------------------------------------------------------------
// User management (xam_table.inc ~0x20A-0x227)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamUserGetXUID = 0x020A;
constexpr std::uint32_t XamUserGetName = 0x020E;
constexpr std::uint32_t XamUserGetSigninState = 0x0210;
constexpr std::uint32_t XamUserGetIndexFromXUID = 0x0211;
constexpr std::uint32_t XamUserCheckPrivilege = 0x0212;
constexpr std::uint32_t XamUserAreUsersFriends = 0x0213;
// XamUserGetSigninInfo (0x0227): previously misdeclared at 0x0182 and never
// registered; corrected here so a future implementation starts from the
// real ordinal.
constexpr std::uint32_t XamUserGetSigninInfo = 0x0227;

// ---------------------------------------------------------------------------
// Input (already implemented in xenon::input::xam::guest; verified against
// xam_table.inc directly - every one of these six ordinals matched the
// reference table exactly, no corrections needed here)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamInputGetCapabilities = 0x0190;
constexpr std::uint32_t XamInputGetState = 0x0191;
constexpr std::uint32_t XamInputSetState = 0x0192;
constexpr std::uint32_t XamInputGetKeystroke = 0x0193;
constexpr std::uint32_t XamInputGetKeystrokeEx = 0x0198;
constexpr std::uint32_t XamInputGetCapabilitiesEx = 0x02AD;

// ---------------------------------------------------------------------------
// Locale, language and time zone (xam_table.inc ~0x3D2, 0x43F, 0x4A9-0x4AB)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamGetLanguage = 0x03D2;
constexpr std::uint32_t XamGetLocale = 0x04A9;
// Renamed from the invented "XamGetTimeZoneInformation": the real xam.xex
// export with this signature (a single output-struct pointer, no
// title/overlapped arguments) is XamQueryTimeZoneInformation. A distinct
// Win32-compatible re-export, plain "GetTimeZoneInformation" (no "Xam"
// prefix, ordinal 0x043F), also exists in xam.xex and is NOT modeled by
// this constant/export - it is a different, unimplemented identity.
// XamSetTimeZoneInformation (0x04AB) is the write-side sibling of this
// export and is also not implemented.
constexpr std::uint32_t XamQueryTimeZoneInformation = 0x04AA;
constexpr std::uint32_t XamSetTimeZoneInformation = 0x04AB;
constexpr std::uint32_t GetTimeZoneInformation = 0x043F;

// ---------------------------------------------------------------------------
// Notifications (xam_table.inc ~0x28A-0x298)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamNotifyCreateListener = 0x028A;
// Renamed from the invented "XamNotifyGetNext"/"XamNotifyPositionUI": the
// real xam.xex exports have no "Xam" prefix at all - they are XNotifyGetNext
// and XNotifyPositionUI (still exported from the "xam" library/module; the
// missing prefix is the real Xbox 360 identity, not a typo). The "Xam"-
// prefixed ordinal 0x028A above (XamNotifyCreateListener) is a distinct,
// separate export that does keep the prefix.
constexpr std::uint32_t XNotifyGetNext = 0x028B;
constexpr std::uint32_t XNotifyPositionUI = 0x028C;

// ---------------------------------------------------------------------------
// Content and storage (xam_table.inc ~0x259-0x269)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamContentCreateEx = 0x0259;
constexpr std::uint32_t XamContentClose = 0x025A;
constexpr std::uint32_t XamContentDelete = 0x025B;
constexpr std::uint32_t XamContentCreateEnumerator = 0x025C;
constexpr std::uint32_t XamContentGetDeviceData = 0x025E;
constexpr std::uint32_t XamContentGetDeviceName = 0x025F;
constexpr std::uint32_t XamContentGetThumbnail = 0x0261;
constexpr std::uint32_t XamContentGetCreator = 0x0262;
constexpr std::uint32_t XamContentGetLicenseMask = 0x0266;
constexpr std::uint32_t XamContentFlush = 0x0267;
constexpr std::uint32_t XamContentOpenFile = 0x0269;

// ---------------------------------------------------------------------------
// System UI (xam_table.inc ~0x2BC-0x2CB); library identity is still "xam" -
// these are xam.xex exports even though some categories below (e.g. content)
// used to (wrongly) claim a lower ordinal range as if "storage device UI"
// were numbered next to content storage calls. It is not; it is grouped
// with the rest of the XamShow* UI family.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamShowSigninUI = 0x02BC;
constexpr std::uint32_t XamShowMessageBoxUI = 0x02CA;
constexpr std::uint32_t XamShowDeviceSelectorUI = 0x02CB;
constexpr std::uint32_t XamShowMarketplaceUI = 0x02C7;

// ---------------------------------------------------------------------------
// Profile enumeration (xam_table.inc ~0x230-0x232)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamProfileCreate = 0x0230;
constexpr std::uint32_t XamProfileCreateEnumerator = 0x0231;
constexpr std::uint32_t XamProfileEnumerate = 0x0232;

// ---------------------------------------------------------------------------
// Achievements and statistics (xam_table.inc 0x2EE, 0x2F7)
//
// Real xam.xex exposes achievement/stat *reading* as enumerator-creation
// calls (XamUserCreateAchievementEnumerator, XamUserCreateStatsEnumerator);
// a caller then walks the resulting enumerator handle. There is no xam.xex
// export for *writing* an achievement or a stat by ordinal under any name -
// on real hardware, unlocking an achievement or updating a stat is done by
// the title writing directly into its cached profile GPD (via the content
// APIs above), not through a dedicated XAM call. XamUserWriteAchievements/
// XamUserReadStats/XamUserWriteStats (previously declared at 0x0280-0x0282)
// were invented ordinals with no real counterpart and have been removed;
// AchievementManager's unlock_achievement()/write_stat() remain available
// as internal Xenon APIs (see xam_session.hpp's achievements() accessor)
// for future GPD-backed writes, but are not exposed as guest XAM exports.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamUserCreateAchievementEnumerator = 0x02EE;
constexpr std::uint32_t XamUserCreateStatsEnumerator = 0x02F7;

// ---------------------------------------------------------------------------
// Title/execution identity (xam_table.inc 0x280)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamGetExecutionId = 0x0280;

}  // namespace ordinal

}  // namespace xenon::xam
