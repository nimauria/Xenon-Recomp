import QtQuick
import QtQuick.Layouts

GridLayout {
    id: root

    property string label: ""
    property string value: ""
    property color valueColor: Theme.text
    property int labelWidth: 128
    readonly property bool stacked: width > 0 && width < Math.round(360 * Math.min(Theme.bodyScale, 1.20))

    Layout.fillWidth: true
    columns: stacked ? 1 : 2
    columnSpacing: Theme.spaceSm
    rowSpacing: stacked ? 2 : 0

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
        wrapMode: Text.WordWrap
        maximumLineCount: root.stacked ? 3 : 2
        elide: Text.ElideRight
    }
}
