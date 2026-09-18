import QtQuick
import QtQuick.Layouts

GridLayout {
    id: root

    property string label: ""
    property string value: ""
    property color valueColor: Theme.text
    property int labelWidth: 128
    readonly property bool stacked: width > 0 && (width < 360 || Theme.textScale >= 1.6)

    Layout.fillWidth: true
    columns: stacked ? 1 : 2
    columnSpacing: Theme.spaceSm
    rowSpacing: stacked ? 1 : 0

    Text {
        Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        Layout.fillWidth: root.stacked
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.typeCaption
        elide: Text.ElideRight
    }

    Text {
        Layout.fillWidth: true
        text: root.value
        color: root.valueColor
        font.pixelSize: Theme.typeCaption
        wrapMode: root.stacked ? Text.Wrap : Text.NoWrap
        elide: root.stacked ? Text.ElideNone : Text.ElideRight
    }
}
