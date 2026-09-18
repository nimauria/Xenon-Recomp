import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    required property string title
    required property string moduleName
    required property string status
    property url artworkSource: ""
    property bool selected: false
    property bool ready: false
    property bool installed: true

    signal activated()

    implicitHeight: 114
    radius: 10
    color: selected ? Theme.accentSoft : (mouseArea.containsMouse ? Theme.surfaceHover : Theme.surface)
    border.width: selected ? 2 : 1
    border.color: selected ? Theme.accent : Theme.border

    RowLayout {
        anchors.fill: parent
        anchors.margins: 9
        spacing: 12

        ArtworkFrame {
            Layout.preferredWidth: 94
            Layout.fillHeight: true
            source: root.artworkSource
            fallbackTitle: "MODULE ART"
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4

            Item { Layout.preferredHeight: 2 }

            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.text
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }

            Text {
                Layout.fillWidth: true
                text: root.moduleName
                color: Theme.textMuted
                elide: Text.ElideRight
                font.pixelSize: 10
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                spacing: 7

                Rectangle {
                    width: 9
                    height: 9
                    radius: 5
                    color: root.ready ? Theme.success : root.installed ? Theme.warning : Theme.textMuted
                }

                Text {
                    text: root.status
                    color: root.ready ? Theme.success : root.installed ? Theme.warning : Theme.textMuted
                    font.pixelSize: 11
                }
            }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.activated()
    }
}
