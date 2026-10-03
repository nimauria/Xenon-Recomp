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

// ---------------------------------------------------------------------------
// System information (xam_table.inc 0x282)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamGetSystemVersion = 0x0282;
constexpr std::uint32_t XGetGameRegion = 0x03CC;
// XGetAVPack/XGetLanguage/XGetVideoMode (xam_table.inc 0x3CB/0x3CD/0x3D1) are
// real, separate, non-"Xam"-prefixed xam.xex exports - distinct identities
// from XamGetLanguage (0x3D2, already implemented) and from xboxkrnl's own
// VdQueryVideoMode. Verified against rexglue-sdk's xam_info.cpp/xam_video.cpp
// (itself crediting xenia), which is also where every titled used them
// during early boot to learn the AV cable type and negotiate display mode
// before issuing any video presentation calls.
constexpr std::uint32_t XGetAVPack = 0x03CB;
constexpr std::uint32_t XGetLanguage = 0x03CD;
constexpr std::uint32_t XGetVideoMode = 0x03D1;

// ---------------------------------------------------------------------------
// XAM heap (xam_table.inc 0x1EA/0x1EC) - a kernel-managed heap distinct from
// the title's own CRT heap; backed here by KernelMemory's ordinary guest
// virtual allocator, matching real hardware's "separate heap, same
// allocator primitive" relationship.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamAlloc = 0x01EA;
constexpr std::uint32_t XamFree = 0x01EC;

// ---------------------------------------------------------------------------
// Title loader control (xam_table.inc 0x1A4/0x1A9) - both terminate the
// running title on real hardware (LaunchTitle additionally stages a new
// title to launch next, which Xenon's single-title session does not model).
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamLoaderLaunchTitle = 0x01A4;
constexpr std::uint32_t XamLoaderTerminateTitle = 0x01A9;

// ---------------------------------------------------------------------------
// Voice/headset (xam_table.inc 0x30C-0x30F) - no voice headset hardware is
// modeled, so these report "no device" consistently rather than fabricating
// a working voice channel.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamVoiceCreate = 0x030C;
constexpr std::uint32_t XamVoiceClose = 0x030D;
constexpr std::uint32_t XamVoiceHeadsetPresent = 0x030E;
constexpr std::uint32_t XamVoiceSubmitPacket = 0x030F;

// ---------------------------------------------------------------------------
// XAM background tasks (xam_table.inc 0x1AF/0x1B1/0x1B3) - XamTaskSchedule
// spawns a real guest-executing thread running the given callback, the same
// primitive ExCreateThread uses (see XenonSession::export_xam_task_schedule).
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamTaskSchedule = 0x01AF;
constexpr std::uint32_t XamTaskCloseHandle = 0x01B1;
constexpr std::uint32_t XamTaskShouldExit = 0x01B3;

// ---------------------------------------------------------------------------
// XMsg* app-message IPC (xam_table.inc 0x1F4/0x1F7/0x1F8/0x1FC) - routes
// through a registered in-process "XAM app" dispatcher (dashboard blades,
// content apps). Xenon has no such app registry, so every call resolves to
// the real "app undefined" outcome real hardware itself reports for an
// unregistered app id - not a fabricated success.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XMsgInProcessCall = 0x01F4;
constexpr std::uint32_t XMsgStartIORequest = 0x01F7;
constexpr std::uint32_t XMsgCancelIORequest = 0x01F8;
constexpr std::uint32_t XMsgStartIORequestEx = 0x01FC;

// ---------------------------------------------------------------------------
// Generic enumerator walk (xam_table.inc 0x250) - walks a handle produced by
// one of the XamContentCreateEnumerator/XamUserCreate*Enumerator family.
// Those producers are themselves still fake-handle stubs in this codebase
// (see xam_content_exports.cpp), so this honestly reports "invalid handle"
// for any handle it is actually given, rather than pretending to walk a real
// enumerator that was never backed by real items.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamEnumerate = 0x0250;

// ---------------------------------------------------------------------------
// Session handles (xam_table.inc 0x316/0x317) - Xbox Live multiplayer
// session objects. Xenon implements no session subsystem; matches xenia/
// rexglue-sdk's own identical fixed-sentinel-handle precedent (nothing else
// in any available reference backs these with a real object either).
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamSessionCreateHandle = 0x0316;
constexpr std::uint32_t XamSessionRefObjByHandle = 0x0317;

