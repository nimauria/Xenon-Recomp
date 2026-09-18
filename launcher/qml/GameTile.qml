import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Button {
    id: root

    required property string title
    required property string moduleName
    required property string status
    property url artworkSource: ""
    property bool selected: false
    property bool ready: false
    property bool installed: true

    signal activated()

    readonly property int artworkSize: Math.round(92 * Math.min(Theme.bodyScale, 1.20))
    implicitHeight: Math.max(112, artworkSize + Theme.spaceMd * 2)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    padding: 0
    clip: true

    Accessible.name: title
    Accessible.description: moduleName + ", " + status
    Accessible.role: Accessible.Button

    contentItem: Item {
        implicitHeight: tileContent.implicitHeight + Theme.spaceMd * 2

        RowLayout {
            id: tileContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spaceMd
            spacing: Theme.spaceMd

            ArtworkFrame {
                Layout.preferredWidth: Math.min(root.artworkSize, Math.max(76, root.width * 0.30))
                Layout.preferredHeight: root.artworkSize
                source: root.artworkSource
                fallbackTitle: ""
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumHeight: root.artworkSize
                spacing: 3

                Text {
                    Layout.fillWidth: true
                    text: root.title
                    color: Theme.text
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: root.moduleName
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: Theme.typeCaption
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm

                    Rectangle {
                        width: 9
                        height: 9
                        radius: 5
                        color: root.ready ? Theme.success : root.installed ? Theme.warning : Theme.textMuted
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.status
                        color: root.ready ? Theme.success : root.installed ? Theme.warning : Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    background: Rectangle {
        radius: Theme.panelRadius
        color: root.selected ? Theme.accentSoft : root.hovered ? Theme.surfaceHover : Theme.surface
        border.width: root.activeFocus ? Theme.focusWidth : Theme.borderWidth
        border.color: root.activeFocus ? Theme.focusRing : root.selected ? Theme.accent : Theme.border
    }

    onClicked: activated()
}
