import QtQuick
import QtQuick.Layouts

Rectangle {
    implicitHeight: Math.max(30, Theme.typeCaption + Theme.spaceMd * 2)
    color: Theme.header
    border.width: Theme.borderWidth
    border.color: Theme.divider

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceXl
        anchors.rightMargin: Theme.spaceXl
        spacing: Theme.spaceLg

        Text {
            Layout.fillWidth: true
            text: "Project Xenon  •  Modular recompilation runtime and launcher"
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
            elide: Text.ElideRight
        }

        Text {
            readonly property var session: launcherBridge.currentSession
            visible: String(session.state || "idle") !== "idle"
            text: "SESSION • " + String(session.title || session.gameId || "Game") + " • " + String(session.stateLabel || "")
            color: String(session.state || "") === "failed" ? Theme.danger
                : String(session.state || "") === "running" ? Theme.success : Theme.warning
            font.pixelSize: Theme.typeCaption
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.maximumWidth: 420
        }

        Text {
            visible: launcherBridge.testMode
            text: "TEST MODE • FICTIONAL UI DATA"
            color: Theme.warning
            font.pixelSize: Theme.typeCaption
            font.weight: Font.DemiBold
        }

        Text {
            text: launcherBridge.platformName + "  •  " + launcherBridge.hostArchitecture
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
        }
    }
}
