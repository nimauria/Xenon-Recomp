// Proves the production core::ExportRegistry - the same registry
// XenonSession::init_exports() populates via XamSession::register_exports()
// - actually resolves XAM exports under the REAL Xbox 360 xam.xex ordinals,
// not Xenon's own (previously wrong) enum constants asserted against
// themselves. Every literal ordinal below is copied directly from the
// authoritative reference tables this correctness pass audited against:
//   - xenia-project/xenia: src/xenia/kernel/xam/xam_table.inc
//   - rexglue/rexglue-sdk: src/kernel/xam/export_table.inc (independent
//     mirror, cross-checked, not merely re-asserting the same source twice)
// A test that only compared xenon::xam::ordinal::XamUserGetXUID against
// itself would keep passing even if the constant, the registration, and the
// test all agreed on the same wrong number - which is exactly how the
// previous ordinals (e.g. XamUserGetXUID registered at 0x0180 instead of
// the real 0x020A) went undetected. Using bare hex literals here means a
// future regression to the old wrong values fails this test even if
// xam_exports.hpp is reverted in lockstep.

#include "xenon/xam/xam_session.hpp"

#include <cassert>
#include <iostream>

#include "xenon/core/export_registry.hpp"
#include "xenon/xam/xam_exports.hpp"

using namespace xenon;

