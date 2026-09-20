import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property alias searchText: searchField.text
    property bool maximized: false
    property bool compact: false
    property int currentPage: 0

    signal minimizeRequested()
    signal maximizeRequested()
    signal closeRequested()
    signal quickCenterRequested()

    function toggleQuickCenter() {
        if (quickCenter.visible) quickCenter.close()
        else quickCenter.open()
    }

    function closeQuickCenter() { quickCenter.close() }
    function focusSearch() { searchField.forceActiveFocus() }
    function quickCenterVisible() { return quickCenter.visible }

    readonly property int chromeHeight: 36
    readonly property int toolbarHeight: Math.max(60, Theme.controlHeight + 16)

    implicitHeight: chromeHeight + toolbarHeight
    color: Theme.header
    border.width: Theme.borderWidth
    border.color: Theme.divider

    Shortcut {
        sequence: "Ctrl+K"
        onActivated: {
            if (commandPalette.visible) {
                commandPalette.close()
                searchField.clear()
            } else {
                searchField.forceActiveFocus()
                commandPalette.openPalette()
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: {
            searchField.forceActiveFocus()
            commandPalette.openPalette()
        }
    }

    Item {
        id: chromeRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.chromeHeight

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            text: "Xenon Launcher"
            color: Theme.textMuted
            font.pixelSize: Math.max(11, Theme.typeCaption * 0.86)
            font.weight: Font.Medium
        }

        RowLayout {
            id: windowControls
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            spacing: 0
            z: 4

            XWindowButton {
                automationId: "window-minimize"
                kind: "minimize"
                Layout.fillHeight: true
                onClicked: root.minimizeRequested()
            }

            XWindowButton {
                automationId: "window-maximize"
                kind: "maximize"
                maximized: root.maximized
                Layout.fillHeight: true
                onClicked: root.maximizeRequested()
            }

            XWindowButton {
                automationId: "window-close"
                kind: "close"
                Layout.fillHeight: true
                onClicked: root.closeRequested()
            }
        }

        MouseArea {
            id: titleDragArea
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: windowControls.left
            acceptedButtons: Qt.LeftButton
            z: 1
            onPressed: {
                if (root.Window.window)
                    root.Window.window.startSystemMove()
            }
            onDoubleClicked: root.maximizeRequested()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: chromeRow.bottom
        height: 1
        color: Theme.divider
    }

    RowLayout {
        id: toolbarRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: chromeRow.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceXl
        anchors.rightMargin: Theme.spaceXl
        spacing: Theme.spaceMd

        XenonBrand {
            Layout.preferredWidth: root.compact ? 132 : 168
            Layout.preferredHeight: root.compact ? 30 : 34
            asset: "lockup"
            brandColor: Theme.accent
        }

        XTextField {
            id: searchField
            automationId: "global-search"
            Layout.fillWidth: true
            Layout.maximumWidth: 660
            Layout.preferredHeight: Theme.controlHeight
            placeholderText: "Search games, modules, profiles, settings and commands…"
            accessibleName: "Search Xenon"
            accessibleDescription: "Global command palette. Press Control K from anywhere in the launcher."

            onActiveFocusChanged: {
                if (activeFocus) commandPalette.openPalette()
            }
            onTextEdited: commandPalette.openPalette()

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Down) {
                    commandPalette.openPalette()
                    commandPalette.moveSelection(1)
                    event.accepted = true
                } else if (event.key === Qt.Key_Up) {
                    commandPalette.openPalette()
                    commandPalette.moveSelection(-1)
                    event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if (commandPalette.visible && commandPalette.activateSelected()) {
                        searchField.clear()
                        event.accepted = true
                    }
                } else if (event.key === Qt.Key_Escape) {
                    commandPalette.close()
                    searchField.clear()
                    event.accepted = true
                }
            }
        }

        CommandPalette {
            id: commandPalette
            parent: searchField
            x: 0
            y: searchField.height + Theme.spaceXs
            width: Math.max(560, searchField.width)
            query: searchField.text
            onCommandExecuted: {
                searchField.clear()
                root.forceActiveFocus()
            }
            onClosed: {
                if (!searchField.activeFocus) searchField.clear()
            }
        }

        Item { Layout.fillWidth: true }

        Item {
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight

            XIconButton {
                id: notificationButton
                anchors.fill: parent
                iconName: "notification"
                tooltip: launcherBridge.notificationUnreadCount > 0
                         ? "Notifications (" + launcherBridge.notificationUnreadCount + " unread)"
                         : "Notifications"
                variant: notificationPanel.visible ? "filled" : "ghost"
                onClicked: notificationPanel.togglePanel()
            }

            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: -2
                anchors.top: parent.top
                anchors.topMargin: -2
                width: Math.max(18, badgeText.implicitWidth + 8)
                height: 18
                radius: 9
                color: Theme.danger
                border.width: 2
                border.color: Theme.header
                visible: launcherBridge.notificationUnreadCount > 0

                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: launcherBridge.notificationUnreadCount > 99 ? "99+" : String(launcherBridge.notificationUnreadCount)
                    color: "white"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }
            }

            NotificationCenter {
                id: notificationPanel
                parent: notificationButton
                x: notificationButton.width - width
                y: notificationButton.height + Theme.spaceXs
            }
        }

        XIconButton {
            id: helpButton
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight
            iconName: "help"
            tooltip: "Help and keyboard shortcuts"
            variant: "ghost"
            onClicked: helpPopup.visible ? helpPopup.close() : helpPopup.open()

            HelpPopover {
                id: helpPopup
                x: helpButton.width - width
                y: helpButton.height + Theme.spaceXs
            }
        }

        XIconButton {
            id: quickCenterButton
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight
            iconName: "menu"
            tooltip: "Quick Center"
            variant: quickCenter.visible ? "filled" : "ghost"
            onClicked: root.toggleQuickCenter()

            QuickCenter {
                id: quickCenter
                parent: quickCenterButton
                x: quickCenterButton.width - width
                y: quickCenterButton.height + Theme.spaceXs
            }
        }

        ProfileMenu {
            Layout.preferredWidth: root.compact ? 166 : 196
            Layout.preferredHeight: Theme.controlHeight
        }
    }

    Image {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceLg
        width: Math.min(300, parent.width * 0.28)
        height: 18
        source: Theme.decorAsset("divider_notch")
        visible: Theme.decorLevel !== "Minimal" && source.toString().length > 0
        fillMode: Image.PreserveAspectFit
        opacity: 0.36
        smooth: true
    }
}
