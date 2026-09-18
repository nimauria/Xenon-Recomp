import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property string label: ""
    property color tone: Theme.textMuted

    implicitHeight: 28
    implicitWidth: row.implicitWidth + Theme.spaceLg
    radius: implicitHeight / 2
    color: Theme.surfaceAlt
    border.width: Theme.borderWidth
    border.color: Theme.border

    Accessible.role: Accessible.StaticText
    Accessible.name: root.label

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceSm

        Rectangle {
            width: 8
            height: 8
            radius: 4
            color: root.tone
        }

        Text {
            text: root.label
            color: root.tone
            font.pixelSize: Theme.typeCaption
            font.weight: Font.Medium
        }
    }
}
