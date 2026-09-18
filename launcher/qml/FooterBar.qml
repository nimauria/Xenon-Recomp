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
