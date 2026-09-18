import QtQuick
import QtQuick.Layouts

Rectangle {
    implicitHeight: 28
    color: Theme.header
    border.width: 1
    border.color: Theme.divider

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 16

        Text {
            Layout.fillWidth: true
            text: "Project Xenon  |  A modular runtime for preservation, compatibility, and new possibilities."
            color: Theme.textMuted
            font.pixelSize: 9
            elide: Text.ElideRight
        }

        Text {
            visible: launcherBridge.testMode
            text: "TEST MODE • FICTIONAL UI DATA"
            color: Theme.warning
            font.pixelSize: 9
            font.weight: Font.DemiBold
        }

        Text {
            text: "x86-64 / ARM64   |   Windows / Linux / Android"
            color: Theme.textMuted
            font.pixelSize: 9
        }
    }
}