// ---------------------------------------------------------------------------
// System UI additions (xam_table.inc 0x2BF/0x2C6/0x2D5/0x2DC) - the rest of
// the XamShow* family declared above. No dashboard/overlay UI is
// implemented, so each reports the real "function failed"/"cancelled"
// outcome real hardware gives when the shell UI is unavailable, matching
// XamShowPartyUI/XamShowCommunitySessionsUI's existing precedent rather than
// leaving guest state untouched.
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamShowFriendsUI = 0x02BF;
constexpr std::uint32_t XamShowPlayerReviewUI = 0x02C6;
constexpr std::uint32_t XamShowGamerCardUIForXUID = 0x02D5;
constexpr std::uint32_t XamShowDirtyDiscErrorUI = 0x02D9;
constexpr std::uint32_t XamShowMessageBoxUIEx = 0x02DC;

// ---------------------------------------------------------------------------
// User profile additions (xam_table.inc 0x219) and gamer tile (0x2F0)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamUserReadProfileSettings = 0x0219;
constexpr std::uint32_t XamWriteGamerTile = 0x02F0;

// ---------------------------------------------------------------------------
// Content additions (xam_table.inc 0x260/0x265)
// ---------------------------------------------------------------------------
constexpr std::uint32_t XamContentSetThumbnail = 0x0260;
constexpr std::uint32_t XamContentGetDeviceState = 0x0265;

// ---------------------------------------------------------------------------
// NetDll_* (XNet/Winsock guest ABI, xam_table.inc 0x33-0x34). The generic
// Xbox Live/system-link network stack every title links against through
// xam.xex - distinct from the unrelated, future Xenon Network client. Only
// the small subset actually reachable during offline boot is modeled here;
// see docs/xam/XAM_V1.md for the rest of this ordinal range's status.
// ---------------------------------------------------------------------------
constexpr std::uint32_t NetDll_WSAStartup = 0x0001;
constexpr std::uint32_t NetDll_WSACleanup = 0x0002;
constexpr std::uint32_t NetDll_socket = 0x0003;
constexpr std::uint32_t NetDll_closesocket = 0x0004;
constexpr std::uint32_t NetDll_shutdown = 0x0005;
constexpr std::uint32_t NetDll_ioctlsocket = 0x0006;
constexpr std::uint32_t NetDll_setsockopt = 0x0007;
constexpr std::uint32_t NetDll_getsockopt = 0x0008;
constexpr std::uint32_t NetDll_getsockname = 0x0009;
constexpr std::uint32_t NetDll_listen = 0x000D;
constexpr std::uint32_t NetDll_accept = 0x000E;
constexpr std::uint32_t NetDll_bind = 0x000B;
constexpr std::uint32_t NetDll_connect = 0x000C;
constexpr std::uint32_t NetDll_select = 0x000F;
constexpr std::uint32_t NetDll_recv = 0x0012;
constexpr std::uint32_t NetDll_recvfrom = 0x0014;
constexpr std::uint32_t NetDll_send = 0x0016;
constexpr std::uint32_t NetDll_sendto = 0x0018;
constexpr std::uint32_t NetDll_inet_addr = 0x001A;
constexpr std::uint32_t NetDll_WSAGetLastError = 0x001B;
constexpr std::uint32_t NetDll_WSASetLastError = 0x001C;
constexpr std::uint32_t NetDll___WSAFDIsSet = 0x0022;
constexpr std::uint32_t NetDll_XNetStartup = 0x0033;
constexpr std::uint32_t NetDll_XNetCleanup = 0x0034;
// XNet address/QoS/key helpers (xam_table.inc 0x35-0x49). XNetRandom is a
// genuine local CSPRNG call (fully implementable); the XnAddr<->InAddr
// translators and QoS lookups require a real Xbox Live secure-session layer
// Xenon does not implement, so those report the real "no such service"
// outcome rather than fabricating network data - see xam_net_exports.cpp.
constexpr std::uint32_t NetDll_XNetRandom = 0x0035;
constexpr std::uint32_t NetDll_XNetCreateKey = 0x0036;
constexpr std::uint32_t NetDll_XNetRegisterKey = 0x0037;
constexpr std::uint32_t NetDll_XNetXnAddrToInAddr = 0x0039;
constexpr std::uint32_t NetDll_XNetInAddrToXnAddr = 0x003C;
constexpr std::uint32_t NetDll_XNetQosListen = 0x0045;
constexpr std::uint32_t NetDll_XNetQosLookup = 0x0046;
constexpr std::uint32_t NetDll_XNetQosServiceLookup = 0x0047;
constexpr std::uint32_t NetDll_XNetQosRelease = 0x0048;
constexpr std::uint32_t NetDll_XNetGetTitleXnAddr = 0x0049;

}  // namespace ordinal

}  // namespace xenon::xam
