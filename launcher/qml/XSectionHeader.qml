import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root

    property string title: ""
    property string description: ""
    property bool prominent: false

    Layout.fillWidth: true
    spacing: Theme.spaceXs

    Text {
        Layout.fillWidth: true
        text: root.title
        color: Theme.text
        font.pixelSize: root.prominent ? Theme.typeTitle : Theme.typeSubtitle
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Text {
        visible: root.description.length > 0
        Layout.fillWidth: true
        text: root.description
        color: Theme.textMuted
        font.pixelSize: root.prominent ? Theme.typeBody : Theme.typeCaption
        lineHeight: 1.3
        wrapMode: Text.WordWrap
    }
}
