import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property alias searchText: searchField.text
    property bool maximized: false

    signal minimizeRequested()
    signal maximizeRequested()
    signal closeRequested()
    signal profilesRequested()

    implicitHeight: 72
    color: Theme.header
    border.width: 1
    border.color: Theme.divider

    MouseArea {
        id: titleDragArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPressed: {
            if (root.Window.window)
                root.Window.window.startSystemMove()
        }
        onDoubleClicked: root.maximizeRequested()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 8
        spacing: 16

        RowLayout {
            Layout.preferredWidth: 360
            spacing: 12

            Item {
                Layout.preferredWidth: 42
                Layout.preferredHeight: 42

                Rectangle {
                    anchors.centerIn: parent
                    width: 8
                    height: 39
                    radius: 4
                    color: Theme.accent
                    rotation: 45
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 8
                    height: 39
                    radius: 4
                    color: Theme.accent
                    rotation: -45
                }
            }

            ColumnLayout {
                spacing: 0

                Text {
                    text: "XENON"
                    color: Theme.text
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                    font.letterSpacing: 4
                }

                Text {
                    text: "PLAY   PRESERVE   REIMAGINE"
                    color: Theme.textMuted
                    font.pixelSize: 8
                    font.letterSpacing: 2.8
                }
            }
        }

        TextField {
            id: searchField
            Layout.fillWidth: true
            Layout.maximumWidth: 680
            Layout.preferredHeight: 42
            placeholderText: "Search games, modules, or content..."
            color: Theme.text
            placeholderTextColor: Theme.textMuted
            selectionColor: Theme.accent
            selectedTextColor: Theme.accentText
            leftPadding: 16
            rightPadding: 16

            background: Rectangle {
                radius: 9
                color: Theme.input
                border.width: 1
                border.color: searchField.activeFocus ? Theme.accent : Theme.border
            }
        }

        Item { Layout.fillWidth: true }

        ComboBox {
            id: profileCombo
            Layout.preferredWidth: 190
            Layout.preferredHeight: 42
            model: [launcherBridge.profileName, "Manage profiles…"]

            onActivated: function(index) {
                if (index === 1) {
                    root.profilesRequested()
                    currentIndex = 0
                }
            }

            contentItem: Text {
                text: "●  " + profileCombo.displayText
                color: Theme.text
                verticalAlignment: Text.AlignVCenter
                leftPadding: 14
                font.pixelSize: 13
                font.weight: Font.Medium
            }

            background: Rectangle {
                radius: 9
                color: Theme.surface
                border.width: 1
                border.color: profileCombo.hovered ? Theme.accent : Theme.border
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 30
            color: Theme.divider
        }

        Repeater {
            model: [
                { glyph: "−", role: "minimize" },
                { glyph: root.maximized ? "❐" : "□", role: "maximize" },
                { glyph: "×", role: "close" }
            ]

            delegate: Rectangle {
                required property var modelData

                Layout.preferredWidth: 44
                Layout.fillHeight: true
                color: buttonMouse.containsMouse
                       ? (modelData.role === "close" ? Theme.danger : Theme.surfaceHover)
                       : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: modelData.glyph
                    color: buttonMouse.containsMouse && modelData.role === "close" ? "white" : Theme.textMuted
                    font.pixelSize: modelData.role === "close" ? 24 : 20
                }

                MouseArea {
                    id: buttonMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (modelData.role === "minimize")
                            root.minimizeRequested()
                        else if (modelData.role === "maximize")
                            root.maximizeRequested()
                        else
                            root.closeRequested()
                    }
                }
            }
        }
    }

}
