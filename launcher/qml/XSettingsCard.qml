import QtQuick
import QtQuick.Layouts

XPanel {
    id: root

    property string title: ""
    property string description: ""
    property int actionWidth: 270
    property bool compact: false
    readonly property bool stacked: width > 0 && (width < 720 || Theme.textScale >= 1.35)
    default property alias actionContent: actionLayout.data

    Layout.fillWidth: true
    implicitHeight: cardLayout.implicitHeight + (compact ? Theme.spaceSm * 2 : Theme.spaceMd * 2)
    color: Theme.surfaceAlt

    GridLayout {
        id: cardLayout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceLg
        anchors.topMargin: root.compact ? Theme.spaceSm : Theme.spaceMd
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.spaceLg
        rowSpacing: root.stacked ? Theme.spaceSm : 0

        ColumnLayout {
            id: textColumn
            Layout.fillWidth: true
            spacing: 3

            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.text
                font.pixelSize: Theme.typeBody
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }

            Text {
                visible: root.description.length > 0
                Layout.fillWidth: true
                text: root.description
                color: Theme.textMuted
                font.pixelSize: Theme.typeCaption
                lineHeight: 1.25
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            id: actionLayout
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.actionWidth
            Layout.minimumWidth: root.stacked ? 0 : Math.min(root.actionWidth, 180)
            Layout.alignment: root.stacked ? Qt.AlignLeft : Qt.AlignVCenter | Qt.AlignRight
            spacing: Theme.spaceSm
        }
    }
}
