import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root

    property string title: ""
    property string description: ""

    Layout.fillWidth: true
    spacing: 4

    Text {
        Layout.fillWidth: true
        text: root.title
        color: Theme.text
        font.pixelSize: Theme.typeSubtitle
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Text {
        visible: root.description.length > 0
        Layout.fillWidth: true
        text: root.description
        color: Theme.textMuted
        font.pixelSize: Theme.typeCaption
        lineHeight: 1.3
        wrapMode: Text.WordWrap
    }
}
