import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    property var recovery: launcherBridge.recoveryState
    readonly property bool safeMode: launcherBridge.safeMode
    readonly property bool automaticSafeMode: Boolean(recovery.automaticSafeMode)
    readonly property int crashCount: Number(recovery.consecutiveUncleanStarts || 0)
    readonly property string automaticReason: String(recovery.automaticSafeModeReason || "")

    modal: true
    width: Math.min(620, parent ? parent.width - Theme.spaceXl * 2 : 620)
    padding: Theme.spaceXl
    closePolicy: Popup.NoAutoClose

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.dialogRadius
        border.width: Math.max(Theme.borderWidth, 1)
        border.color: root.safeMode ? Theme.warning : Theme.border
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            StatusPill {
                label: root.safeMode ? "SAFE MODE" : "RECOVERY"
                tone: Theme.warning
            }
            Text {
                Layout.fillWidth: true
                text: root.safeMode ? "Xenon started in Safe Mode" : "Xenon did not shut down cleanly"
                color: Theme.text
                font.pixelSize: Theme.typeTitle
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.safeMode
                ? (root.automaticSafeMode
                   ? (root.automaticReason === "startup-failure"
                      ? "The previous process ended before Xenon reached its interactive state, so Safe Mode was enabled automatically to avoid repeating the same startup failure. Production profiles, library/module state, runtime services and automatic update checks are not loaded in this session."
                      : "Xenon detected repeated unclean launches and entered Safe Mode automatically to avoid a crash loop. Production profiles, library/module state, runtime services and automatic update checks are not loaded in this session.")
                   : "Safe Mode is active. Production profiles, library/module state, runtime services and automatic update checks are not loaded in this session, so you can inspect settings and diagnostics without immediately recreating the previous startup path.")
                : "The previous launcher process ended without recording a clean shutdown. This can happen after a crash, forced termination or system interruption. Xenon preserved the previous startup log before starting this session."
            color: Theme.textMuted
            font.pixelSize: Theme.typeBody
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }

        XPanel {
            Layout.fillWidth: true
            implicitHeight: details.implicitHeight + Theme.spaceMd * 2
            color: Theme.surfaceAlt
            ColumnLayout {
                id: details
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceXs
                XInfoRow {
                    label: "Last known phase"
                    value: String(root.recovery.previousPhase || "Unknown")
                }
                XInfoRow {
                    label: "Previous launch"
                    value: String(root.recovery.previousStartedAt || "Unknown")
                }
                XInfoRow {
                    visible: root.crashCount > 0
                    label: "Consecutive unclean starts"
                    value: String(root.crashCount)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: String(root.recovery.lastRecoveryLogPath || "").length > 0
            text: "A copy of the previous launcher log has been preserved in the Recovery folder and can be included when diagnosing the issue."
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            XButton {
                text: "Open Recovery Folder"
                onClicked: launcherBridge.openFolder(launcherBridge.recoveryDirectory())
            }

            Item { Layout.fillWidth: true }

            XButton {
                visible: root.safeMode
                text: "Restart Normally"
                onClicked: launcherBridge.restartNormally()
            }

            XButton {
                visible: !root.safeMode
                text: "Restart in Safe Mode"
                variant: "primary"
                onClicked: launcherBridge.restartInSafeMode()
            }

            XButton {
                text: root.safeMode ? "Continue in Safe Mode" : "Continue Normally"
                variant: root.safeMode ? "primary" : "default"
                onClicked: {
                    launcherBridge.acknowledgeRecovery()
                    root.close()
                }
            }
        }
    }
}
