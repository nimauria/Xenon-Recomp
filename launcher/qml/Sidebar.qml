import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property int currentIndex: 0
    signal pageRequested(int index)

    implicitWidth: 198
    color: Theme.sidebar
    border.width: 1
    border.color: Theme.divider

    readonly property var entries: [
        { title: "Library", glyph: "▣" },
        { title: "Modules", glyph: "◇" },
        { title: "Profiles", glyph: "○" },
        { title: "Settings", glyph: "⚙" }
    ]

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 18
        anchors.bottomMargin: 16
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 7

        Repeater {
            model: root.entries

            delegate: Rectangle {
                required property int index
                required property var modelData

                Layout.fillWidth: true
                Layout.preferredHeight: 52
                radius: 9
                color: root.currentIndex === index ? Theme.accentSoft
                     : navMouse.containsMouse ? Theme.surfaceHover
                     : "transparent"
                border.width: root.currentIndex === index ? 1 : 0
                border.color: Theme.accent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 13

                    Text {
                        text: modelData.glyph
                        color: root.currentIndex === index ? Theme.accent : Theme.textMuted
                        font.pixelSize: 22
                    }

                    Text {
                        Layout.fillWidth: true
                        text: modelData.title
                        color: root.currentIndex === index ? Theme.text : Theme.textMuted
                        font.pixelSize: 15
                        font.weight: root.currentIndex === index ? Font.DemiBold : Font.Normal
                    }
                }

                MouseArea {
                    id: navMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.pageRequested(index)
                }
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                width: 11
                height: 11
                radius: 6
                color: Theme.success
            }

            ColumnLayout {
                spacing: 1

                Text {
                    text: "Xenon Ready"
                    color: Theme.success
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }

                Text {
                    text: launcherBridge.testMode
                          ? "Test mode • front-end ready"
                          : launcherBridge.backendConnected
                            ? "Runtime connected"
                            : "Front-end ready"
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }
        }

        Text {
            text: "v" + launcherBridge.version
            color: Theme.textMuted
            font.pixelSize: 10
        }
    }
}
