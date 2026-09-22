pragma Singleton

import QtQuick

// Arbitrates which surface currently owns semantic navigation input
// (keyboard Shortcut items in Main.qml and gamepad actions from
// launcherBridge.frontendAction both funnel through the same handlers - see
// Main.qml's handleFrontendAction comment). Qt signals dispatch to every
// connected slot, so a modal dialog that wants to interpret navigation
// actions itself (e.g. AvatarCropEditor's controller support) must make the
// page-level handler step aside instead of both reacting to the same event.
// A simple depth counter (not a single bool) so nested/sequential modals
// compose correctly without one closing early and un-guarding a still-open
// dialog underneath it.
QtObject {
    property int modalDepth: 0
    readonly property bool modalActive: modalDepth > 0

    function pushModal() { modalDepth += 1 }
    function popModal() { modalDepth = Math.max(0, modalDepth - 1) }
}