namespace {

void expect_export(const core::ExportRegistry& registry, std::uint32_t ordinal,
                    const char* name) {
  assert(registry.contains("xam", ordinal) &&
         "expected export missing at its real Xbox 360 ordinal");
  const auto* descriptor = registry.resolve("xam", ordinal);
  assert(descriptor != nullptr);
  assert(descriptor->library == "xam" &&
         "XAM exports must be registered under the \"xam\" library identity");
  assert(descriptor->name == name &&
         "export registered at the right ordinal but under the wrong name");
  // Name-based resolution must agree with ordinal-based resolution - a real
  // XEX may import either by ordinal or by name.
  assert(registry.contains("xam", name));
  const auto* by_name = registry.resolve("xam", name);
  assert(by_name != nullptr && by_name->ordinal == ordinal);
}

void test_user_exports_use_real_ordinals() {
  std::cout << "[TEST] User exports registered under real ordinals..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  assert(xam.register_exports(registry));

  expect_export(registry, 0x020Au, "XamUserGetXUID");
  expect_export(registry, 0x0210u, "XamUserGetSigninState");
  expect_export(registry, 0x020Eu, "XamUserGetName");
  expect_export(registry, 0x0212u, "XamUserCheckPrivilege");

  // Regression guard: the old, invented ordinals must not resolve to
  // anything - a real retail XEX importing "xam" ordinal 0x0180 (the
  // formerly-wrong XamUserGetXUID slot) must see an unresolved import, not
  // a coincidentally-still-working stub.
  assert(!registry.contains("xam", 0x0180u));
  assert(!registry.contains("xam", 0x0181u));
  assert(!registry.contains("xam", 0x0183u));
  assert(!registry.contains("xam", 0x0187u));

  std::cout << "  \xE2\x9C\x93 User exports match the real xam.xex ordinal table" << std::endl;
}

void test_locale_exports_use_real_ordinals() {
  std::cout << "[TEST] Locale exports registered under real ordinals..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  assert(xam.register_exports(registry));

  expect_export(registry, 0x03D2u, "XamGetLanguage");
  expect_export(registry, 0x04A9u, "XamGetLocale");
  // Renamed from the invented "XamGetTimeZoneInformation" to its real
  // identity, XamQueryTimeZoneInformation.
  expect_export(registry, 0x04AAu, "XamQueryTimeZoneInformation");

  assert(!registry.contains("xam", 0x0206u));
  assert(!registry.contains("xam", 0x0207u));
  assert(!registry.contains("xam", 0x0208u));
  assert(!registry.contains("xam", "XamGetTimeZoneInformation"));

  std::cout << "  \xE2\x9C\x93 Locale exports match the real xam.xex ordinal table" << std::endl;
}

void test_content_exports_use_real_ordinals() {
  std::cout << "[TEST] Content exports registered under real ordinals..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  assert(xam.register_exports(registry));

  // XamShowDeviceSelectorUI is part of the XamShow* system-UI family, not
  // content storage, despite being implemented alongside ContentManager.
  expect_export(registry, 0x02CBu, "XamShowDeviceSelectorUI");
  expect_export(registry, 0x025Cu, "XamContentCreateEnumerator");
  expect_export(registry, 0x025Au, "XamContentClose");
  expect_export(registry, 0x025Eu, "XamContentGetDeviceData");
  expect_export(registry, 0x025Fu, "XamContentGetDeviceName");

  assert(!registry.contains("xam", 0x0250u));
  assert(!registry.contains("xam", 0x0234u));
  assert(!registry.contains("xam", 0x0237u));
  assert(!registry.contains("xam", 0x0238u));
  assert(!registry.contains("xam", 0x0239u));

  std::cout << "  \xE2\x9C\x93 Content exports match the real xam.xex ordinal table" << std::endl;
}

void test_notification_exports_use_real_ordinals() {
  std::cout << "[TEST] Notification exports registered under real ordinals..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  assert(xam.register_exports(registry));

  expect_export(registry, 0x028Au, "XamNotifyCreateListener");
  // Real xam.xex identities have no "Xam" prefix at all.
  expect_export(registry, 0x028Bu, "XNotifyGetNext");
  expect_export(registry, 0x028Cu, "XNotifyPositionUI");

  // 0x0210 and 0x0212 are legitimately reused for XamUserGetSigninState and
  // XamUserCheckPrivilege respectively (see test_user_exports_use_real_
  // ordinals) - they must NOT still resolve to the old, wrong notification
  // registrations that used to sit at those same numbers.
  assert(registry.resolve("xam", 0x0210u)->name != "XamNotifyCreateListener");
  assert(registry.resolve("xam", 0x0212u)->name != "XamNotifyPositionUI");
  assert(!registry.contains("xam", "XamNotifyGetNext"));
  assert(!registry.contains("xam", "XamNotifyPositionUI"));
  assert(!registry.contains("xam", 0x0211u));

  std::cout << "  \xE2\x9C\x93 Notification exports match the real xam.xex ordinal table" << std::endl;
}

void test_achievement_exports_use_real_ordinals_and_drop_fabricated_ones() {
  std::cout << "[TEST] Achievement/stats exports registered under real ordinals..." << std::endl;

  core::ExportRegistry registry;
  xam::XamSession xam;
  assert(xam.initialize());
  assert(xam.register_exports(registry));

  expect_export(registry, 0x02EEu, "XamUserCreateAchievementEnumerator");
  expect_export(registry, 0x02F7u, "XamUserCreateStatsEnumerator");

  // XamUserWriteAchievements/XamUserReadStats/XamUserWriteStats were
  // invented ordinals with no xam.xex export under any name - they must be
  // gone entirely, not merely renumbered.
  assert(!registry.contains("xam", 0x0280u));
  assert(!registry.contains("xam", 0x0281u));
  assert(!registry.contains("xam", 0x0282u));
  assert(!registry.contains("xam", "XamUserWriteAchievements"));
  assert(!registry.contains("xam", "XamUserReadStats"));
  assert(!registry.contains("xam", "XamUserWriteStats"));
  // The old (wrong) achievement-enumerator ordinal must not still resolve.
  assert(!registry.contains("xam", 0x0284u));

  std::cout << "  \xE2\x9C\x93 Achievement/stats exports match the real xam.xex ordinal table, "
               "fabricated write ordinals removed"
            << std::endl;
}

void test_input_ordinals_unchanged() {
  std::cout << "[TEST] XAM input ordinals verified against the real table (no changes needed)..."
            << std::endl;

  // xenon::input::xam::guest registers directly into cpu::ExternalCallRegistry
  // (bridged into the same production core::ExportRegistry by XenonSession -
  // see tests/core/session_tests.cpp's XamInputGetCapabilities check), so
  // this test only re-confirms the literal ordinals this pass audited
  // against xam_table.inc, all six of which were already correct.
  static_assert(xam::ordinal::XamInputGetCapabilities == 0x0190u);
  static_assert(xam::ordinal::XamInputGetState == 0x0191u);
  static_assert(xam::ordinal::XamInputSetState == 0x0192u);
  static_assert(xam::ordinal::XamInputGetKeystroke == 0x0193u);
  static_assert(xam::ordinal::XamInputGetKeystrokeEx == 0x0198u);
  static_assert(xam::ordinal::XamInputGetCapabilitiesEx == 0x02ADu);

  std::cout << "  \xE2\x9C\x93 Input ordinals confirmed correct" << std::endl;
}

}  // namespace

int main() {
  std::cout << "\n=== XAM Export Ordinal Correctness Tests ===" << std::endl;

  test_user_exports_use_real_ordinals();
  test_locale_exports_use_real_ordinals();
  test_content_exports_use_real_ordinals();
  test_notification_exports_use_real_ordinals();
  test_achievement_exports_use_real_ordinals_and_drop_fabricated_ones();
  test_input_ordinals_unchanged();

  std::cout << "\n\xE2\x9C\x85 All tests passed!" << std::endl;
  return 0;
}
