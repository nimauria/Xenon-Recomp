import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property string label: ""
    property color tone: Theme.textMuted

    implicitHeight: 26
    implicitWidth: row.implicitWidth + 18
    radius: implicitHeight / 2
    color: Theme.surfaceAlt
    border.width: 1
    border.color: Theme.border

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 7

        Rectangle {
            width: 8
            height: 8
            radius: 4
            color: root.tone
        }

        Text {
            text: root.label
            color: root.tone
            font.pixelSize: 11
            font.weight: Font.Medium
        }
    }
}
